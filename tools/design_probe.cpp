// design_probe.cpp — 为「环境抑制 / 脚步增强 / 瞬态灵敏度」定语义
//
// 现状两个问题：
//   1) envThrDb 是绝对 dBFS 阈值 → 系统音量、玩家距离一变就失效；
//      抬高它又因为同一公式作用在 band1/2/3，会削掉脚步主体频段。
//   2) stepSens 只是移动宽带瞬态门限 → 选择性随它单调下降，滑杆两端无意义。
//
// 候选方案：
//   ENV_STATIC  环境抑制 = band0(<150) 静态低架 + band1 少量，不设阈值
//                （依据：脚步素材在 125Hz 以下能量为 0，band0 属环境独占区）
//   STEP_RATIO  脚步判据 = 中频(150-3k)瞬态突出度 减去 高频(>3k)瞬态突出度
//                （依据：脚步主体 250Hz-2kHz，枪声主导 2-8kHz）
//
// 方法：每条素材跑完整「分频 → 包络 → 增益」链路（不限幅），素材先归一到
//      -20dBFS 峰值以模拟不同距离/音量。只有归一化后仍成立的判据才是相对的。
//
// 用法: design_probe <wav>[|kind]...
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include "dsp.h"

using namespace sl;

static float g_fs = 48000.f;

static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    if (sz < 44) return false;
    size_t off = 12; int ch = 2, bits = 16; long sr = 48000;
    while (off + 8 <= (size_t)sz) {
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
                      ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
        uint32_t csz = (uint32_t)buf[off+4] | ((uint32_t)buf[off+5]<<8) |
                       ((uint32_t)buf[off+6]<<16) | ((uint32_t)buf[off+7]<<24);
        const unsigned char* p = buf.data() + off + 8;
        if (id == 0x20746D66 && csz >= 16) {
            ch = p[2] | (p[3]<<8);
            sr = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            int frames = (int)(csz / (size_t)(ch * (bits / 8)));
            L.resize((size_t)frames); R.resize((size_t)frames);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i * ch * (bits / 8);
                float a, b;
                if (bits == 32) { int32_t ia, ib; memcpy(&ia,q,4); memcpy(&ib,q+4,4);
                                  a = ia/2147483648.0f; b = ib/2147483648.0f; }
                else             { int16_t ia, ib; memcpy(&ia,q,2); memcpy(&ib,q+2,2);
                                  a = ia/32768.0f; b = ib/32768.0f; }
                L[(size_t)i] = a; R[(size_t)i] = ch > 1 ? b : a;
            }
            g_fs = (float)sr;
            return !L.empty();
        }
        off += 8 + csz + (csz & 1);
    }
    return false;
}

// envMode: 0 = 现状（绝对阈值），1 = 静态低架
// stepMode: 0 = 现状（宽带瞬态门限），1 = 中/高频瞬态突出度对比
struct Cfg { const char* name; float envAmt, stepAmt, sens; int envMode, stepMode; };

struct Result {
    float b[4];        // 各频段增益 dB 的时间平均
    float tot;
    float b2p90;      // band2(600-3k) 增益 dB 的 90 分位（瞬态峰值）
    float b2p50;
    float trMean;     // 脚步触发量均值
    float trFrac;     // stepTr > 0.5 的时间占比 = 触发率
    float tiltMean;   // 低频-中频 谱倾斜 dB 均值
    float tiltVar;    // 谱倾斜标准差
};

