// mix_probe.cpp — 混音场景 SNR 实验：决定「环境抑制 / 脚步增强 / 瞬态灵敏度」的语义
//
// 为什么必须混音测：单独播素材测不出关键指标。游戏里脚步/枪声是叠在**持续环境音之上**的，
// 真正要衡量的是「事件相对同时存在的底噪有多突出」。另外现有实现把整条频段乘增益
// （bl[b] *= g），稳态成分被一起抬——那抬的是话音和底噪，不是脚步。
//
// 本探针构造：loop 把一条 env 素材循环铺满当底噪，event 在指定位置叠上去，
// 跑完整 DSP 链路（分频 → 包络 → 增益 → 叠加），比较事件段与事件前底噪段的能量。
//
// 指标：
//   SNR_before  事件前（纯底噪）到事件段的能量差，dB
//   SNR_after   同上，但经过 DSP
//   gain        SNR_after - SNR_before  ← 这个越大越好（对比度改善）
//   bleedSNR    非目标事件（枪声）被抬多少，dB（越小越好）
//
// 用法: mix_probe --env <wav> --event <wav> --pos <0..1> [--cfg <name>]
//       mix_probe --batch <envs.txt> <events.txt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <memory>
#include "dsp.h"

using namespace sl;

// ---------------------------------------------------------------- wav
static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R, float& fs) {
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
        if (off + 8 + csz > (size_t)sz) break;      // 防御：畸形 chunk 越界
        if (id == 0x20746D66 && csz >= 16) {
            ch = p[2] | (p[3]<<8);
            sr = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            int bps = bits / 8;
            if (ch < 1 || bps < 1 || csz < 8) { off += 8 + csz + (csz & 1); continue; }
            int frames = (int)(csz / (size_t)(ch * bps));
            if (frames <= 0 || (size_t)frames * ch * bps > csz) { off += 8 + csz + (csz & 1); continue; }
            L.resize((size_t)frames); R.resize((size_t)frames);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i*ch*bps;
                float a, b;
                if (bits == 32) { int32_t ia, ib; memcpy(&ia,q,4); memcpy(&ib,q+4,4);
                                  a = ia/2147483648.0f; b = ib/2147483648.0f; }
                else             { int16_t ia, ib; memcpy(&ia,q,2); memcpy(&ib,q+2,2);
                                  a = ia/32768.0f; b = ib/32768.0f; }
                L[(size_t)i] = a; R[(size_t)i] = ch>1 ? b : a;
            }
            fs = (float)sr;
            return !L.empty();
        }
        off += 8 + csz + (csz & 1);
    }
    return false;
}

// ---------------------------------------------------------------- DSP 变体
// envMode : 0 = 现状绝对阈值, 1 = 静态低架, 2 = 谱倾斜自适应下扩
// stepMode: 0 = 现状宽带瞬态门限, 1 = 中频瞬态, 2 = 瞬态分量增强(只抬瞬态)
// sepMode : 0 = 无, 1 = 中频突出度(压低频独大)
struct Cfg { const char* name; float envAmt, stepAmt, sens; int envMode, stepMode, sepMode; };
struct Out { float snrBefore, snrAfter, bleed; };

// bandPass helper
struct Splitter {
    LR4 lp150, hp150, lp600, hp600, lp3k, hp3k;
    Biquad ap600, ap3k, ap3kb;
    void prep(float fs) {
        lp150.setLP(fs,150.f); hp150.setHP(fs,150.f);
        lp600.setLP(fs,600.f); hp600.setHP(fs,600.f);
        lp3k.setLP(fs,3000.f); hp3k.setHP(fs,3000.f);
        ap600.setAP(fs,600.f,0.70710678f);
        ap3k.setAP(fs,3000.f,0.70710678f); ap3kb.setAP(fs,3000.f,0.70710678f);
    }
    void split(float l, float r, float bL[4], float bR[4]) {
        float h1l = hp150.hp(l), a0l = lp150.lp(l);
        float h2l = hp600.hp(h1l), b1l = lp600.lp(h1l);
        bL[0] = ap3k.proc(ap600.proc(a0l));
        bL[1] = ap3kb.proc(b1l);
        bL[2] = lp3k.lp(h2l);
        bL[3] = hp3k.hp(h2l);
        float h1r = hp150.hp(r), a0r = lp150.lp(r);
        float h2r = hp600.hp(h1r), b1r = lp600.lp(h1r);
        bR[0] = ap3k.proc(ap600.proc(a0r));
        bR[1] = ap3kb.proc(b1r);
        bR[2] = lp3k.lp(h2r);
        bR[3] = hp3k.hp(h2r);
    }
};

