// 离线测量：在真实 PUBG 素材上跑 DspCore 的瞬态检测与环境阈值判定，
// 输出 stepSens / envThr 各档位下的实际行为。samples/wem 里的素材按频谱特征分三类：
//   footstep 代理 = 中频瞬态（0.4~2.5kHz 质心 + 高动态）
//   ambience 代理 = 低频隆隆（质心 <250Hz）
//   gunshot 代理 = 明亮瞬态（质心 >=2.5kHz + 高动态）
// 每个素材跑一遍：记录 trSm_ 分布、以及四个频段包络 dB 的分布。
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;

struct Item { const char* name; const char* kind; float sens; };

static float g_fs = 48000.f;
static std::vector<float> g_tr;      // trSm_ 采样
static std::vector<float> g_band[4]; // 四段包络 dB

// 直接复刻 DspCore 的检测部分（不做实际分频/增益渲染，只测判据）
static void analyse(const std::vector<float>& L, const std::vector<float>& R,
                    const DspParams& p) {
    Env envFast, envSlow, envB[4];
    envFast.set(g_fs, 3.f, 3.f);
    envSlow.set(g_fs, 150.f, 150.f);
    for (int b = 0; b < 4; ++b) envB[b].set(g_fs, 5.f, 120.f);
    LR4 lp150, hp150, lp600, hp600, lp3k, hp3k;
    lp150.setLP(g_fs, 150.f); hp150.setHP(g_fs, 150.f);
    lp600.setLP(g_fs, 600.f); hp600.setHP(g_fs, 600.f);
    lp3k.setLP(g_fs, 3000.f); hp3k.setHP(g_fs, 3000.f);
    Biquad ap600, ap3k, ap3kb;
    ap600.setAP(g_fs, 600.f, 0.70710678f);
    ap3k.setAP(g_fs, 3000.f, 0.70710678f);
    ap3kb.setAP(g_fs, 3000.f, 0.70710678f);

    const float sensDb = 6.0f - 6.0f * p.stepSens;
    const int n0 = (int)L.size();
    for (int base = 0; base + 32 <= n0; base += 32) {
        int n = std::min(32, n0 - base);
        float bl[4][32], br[4][32];
        for (int i = 0; i < n; ++i) {
            float l = L[base + i], r = R[base + i];
            float h1l = hp150.hp(l);  float a0l = lp150.lp(l);
            float h2l = hp600.hp(h1l); float b1l = lp600.lp(h1l);
            a0l = ap3k.proc(ap600.proc(a0l));
            b1l = ap3kb.proc(b1l);
            bl[0][i] = a0l; br[0][i] = ap3k.proc(ap600.proc(r));
            bl[1][i] = b1l; br[1][i] = ap3kb.proc(r);
            bl[2][i] = lp3k.lp(h2l); br[2][i] = lp3k.lp(hp600.hp(h1l));
            bl[3][i] = hp3k.hp(h2l); br[3][i] = hp3k.hp(hp600.hp(h1l));
            float m = 0.5f * (l + r);
            envFast.proc(m); envSlow.proc(m);
            for (int b = 0; b < 4; ++b) envB[b].proc(0.5f * (bl[b][i] + br[b][i]));
        }
        float tdb = linToDb(envFast.e) - linToDb(envSlow.e);
        float tr = clampf((tdb - sensDb) / 12.0f, 0.f, 1.f);
        g_tr.push_back(tr);
        for (int b = 0; b < 4; ++b) g_band[b].push_back(linToDb(envB[b].e));
    }
}

static void stat(const char* tag, std::vector<float>& v) {
    if (v.empty()) { printf("%s: (empty)\n", tag); return; }
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[(size_t)(p * (v.size() - 1))]; };
    printf("%-14s n=%5zu  p10=%7.1f  med=%7.1f  p90=%7.1f  p99=%7.1f  mean=%6.2f\n",
           tag, v.size(), pct(0.10), pct(0.50), pct(0.90), pct(0.99),
           [&]{ double s = 0; for (float x : v) s += x; return s / v.size(); }());
}

static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    if (sz < 44) return false;
    size_t off = 12;
    int ch = 2, bits = 16;
    long sr = 48000;
    while (off + 8 <= (size_t)sz) {
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                      ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
        uint32_t csz = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                       ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
        const unsigned char* p = buf.data() + off + 8;
        if (id == 0x20746D66 && csz >= 16) {
            ch = p[2] | (p[3] << 8);
            sr = (long)p[4] | ((long)p[5] << 8) | ((long)p[6] << 16) | ((long)p[7] << 24);
            bits = p[14] | (p[15] << 8);
        } else if (id == 0x61746164) {
            int frames = (int)((csz) / (size_t)(ch * (bits / 8)));
            L.resize((size_t)frames); R.resize((size_t)frames);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i * ch * (bits / 8);
                float a, b;
                if (bits == 32) {
                    int32_t ia, ib;
                    memcpy(&ia, q, 4);
                    memcpy(&ib, q + 4, 4);
                    a = (float)ia / 2147483648.0f; b = (float)ib / 2147483648.0f;
                } else {
                    int16_t ia, ib;
                    memcpy(&ia, q, 2);
                    memcpy(&ib, q + 2, 2);
                    a = ia / 32768.0f; b = ib / 32768.0f;
                }
                L[(size_t)i] = a;
                R[(size_t)i] = ch > 1 ? b : a;
            }
            g_fs = (float)sr;
            return !L.empty();
        }
        off += 8 + csz + (csz & 1);
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: tipprobe <file.wav> <sens 0..1>\n"); return 1; }
    float sens = argc > 2 ? (float)atof(argv[2]) : 0.70f;
    std::vector<float> L, R;
    if (!loadWav(argv[1], L, R)) { printf("load failed\n"); return 1; }
    DspParams p; p.stepSens = sens;
    analyse(L, R, p);
    printf("== %s  stepSens=%.2f (sensDb=%.1f dB)  frames=%zu\n",
           argv[1], sens, 6.0f - 6.0f * sens, L.size());
    stat("trSm_", g_tr);
    static const char* bn[4] = { "band<150", "150-600", "600-3k", ">3k" };
    for (int b = 0; b < 4; ++b) stat(bn[b], g_band[b]);
    return 0;
}