static Result run(const Cfg& c, const std::vector<float>& L0, const std::vector<float>& R0) {
    float pk = 0.f;
    for (size_t i = 0; i < L0.size(); ++i)
        pk = std::max(pk, std::max(fabsf(L0[i]), fabsf(R0[i])));
    float norm = (pk > 1e-6f) ? dbToLin(-20.f) / pk : 1.f;
    const float fs = g_fs;

    LR4 lp150, hp150, lp600, hp600, lp3k, hp3k;
    lp150.setLP(fs,150.f); hp150.setHP(fs,150.f);
    lp600.setLP(fs,600.f); hp600.setHP(fs,600.f);
    lp3k.setLP(fs,3000.f); hp3k.setHP(fs,3000.f);
    Biquad ap600, ap3k, ap3kb;
    ap600.setAP(fs,600.f,0.70710678f);
    ap3k.setAP(fs,3000.f,0.70710678f); ap3kb.setAP(fs,3000.f,0.70710678f);

    Env envB[4];
    for (int b = 0; b < 4; ++b) envB[b].set(fs, 5.f, 120.f);
    Env fw, sw;  fw.set(fs,3.f,3.f);   sw.set(fs,150.f,150.f);   // 宽带
    Env fm, sm;  fm.set(fs,3.f,3.f);   sm.set(fs,150.f,150.f);   // 150-3k
    Env fh, sh;  fh.set(fs,3.f,3.f);   sh.set(fs,150.f,150.f);   // >3k

    const float envThrDb = -42.f;                    // 现状默认
    const float sensDbCur = 6.f - 6.f*c.sens;       // 现状映射
    const float biasRel  = 2.f + 10.f*c.sens;       // 候选映射：越大越严

    double sumDb[4] = {0,0,0,0}, sumTot = 0; long nb = 0;
    double sumTr = 0, sumTilt = 0, sumTilt2 = 0; long trHit = 0;
    std::vector<float> g2hist;
    const int kCB = 32;
    const int n0 = (int)L0.size();

    for (int base = 0; base + kCB <= n0; base += kCB) {
        int n = std::min(kCB, n0 - base);
        float bl[4][kCB], br[4][kCB];
        for (int i = 0; i < n; ++i) {
            float l = L0[base+i] * norm, r = R0[base+i] * norm;
            float h1l = hp150.hp(l), a0l = lp150.lp(l);
            float h2l = hp600.hp(h1l), b1l = lp600.lp(h1l);
            a0l = ap3k.proc(ap600.proc(a0l));
            b1l = ap3kb.proc(b1l);
            bl[0][i]=a0l;  br[0][i]=ap3k.proc(ap600.proc(r));
            bl[1][i]=b1l;  br[1][i]=ap3kb.proc(r);
            bl[2][i]=lp3k.lp(h2l); br[2][i]=lp3k.lp(hp600.hp(h1l));
            bl[3][i]=hp3k.hp(h2l); br[3][i]=hp3k.hp(hp600.hp(h1l));
            float m  = 0.5f*(l+r);
            float mM = 0.5f*(bl[1][i]+br[1][i] + bl[2][i]+br[2][i]);
            float mH = 0.5f*(bl[3][i]+br[3][i]);
            fw.proc(m); sw.proc(m);
            fm.proc(mM); sm.proc(mM);
            fh.proc(mH); sh.proc(mH);
            for (int b = 0; b < 4; ++b) envB[b].proc(0.5f*(bl[b][i]+br[b][i]));
        }

        float envCut0 = 0.f, envCut1 = 0.f;
        if (c.envMode == 0) {
            float e0 = linToDb(envB[0].e), e1 = linToDb(envB[1].e);
            float cut = (e0 < envThrDb) ? (e0 - envThrDb)*0.5f : 0.f;
            envCut0 = clampf(-18.f*c.envAmt + clampf(cut, -12.f*c.envAmt, 0.f), -30.f, 0.f);
            float cut1 = (e1 < envThrDb) ? (e1 - envThrDb)*0.5f : 0.f;
            envCut1 = clampf(cut1, -12.f*c.envAmt*0.6f, 0.f);
        } else {
            envCut0 = -20.f * c.envAmt;
            envCut1 = -6.f  * c.envAmt;
        }

        float stepTr;
        if (c.stepMode == 0) {
            float tdb = linToDb(fw.e) - linToDb(sw.e);
            stepTr = clampf((tdb - sensDbCur)/12.f, 0.f, 1.f);
        } else {
            float tM = linToDb(fm.e) - linToDb(sm.e);
            float tH = linToDb(fh.e) - linToDb(sh.e);
            stepTr = clampf((tM - tH - biasRel)/6.f, 0.f, 1.f);
        }

        const float wgt[4] = {0.f, 0.25f, 1.0f, 0.5f};
        float g[4];
        for (int b = 0; b < 4; ++b) {
            float e = linToDb(envB[b].e);
            float gdb = (b == 0) ? envCut0 : (b == 1 ? envCut1 : 0.f);
            float guard = clampf((-6.f - e)/12.f + 1.f, 0.f, 1.f);
            gdb += c.stepAmt * 12.f * wgt[b] * stepTr * guard;
            g[b] = dbToLin(gdb);
            sumDb[b] += linToDb(g[b]);
        }
        sumTot += linToDb(g[0]+g[1]+g[2]+g[3]);
        sumTr += stepTr;
        if (stepTr > 0.5f) ++trHit;
        float tilt = linToDb(envB[0].e) - 0.5f*(linToDb(envB[1].e)+linToDb(envB[2].e));
        sumTilt += tilt; sumTilt2 += (double)tilt*tilt;
        g2hist.push_back(linToDb(g[2]));
        ++nb;
    }
    Result r{};
    for (int b = 0; b < 4; ++b) r.b[b] = nb ? (float)(sumDb[b]/nb) : 0.f;
    r.tot   = nb ? (float)(sumTot/nb) : 0.f;
    r.trMean = nb ? (float)(sumTr/nb) : 0.f;
    r.trFrac = nb ? (float)trHit/nb : 0.f;
    double tm = nb ? sumTilt/nb : 0.0;
    r.tiltMean = (float)tm;
    r.tiltVar  = nb ? (float)std::sqrt(std::max(0.0, sumTilt2/nb - tm*tm)) : 0.f;
    if (!g2hist.empty()) {
        std::sort(g2hist.begin(), g2hist.end());
        r.b2p50 = g2hist[g2hist.size()/2];
        r.b2p90 = g2hist[(size_t)(g2hist.size()*0.90)];
    }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: design_probe <wav>[|kind]...\n"); return 1; }

    struct Job {
        std::string kind, path;
        std::vector<float> L, R;
    };
    std::vector<Job> jobs;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        size_t bar = s.rfind('|');
        Job j;
        if (bar != std::string::npos) { j.kind = s.substr(bar+1); j.path = s.substr(0, bar); }
        else { j.kind = "?"; j.path = s; }
        if (!loadWav(j.path.c_str(), j.L, j.R)) {
            printf("LOAD FAIL %s\n", j.path.c_str());
            continue;
        }
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) { printf("no input\n"); return 1; }

    Cfg cfgs[] = {
        { "现状 envThr+sens",        0.55f, 0.65f, 0.70f, 0, 0 },
        { "候选 静态环境+宽带瞬态",  0.55f, 0.65f, 0.70f, 1, 0 },
        { "现状环境+中频比瞬态",    0.55f, 0.65f, 0.30f, 0, 1 },
        { "候选 静态环境+中频比",    0.55f, 0.65f, 0.30f, 1, 1 },
        { "候选 静态环境+中频比 10",0.55f, 0.65f, 0.10f, 1, 1 },
        { "候选 静态环境+中频比 50",0.55f, 0.65f, 0.50f, 1, 1 },
        { "候选 满档 env=100",       1.00f, 0.65f, 0.30f, 1, 1 },
        { "候选 满档 step=100",      0.55f, 1.00f, 0.30f, 1, 1 },
    };

    static const char* kinds[4] = { "step", "gun", "env", "midsus" };
    for (const Cfg& c : cfgs) {
        printf("\n===== %s  envAmt=%.2f stepAmt=%.2f sens=%.2f =====\n",
               c.name, c.envAmt, c.stepAmt, c.sens);
        printf("%-8s %-22s %7s %7s %7s %7s %8s\n", "kind", "file", "<150", "150-600", "600-3k", ">3k", "total");
        std::map<std::string, std::vector<Result>> byKind;
        for (const Job& j : jobs) {
            Result r = run(c, j.L, j.R);
            const char* base = strrchr(j.path.c_str(), '/');
            printf("%-8s %-22s %7.2f %7.2f %7.2f %7.2f %8.2f\n",
                   j.kind.c_str(), base ? base + 1 : j.path.c_str(),
                   r.b[0], r.b[1], r.b[2], r.b[3], r.tot);
            byKind[j.kind].push_back(r);
        }
        // 按类汇总
        auto mean = [](const std::vector<Result>& v, int b) {
            if (v.empty()) return 0.0;
            double s = 0; for (const Result& r : v) s += r.b[b]; return s / v.size();
        };
        auto meanF = [&](const std::vector<Result>& v, float Result::*f) {
            if (v.empty()) return 0.0;
            double s = 0; for (const Result& r : v) s += r.*f; return s / v.size();
        };
        auto meanTot = [&](const std::vector<Result>& v) { return meanF(v, &Result::tot); };
        printf("  ---\n");
        double m[4][4] = {};
        int n[4] = {};
        for (size_t i = 0; i < 4; ++i) {
            auto it = byKind.find(kinds[i]);
            if (it == byKind.end()) continue;
            n[i] = (int)it->second.size();
            for (int b = 0; b < 4; ++b) m[i][b] = mean(it->second, b);
        }
        for (int i = 0; i < 4; ++i)
            if (n[i]) printf("  %-8s %6d  B2p90=%+6.2f  trFrac=%5.1f%%  trMean=%5.3f  tilt=%+6.1f±%.1f\n",
                            kinds[i], n[i],
                            meanF(byKind[kinds[i]], &Result::b2p90),
                            meanF(byKind[kinds[i]], &Result::trFrac) * 100.0,
                            meanF(byKind[kinds[i]], &Result::trMean),
                            meanF(byKind[kinds[i]], &Result::tiltMean),
                            meanF(byKind[kinds[i]], &Result::tiltVar));
        double stepB2 = m[0][2], envB0 = m[2][0];
        double gunTot = meanTot(byKind["gun"]), midTot = meanTot(byKind["midsus"]);
        double stepP90 = meanF(byKind["step"], &Result::b2p90);
        double gunP90  = meanF(byKind["gun"], &Result::b2p90);
        printf("  >> stepB2mean=%+.2f  envB0=%+.2f  stepP90=%+.2f gunP90=%+.2f  P90sep=%+.2f\n",
               stepB2, envB0, stepP90, gunP90, stepP90 - gunP90);
    }
    return 0;
}