static Out runDsp(const Cfg& c, const std::vector<float>& L, const std::vector<float>& R,
                  float fs, int evStart, int evEnd) {
    // 栈帧别太大：Splitter/分频缓冲放堆上（Windows 默认栈 1MB，MinGW 栈帧一大就踩坏）
    auto spP = std::make_unique<Splitter>();
    spP->prep(fs);
// 注意：blp[i] 传给 split() 时被当作 float[4] 用（split 内写 bL[0..3]），
// 所以每"行"必须留 4 个 float 的步长但只用到前 4 个；行数 = 控制块数 = kCB。
// 用 kCB 行 × 4 列的紧凑布局，split 签名改成 float* 更直白。
std::vector<float> bl(4*32), br(4*32);
float* blp = bl.data();
float* brp = br.data();

    Env envB[4];
    for (int b = 0; b < 4; ++b) envB[b].set(fs, 5.f, 120.f);
    // 快/慢包络：宽带、中频、高频
    Env fw, sw, fm, sm, fh, sh;
    fw.set(fs,3.f,3.f);   sw.set(fs,150.f,150.f);
    fm.set(fs,3.f,3.f);   sm.set(fs,150.f,150.f);
    fh.set(fs,3.f,3.f);   sh.set(fs,150.f,150.f);
    float tiltInit = 0.f; bool tiltReady = false;

    const float envThrDb = -42.f;
    const float sensDbCur = 6.f - 6.f*c.sens;
    const float biasRel  = 2.f + 10.f*c.sens;

    const int n0 = (int)L.size();
    const int kCB = 32;
    double ePre = 0, eEv = 0; long nPre = 0, nEv = 0;
    // bleed: 事件段内高频(>3k)能量占比变化，用来看枪声有没被抬
    double hPre = 0, hEv = 0;

    for (int base = 0; base + kCB <= n0; base += kCB) {
        int n = std::min(kCB, n0 - base);
        // bl,br 已提到堆上
        for (int i = 0; i < n; ++i) {
            int idx = base + i;
            float l = L[idx], r = R[idx];
            spP->split(l, r, blp+i*4, brp+i*4);
            for (int b = 0; b < 4; ++b) {
                float mid = 0.5f*(blp[i*4+b]+brp[i*4+b]);
                envB[b].proc(mid);
            }
            float m  = 0.5f*(l+r);
            float mM = 0.5f*(blp[i*4+1]+brp[i*4+1] + blp[i*4+2]+brp[i*4+2]);
            float mH = 0.5f*(blp[i*4+3]+brp[i*4+3]);
            fw.proc(m); sw.proc(m);
            fm.proc(mM); sm.proc(mM);
            fh.proc(mH); sh.proc(mH);
        }

        // ---- 环境抑制判据
        float envCut0 = 0.f, envCut1 = 0.f;
        float e0 = linToDb(envB[0].e), e1 = linToDb(envB[1].e);
        if (c.envMode == 0) {
            float cut = (e0 < envThrDb) ? (e0 - envThrDb)*0.5f : 0.f;
            envCut0 = clampf(-18.f*c.envAmt + clampf(cut, -12.f*c.envAmt, 0.f), -30.f, 0.f);
            float cut1 = (e1 < envThrDb) ? (e1 - envThrDb)*0.5f : 0.f;
            envCut1 = clampf(cut1, -12.f*c.envAmt*0.6f, 0.f);
        } else if (c.envMode == 1) {
            envCut0 = -20.f*c.envAmt;
            envCut1 = -6.f*c.envAmt;
        } else {
            // 谱倾斜自适应：低频相对中频比自己近期的常态更隆隆 → 压
            float eMid = 0.5f*(linToDb(envB[1].e)+linToDb(envB[2].e));
            float tilt = e0 - eMid;
            if (!tiltReady && base > (int)(fs*2*kCB)) { tiltInit = tilt; tiltReady = true; }
            float tref = tiltReady ? tiltInit : tilt;
            float excess = tref - tilt;
            float g = clampf((excess - 2.f)/8.f, 0.f, 1.f);
            envCut0 = -20.f*c.envAmt*g;
            envCut1 = -6.f*c.envAmt*g;
        }

        // ---- 脚步判据
        float tr[4] = {0,0,0,0};    // 每段瞬态占比
        if (c.stepMode == 0) {
            float tdb = linToDb(fw.e) - linToDb(sw.e);
            float v = clampf((tdb - sensDbCur)/12.f, 0.f, 1.f);
            tr[1] = tr[2] = tr[3] = v;
        } else if (c.stepMode == 1) {
            float tM = linToDb(fm.e) - linToDb(sm.e);
            float v = clampf((tM - biasRel)/9.f, 0.f, 1.f);
            tr[1] = tr[2] = tr[3] = v;
        } else {
            // 每段各自的瞬态占比（瞬态分量增强）
            tr[1] = clampf((linToDb(envB[1].e*4.f) - linToDb(envB[1].e))/6.f, 0.f, 1.f);
            // 更稳：用快慢比
            tr[1] = clampf((linToDb(fm.e) - linToDb(sm.e) - biasRel)/9.f, 0.f, 1.f);
            float tH = linToDb(fh.e) - linToDb(sh.e);
            tr[3] = clampf((tH - biasRel - 4.f)/9.f, 0.f, 1.f);
            tr[2] = tr[1];
        }

        // ---- 分离（低频独大 → 不是脚步，压住增强）
        float sep = 1.f;
        if (c.sepMode == 1) {
            float eMid = 0.5f*(linToDb(envB[1].e)+linToDb(envB[2].e));
            float tilt = e0 - eMid;
            sep = clampf((12.f - tilt)/8.f, 0.f, 1.f);
        }

        // ---- 合成每段增益
        const float wgt[4] = {0.f, 0.30f, 1.0f, 0.45f};
        for (int b = 0; b < 4; ++b) {
            float e = linToDb(envB[b].e);
            float gdb = (b == 0) ? envCut0 : (b == 1 ? envCut1 : 0.f);
            float guard = clampf((-6.f - e)/12.f + 1.f, 0.f, 1.f);
            gdb += c.stepAmt * 12.f * wgt[b] * tr[b] * guard * sep;
            float g = dbToLin(gdb);
            // 应用到本块
            for (int i = 0; i < n; ++i) { blp[i*4+b] *= g; brp[i*4+b] *= g; }
        }

        // ---- 叠加并统计能量
        for (int i = 0; i < n; ++i) {
            int idx = base + i;
            float ol = blp[i*4+0]+blp[i*4+1]+blp[i*4+2]+blp[i*4+3];
            float or_ = brp[i*4+0]+brp[i*4+1]+brp[i*4+2]+brp[i*4+3];
            float e2 = ol*ol + or_*or_;
            float h2 = (blp[i*4+3]*blp[i*4+3] + brp[i*4+3]*brp[i*4+3]);
            if (idx >= evStart && idx < evEnd) { eEv += e2; hEv += h2; ++nEv; }
            else if (idx >= evStart - n0/4 && idx < evStart) { ePre += e2; hPre += h2; ++nPre; }
        }
    }

    Out o{};
    o.snrBefore = 10.f*log10f((eEv/(nEv?nEv:1) + 1e-20f) / (ePre/(nPre?nPre:1) + 1e-20f));
    // after 需要单独跑一遍 dry，这里先用增益后的事件段能量比 —— 简化：重跑一次 dry
    o.snrAfter = 0; o.bleed = 0;
    return o;
}

