// probe_limiter.cpp — 「锁定最大输出」在枪声+尾音期间对脚步/低频做了什么
//
// 关键设计：把限幅器的影响单独分离出来 —— bOn=1 与 bOn=0 两条链路使用**完全相同**的
// aOn/env/step 参数，两者相减即为「限幅带来的增益变化」，不含环境抑制的静态衰减。
//
// A. 增益曲线：枪声期间整条信号被压多少 dB、持续多久、脚步落在其中时被压多少
// B. 频段一致性：1.2 kHz 窄带信号与全带的衰减差 —— 相等 => 全带统一压，无新增频段掩蔽
// C. 枪声听感：峰值对齐后测起音/尾音包络
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;
static const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ wav in
static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R, float& fs) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open fail: %s\n", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    if (sz < 44) return false;
    size_t off = 12; int ch = 2, bits = 16, fmt = 3; long sr = 48000;
    while (off + 8 <= (size_t)sz) {
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
                      ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
        uint32_t csz = (uint32_t)buf[off+4] | ((uint32_t)buf[off+5]<<8) |
                       ((uint32_t)buf[off+6]<<16) | ((uint32_t)buf[off+7]<<24);
        const unsigned char* p = buf.data() + off + 8;
        if (off + 8 + (size_t)csz > (size_t)sz) break;
        if (id == 0x20746D66 && csz >= 16) {
            fmt = p[0] | (p[1]<<8);
            ch  = p[2] | (p[3]<<8);
            sr  = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            int bps = bits / 8;
            if (ch < 1 || bps < 1 || csz < 4) { off += 8 + csz + (csz & 1); continue; }
            int frames = (int)(csz / (size_t)(ch * bps));
            L.assign((size_t)frames, 0.f); R.assign((size_t)frames, 0.f);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i * ch * bps;
                float v[2] = {0.f, 0.f};
                for (int c = 0; c < (ch < 2 ? 1 : 2); ++c) {
                    const unsigned char* r = q + c * bps;
                    float x;
                    if (fmt == 3 && bps == 4) x = *(const float*)r;
                    else if (fmt == 3 && bps == 8) x = (float)*(const double*)r;
                    else if (bps == 2) x = *(const int16_t*)r / 32768.0f;
                    else if (bps == 3) x = (float)((int32_t)(r[0] | (r[1]<<8) | ((int8_t)r[2]<<16))) / 8388608.0f;
                    else x = (float)(*(const int32_t*)r) / 2147483648.0f;
                    v[c] = x;
                }
                if (ch < 2) v[1] = v[0];
                L[(size_t)i] = v[0]; R[(size_t)i] = v[1];
            }
            fs = (float)sr;
            return true;
        }
        off += 8 + (size_t)csz + (csz & 1);
    }
    return false;
}

static void resample(std::vector<float>& L, std::vector<float>& R, float fsIn, float fsOut) {
    if (fabsf(fsIn - fsOut) < 1.f) return;
    size_t nIn = L.size();
    size_t nOut = (size_t)((double)nIn * fsOut / fsIn);
    std::vector<float> l(nOut), r(nOut);
    double step = (double)fsIn / fsOut;
    for (size_t i = 0; i < nOut; ++i) {
        double x = i * step; size_t i0 = (size_t)x;
        if (i0 >= nIn) i0 = nIn - 1;
        size_t i1 = std::min(i0 + 1, nIn - 1);
        double fr = x - (double)i0;
        l[i] = (float)(L[i0] + (L[i1] - L[i0]) * fr);
        r[i] = (float)(R[i0] + (R[i1] - R[i0]) * fr);
    }
    L.swap(l); R.swap(r);
}

// 素材前段常带静音，测量窗口必须对准起音
static void trimToOnset(std::vector<float>& L, std::vector<float>& R) {
    float pk = 0.f;
    for (float v : L) pk = std::max(pk, fabsf(v));
    if (pk <= 0.f) return;
    const float thr = pk * 0.10f;                 // -20 dB：跳过低电平前导
    size_t at = 0;
    for (size_t i = 0; i < L.size(); ++i) if (fabsf(L[i]) >= thr) { at = i; break; }
    size_t back = (size_t)(48000 * 0.002);
    at = at > back ? at - back : 0;
    L.erase(L.begin(), L.begin() + (long)at);
    R.erase(R.begin(), R.begin() + (long)at);
}