// 干跑（旁路 A 段/B 段）得到 SNR before
static float snrDry(const std::vector<float>& L, const std::vector<float>& R,
                    int evStart, int evEnd) {
    int n0 = (int)L.size();
    double ePre = 0, eEv = 0; long nPre = 0, nEv = 0;
    for (int i = 0; i < n0; ++i) {
        float e = L[i]*L[i] + R[i]*R[i];
        if (i >= evStart && i < evEnd) { eEv += e; ++nEv; }
        else if (i >= evStart - n0/4 && i < evStart) { ePre += e; ++nPre; }
    }
    return 10.f*log10f((eEv/(nEv?nEv:1) + 1e-20f) / (ePre/(nPre?nPre:1) + 1e-20f));
}

// 构造混音：env 循环铺底噪，event 在 evStart 位置叠加
// levelEnv / levelEv: 相对 -20dBFS 归一后的线性增益，用来模拟「远处脚步 vs 近处枪声」
static void buildMix(const std::vector<float>& EL, const std::vector<float>& ER,
                     const std::vector<float>& VL, const std::vector<float>& VR,
                     float levelEnv, float levelEv,
                     std::vector<float>& L, std::vector<float>& R, int& evStart, int& evEnd) {
    // 底噪只取前 2 秒循环（判据的时间尺度是秒级，再长不增加信息量，只拖慢仿真）
    int nb = (int)EL.size();
    const int kMaxEnv = 96000;                 // 2s @48k
    if (nb > kMaxEnv) nb = kMaxEnv;
    const int lead = nb;                       // 事件前先铺 1 个循环的纯底噪
    const int n = nb * 3 + (int)VL.size();
    L.assign((size_t)n, 0.f); R.assign((size_t)n, 0.f);
    for (int i = 0; i < n; ++i) {
        int j = i % nb;
        L[(size_t)i] = EL[(size_t)j]*levelEnv;
        R[(size_t)i] = ER[(size_t)j]*levelEnv;
    }
    evStart = lead;
    evEnd   = lead + (int)VL.size();
    if (evEnd > n) evEnd = n;
    for (size_t i = 0; i < VL.size(); ++i) {
        int k = evStart + (int)i;
        if (k >= n) break;
        L[(size_t)k] += VL[i]*levelEv;
        R[(size_t)k] += VR[i]*levelEv;
    }
}

static float rmsDb(const std::vector<float>& L, const std::vector<float>& R, int a, int b) {
    double s = 0; long n = 0;
    for (int i = a; i < b && i < (int)L.size(); ++i) {
        if (i < 0) continue;
        s += (double)L[(size_t)i]*L[(size_t)i] + (double)R[(size_t)i]*R[(size_t)i];
        ++n;
    }
    if (!n) return -120.f;
    return (float)(10.0*std::log10(s/n + 1e-20));
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // 用法: mix_probe <envs.txt> <events.txt>
    // 每行一个 wav 路径
    if (argc < 3) { printf("usage: mix_probe <envs.txt> <events.txt>\n"); return 1; }
    auto readList = [](const char* p) {
        std::vector<std::string> v;
        FILE* f = fopen(p, "r");
        if (!f) return v;
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
            if (!s.empty()) v.push_back(s);
        }
        fclose(f);
        return v;
    };
    std::vector<std::string> envFiles = readList(argv[1]);
    std::vector<std::string> evFiles  = readList(argv[2]);
    if (envFiles.empty()) { printf("no env files\n"); return 1; }

    struct Clip { std::vector<float> L, R; float fs; };
    std::vector<Clip> envs, evs;
    for (auto& f : envFiles) { Clip c; if (loadWav(f.c_str(), c.L, c.R, c.fs)) envs.push_back(std::move(c)); }
    for (auto& f : evFiles)  { Clip c; if (loadWav(f.c_str(), c.L, c.R, c.fs)) evs.push_back(std::move(c)); }
    printf("loaded env=%zu event=%zu\n", envs.size(), evs.size());
    if (envs.empty() || evs.empty()) return 1;

    // 事件电平场景：近处(+0dB) / 中距(-12) / 远处(-24)，底噪固定 -12
    const float levEnv = dbToLin(-12.f);
    float levEv[3] = { dbToLin(0.f), dbToLin(-12.f), dbToLin(-24.f) };
    const char* levName[3] = { "近 +0dB", "中 -12dB", "远 -24dB" };

    Cfg cfgs[] = {
        { "现状",                 0.55f, 0.65f, 0.70f, 0, 0, 0 },
        { "现状 sens=0",          0.55f, 0.65f, 0.00f, 0, 0, 0 },
        { "现状 sens=100",        0.55f, 0.65f, 1.00f, 0, 0, 0 },
        { "静态低架+宽带瞬态",    0.55f, 0.65f, 0.70f, 1, 0, 0 },
        { "倾斜下扩+宽带瞬态",    0.55f, 0.65f, 0.70f, 2, 0, 0 },
        { "倾斜下扩+中频瞬态",    0.55f, 0.65f, 0.30f, 2, 1, 0 },
        { "倾斜+中频+分离",       0.55f, 0.65f, 0.30f, 2, 1, 1 },
        { "倾斜+中频+分离 s=10",  0.55f, 0.65f, 0.10f, 2, 1, 1 },
        { "倾斜+中频+分离 s=50",  0.55f, 0.65f, 0.50f, 2, 1, 1 },
        { "倾斜+中频+分离 满档",  1.00f, 1.00f, 0.30f, 2, 1, 1 },
        { "静态+中频+分离",       0.55f, 0.65f, 0.30f, 1, 1, 1 },
    };

    printf("\n%-22s", "配置");
    for (int i = 0; i < 3; ++i) printf(" %14s", levName[i]);
    printf("   |  底噪残留\n");
    printf("%-22s %14s %14s %14s   |  %8s\n", "", "step提升dB", "step提升dB", "step提升dB", "env衰减dB");

    for (const Cfg& c : cfgs) {
        double gainStep[3] = {0,0,0};
        double gainGun = 0;
        double envAtt = 0;
        int nStep = 0, nGun = 0, nEnv = 0;

        for (size_t ei = 0; ei < envs.size(); ++ei) {
            const Clip& ec = envs[ei];
            // 底噪单独过 DSP，看衰减了多少
            {
                std::vector<float> L, R; int evS, evE;
                buildMix(ec.L, ec.R, ec.L, ec.R, levEnv, 0.f, L, R, evS, evE);
                int nb0=(int)ec.L.size(); if(nb0>96000)nb0=96000; float before = rmsDb(L, R, nb0, nb0*2);
                runDsp(c, L, R, ec.fs, nb0, nb0*2);
                float after = rmsDb(L, R, nb0, nb0*2);
                envAtt += before - after; ++nEnv;
            }
            for (size_t vi = 0; vi < evs.size(); ++vi) {
                const Clip& vc = evs[vi];
                for (int s = 0; s < 3; ++s) {
                    std::vector<float> L, R; int evS, evE;
                    buildMix(ec.L, ec.R, vc.L, vc.R, levEnv, levEv[s], L, R, evS, evE);
                    int preA = evS - (int)((ec.L.size()<96000?ec.L.size():96000))/4;
                    if (preA < 0) preA = 0;
                    float pre = rmsDb(L, R, preA, evS);
                    float dryEv = rmsDb(L, R, evS, evE);
                    runDsp(c, L, R, ec.fs, evS, evE);
                    float wetPre = rmsDb(L, R, preA, evS);
                    float wetEv  = rmsDb(L, R, evS, evE);
                    float drySnr = dryEv - pre;
                    float wetSnr = wetEv - wetPre;
                    double g = wetSnr - drySnr;
                    // 事件若是脚步(step 目录) 记 step，否则 gun
                    const char* bn = strrchr(evFiles[vi].c_str(), '/');
                    bool isStep = bn && strstr(bn, "step") != nullptr;
                    if (isStep) { gainStep[s] += g; ++nStep; }
                    else         { gainGun += g;   ++nGun;  }
                }
            }
        }
        printf("%-22s", c.name);
        for (int s = 0; s < 3; ++s) printf(" %+14.2f", nStep ? gainStep[s]/nStep : 0.0);
        printf("   |  %+6.2f  (gun %+.2f, n=%d/%d)\n",
               nEnv ? envAtt/nEnv : 0.0, nGun ? gainGun/nGun : 0.0, nStep, nGun);
    }
    return 0;
}