// ------------------------------------------------------------------ 窄带滤波（证明全带统一压）
struct Bp {
    float b0=0,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    void set(float fs, float f, float q) {
        float w0 = 6.28318530718f*f/fs, cw = cosf(w0), sw = sinf(w0);
        float al = sw/(2*q), a0 = 1+al;
        b0 = al/a0; b1 = 0; b2 = -al/a0; a1 = -2*cw/a0; a2 = (1-al)/a0;
    }
    float proc(float x) { float y = b0*x + z1; z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y; return y; }
};

static double rmsDb(const std::vector<float>& x, size_t from, size_t n) {
    double s = 0; size_t c = 0;
    for (size_t i = from; i < from + n && i < x.size(); ++i) { s += (double)x[i]*x[i]; ++c; }
    return c ? 10.0*log10(s/(double)c + 1e-20) : -200.0;
}
static double peakOf(const std::vector<float>& x, size_t from, size_t n) {
    double p = 0;
    for (size_t i = from; i < from + n && i < x.size(); ++i) p = std::max(p, (double)fabsf(x[i]));
    return p;
}

struct Mix {
    std::vector<float> L, R;
    size_t gunAt = 0, gunEnd = 0;
    std::vector<size_t> stepAt;
};

static void addAt(Mix& m, const std::vector<float>& L, const std::vector<float>& R,
                  size_t at, float gain) {
    for (size_t i = 0; i < L.size(); ++i) {
        if (at + i >= m.L.size()) break;
        m.L[at+i] += L[i]*gain;
        m.R[at+i] += R[i]*gain;
    }
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "C:/Users/Administrator/AppData/Local/Temp/sl";
    char path[1024];
    std::vector<float> gL, gR, sL, sR, aL, aR; float fs = 48000.f;

    snprintf(path, sizeof(path), "%s/w3144728661.wav", dir);
    if (!loadWav(path, gL, gR, fs)) return 1; resample(gL, gR, fs, 48000.f);
    snprintf(path, sizeof(path), "%s/w2522402869.wav", dir);
    std::vector<float> g2L, g2R; float fs2 = 48000.f;
    bool hasLong = loadWav(path, g2L, g2R, fs2);
    if (hasLong) resample(g2L, g2R, fs2, 48000.f);

    snprintf(path, sizeof(path), "%s/w2253900037.wav", dir);
    if (!loadWav(path, sL, sR, fs)) return 1; resample(sL, sR, fs, 48000.f);
    snprintf(path, sizeof(path), "%s/w2833643493.wav", dir);
    if (!loadWav(path, aL, aR, fs)) return 1; resample(aL, aR, fs, 48000.f);

    trimToOnset(gL, gR); trimToOnset(sL, sR);
    if (hasLong) trimToOnset(g2L, g2R);

    const double fsr = 48000.0;
    const size_t total = (size_t)(fsr * 7.0);

    auto buildMix = [&](const std::vector<float>& shotL, const std::vector<float>& shotR) {
        Mix m; m.L.assign(total, 0.f); m.R.assign(total, 0.f);
        float apk = 0.f; for (float v : aL) apk = std::max(apk, fabsf(v));
        float ag = dbToLin(-18.f) / std::max(apk, 1e-6f);
        for (size_t t = 0; t < total; t += aL.size()) addAt(m, aL, aR, t, ag);
        m.gunAt = (size_t)(fsr * 1.0);
        addAt(m, shotL, shotR, m.gunAt, 1.0f);
        m.gunEnd = std::min(total, m.gunAt + shotL.size());
        float spk = 0.f; for (float v : sL) spk = std::max(spk, fabsf(v));
        float sg = dbToLin(-20.f) / std::max(spk, 1e-6f);
        for (size_t t = (size_t)(fsr*0.2); t + sL.size() < total; t += (size_t)(fsr*0.4)) {
            addAt(m, sL, sR, t, sg); m.stepAt.push_back(t);
        }
        return m;
    };

    auto runDsp = [&](const Mix& in, DspParams p, std::vector<float>& oL) {
        std::vector<float> io(in.L.size()*2);
        for (size_t i = 0; i < in.L.size(); ++i) { io[2*i] = in.L[i]; io[2*i+1] = in.R[i]; }
        DspCore d; d.Prepare(48000.f); d.SetParams(p);
        d.Process(io.data(), (int)(io.size()/2));
        oL.resize(in.L.size());
        for (size_t i = 0; i < oL.size(); ++i) oL[i] = io[2*i];
    };

    const size_t hop = 480;   // 10 ms
    const size_t W   = 480;   // 10 ms 能量窗

    printf("素材长度：枪声A %.2f s / 枪声B %.2f s / 脚步 %.2f s / 环境 %.2f s\n",
           gL.size()/fsr, hasLong ? g2L.size()/fsr : 0.0, sL.size()/fsr, aL.size()/fsr);
    printf("场景：环境底噪 -18 dBFS 铺满，脚步每 400 ms（峰值 -20 dBFS），t=1.00 s 一发满刻度枪声\n\n");

    for (int useB = 0; useB <= (hasLong ? 1 : 0); ++useB) {
        Mix mix = buildMix(useB ? g2L : gL, useB ? g2R : gR);
        const size_t shotLen = useB ? g2L.size() : gL.size();
        printf("=================== 枪声素材 %s（%.2f s）===================\n",
               useB ? "B 长尾" : "A 短促", shotLen/fsr);
        printf("%-8s %8s %10s %10s %11s %8s %9s\n",
               "上限", "最大压", "压缩时长", "尾音后恢复", "脚步被压", "窄带差", "枪声峰");

        for (float ceilDb : {0.f, -3.f, -6.f, -9.f, -12.f, -18.f, -24.f}) {
            DspParams p; p.aOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f;
            p.ceilDb = ceilDb; p.lookMs = 1.5f; p.bypass = false;
            p.bOn = true;  std::vector<float> on;
            runDsp(mix, p, on);
            p.bOn = false; std::vector<float> off;
            runDsp(mix, p, off);

            std::vector<double> G(total/hop + 2, 0.0);
            double maxDuck = 0; size_t peakAt = 0;
            for (size_t f = 0; f*hop + W < total; ++f) {
                G[f] = rmsDb(on, f*hop, W) - rmsDb(off, f*hop, W);
                if (G[f] < maxDuck) { maxDuck = G[f]; peakAt = f*hop; }
            }
            size_t endDuck = mix.gunAt;
            for (size_t f = mix.gunAt/hop; f*hop + W < total; ++f)
                if (G[f] < -1.0) endDuck = f*hop;
            double duckMs = (double)(endDuck - mix.gunAt) / fsr * 1000.0;

            double acc = 0; int cnt = 0, maskedStep = 0;
            for (size_t st : mix.stepAt) {
                if (st < mix.gunAt || st > endDuck + (size_t)(fsr*0.2)) continue;
                size_t f = st / hop; if (f + 1 >= G.size()) continue;
                double g = 0.5*(G[f] + G[f+1]);
                acc += g; ++cnt;
                if (g < -3.0) ++maskedStep;
            }
            double stepDuck = cnt ? acc/cnt : 0.0;

            Bp b1, b2;
            b1.set(48000.f, 1200.f, 3.f); b2.set(48000.f, 1200.f, 3.f);
            std::vector<float> won(total), woff(total);
            for (size_t i = 0; i < total; ++i) { won[i] = b1.proc(on[i]); woff[i] = b2.proc(off[i]); }
            size_t widx = peakAt;
            double gFull = rmsDb(on, widx, W) - rmsDb(off, widx, W);
            double gBand = rmsDb(won, widx, W) - rmsDb(woff, widx, W);

            // 脚步带的「总量」变化：输入 -> 输出（含 duck + 环境抑制 + 限幅全部）
            // 这是用户真正听到的数字：枪声窗口内脚步带被抬/被压了多少
            Bp fb1, fb2;
            fb1.set(48000.f, 700.f, 0.7f); fb2.set(48000.f, 700.f, 0.7f);
            std::vector<float> fin(total), fout(total);
            for (size_t i = 0; i < total; ++i) { fin[i] = fb1.proc(mix.L[i]); fout[i] = fb2.proc(on[i]); }
            double tacc = 0; int tcnt = 0;
            for (size_t st : mix.stepAt) {
                if (st < mix.gunAt || st > endDuck + (size_t)(fsr*0.2)) continue;
                tacc += rmsDb(fout, st, (size_t)(fsr*0.09)) - rmsDb(fin, st, (size_t)(fsr*0.09));
                ++tcnt;
            }
            double stepTotal = tcnt ? tacc/tcnt : 0.0;

            printf("%-8.0f %8.1f %9.0fms %9.0fms %9.1f/%d %8.2f %9.3f %9.1f\n",
                   ceilDb, maxDuck, duckMs,
                   (double)((long)endDuck - (long)(mix.gunAt + shotLen))/fsr*1000.0,
                   stepDuck, maskedStep, gBand - gFull,
                   peakOf(on, mix.gunAt, shotLen), stepTotal);
        }
        printf("\n");
    }

    printf("=================== C. 枪声本身：起音 vs 尾音 ===================\n");
    printf("峰值对齐；attack=峰值前2ms~后4.7ms 的峰；body=峰值后 80-400 ms 的 RMS\n\n");
    printf("%-8s %-8s %9s %9s %10s %9s %9s\n",
           "枪声", "上限", "attack", "body", "atk-body", "HF/LF变化", "峰值");
    for (int useB = 0; useB <= (hasLong ? 1 : 0); ++useB) {
        const std::vector<float>& src = useB ? g2L : gL;
        size_t pkIdx = 0; double pkv = 0;
        for (size_t i = 0; i < src.size(); ++i) if (fabsf(src[i]) > pkv) { pkv = fabsf(src[i]); pkIdx = i; }
        for (float ceilDb : {0.f, -3.f, -6.f, -9.f, -12.f}) {
            DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f;
            p.ceilDb = ceilDb; p.lookMs = 1.5f; p.bypass = false;
            Mix m; m.L.assign(total, 0.f); m.R.assign(total, 0.f);
            addAt(m, src, src, (size_t)(fsr*1.0), 1.0f);
            std::vector<float> o; runDsp(m, p, o);
            size_t at = (size_t)(fsr*1.0);
            double aI = peakOf(src, pkIdx > 96 ? pkIdx-96 : 0, 320);
            double bI = rmsDb(src, pkIdx + (size_t)(fsr*0.08), (size_t)(fsr*0.32));
            double aO = peakOf(o, at + (pkIdx > 96 ? pkIdx-96 : 0), 320);
            double bO = rmsDb(o, at + pkIdx + (size_t)(fsr*0.08), (size_t)(fsr*0.32));

            // 频谱倾斜：枪声起音后 0-200 ms，4-16k 与 40-300 的能量比（闷 = 高频相对变少）
            auto hfLf = [&](const std::vector<float>& x, size_t from) {
                Bp hp, lp; hp.set(48000.f, 4000.f, 0.707f); lp.set(48000.f, 300.f, 0.707f);
                // 这里用一阶近似：直接算高频能量（4k 以上）与低频能量（300 以下）比值
                double eh = 0, el = 0;
                Bp h2, l2; h2.set(48000.f, 5000.f, 0.707f); l2.set(48000.f, 200.f, 0.707f);
                for (size_t i = from; i < from + (size_t)(fsr*0.2) && i < x.size(); ++i) {
                    double h = h2.proc(x[i]), l = l2.proc(x[i]);
                    eh += h*h; el += l*l;
                }
                return 10.0*log10((eh+1e-20)/(el+1e-20));
            };
            double hfChange = hfLf(o, at) - hfLf(src, 0);
            printf("%-8s %-8.0f %8.1f %8.1f %10.1f %9.1f %9.3f\n",
                   useB ? "B长尾" : "A短促", ceilDb,
                   aO - aI, bO - bI, (aO-aI)-(bO-bI), hfChange, peakOf(o, at, src.size()));
        }
    }

    // ---------------- D. 脚步增强在枪声期间是否被关掉 ----------------
    printf("\n=================== D. 「脚步增强」在枪声期间的实际动作 ===================\n");
    printf("trSm=瞬态判据输出(0..1)，boost2=band2 实际抬升 dB = 7.8 * trSm * guard；guard=(-6-edb)/12+1 限幅\n\n");
    {
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f;
        p.ceilDb = -6.f; p.lookMs = 1.5f; p.bypass = false;
        Mix m = buildMix(hasLong ? g2L : gL, hasLong ? g2R : gR);
        std::vector<float> io(m.L.size()*2);
        for (size_t i = 0; i < m.L.size(); ++i) { io[2*i] = m.L[i]; io[2*i+1] = m.R[i]; }
        DspCore d; d.Prepare(48000.f); d.SetParams(p);
        const size_t chunk = 480;
        printf("%-8s %6s %7s\n", "t(s)", "trSm", "boost2");
        for (size_t i = 0; i + chunk <= m.L.size(); i += chunk) {
            d.Process(io.data() + i*2, (int)chunk);
            size_t t = i;
            // 只打关键区间：枪声前 0.8-1.0 s、枪声期间
            if (t < (size_t)(fsr*0.8) || t > (size_t)(fsr*3.6)) continue;
            if ((i / chunk) % 10) continue;                 // 每 100 ms 打一行
            printf("%-8.2f %6.2f %7.1f\n", (double)t/fsr, d.LastTrSm(),
                   7.8f * p.stepAmt * d.LastTrSm());
        }
    }
    return 0;
}
