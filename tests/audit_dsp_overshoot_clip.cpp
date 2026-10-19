// audit_dsp_overshoot_clip.cpp — 量化两件事：
//   (1) 主通路限幅的最大超限幅度（以及它为什么发生）
//   (2) track() 里 clipCount_ 的 |x|>=0.999 判据在「最大输出=100%」时的误报
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

static void worstOvershoot() {
    printf("=== A) 主通路限幅的最坏超限 ===\n");
    printf("  机理：dsp.h:405 用**本块**峰值 pk 算 target，dsp.h:418-419 却把\n");
    printf("        target 乘到**上一块**的 pendL_ 上。上一块峰值 pk_prev 可能远大于 pk。\n");
    printf("        最坏情况：本块静音 -> target=1.0 -> 上一块满幅原样输出。\n");
    printf("        上界 = ceilLin/pk_prev * pk_prev = pk_prev（即输入峰值，不设限）。\n\n");

    struct Case { const char* tag; float ceilDb; int quietEvery; float amp; };
    const Case cs[] = {
        {"满幅正弦 + 每8块插1块静音",  -6.f, 8,  1.0f},
        {"满幅正弦 + 每2块插1块静音",  -6.f, 2,  1.0f},
        {"满幅正弦 + 交替块",          0.f, 2,  1.0f},
        {"满幅方波 + 交替块",         -6.f, 2,  1.0f},
        {"超载 4x 正弦 + 交替块",     -6.f, 2,  4.0f},
        {"超载 100x 正弦 + 交替块",   -6.f, 2,  100.0f},
    };
    const int N = 48000*3;
    for (auto& c : cs) {
        std::vector<float> io((size_t)N*2);
        for (int i = 0; i < N; ++i) {
            int blk = i/32;
            bool quiet = (c.quietEvery && (blk % c.quietEvery == c.quietEvery-1));
            float s = quiet ? 0.f : c.amp*(float)std::sin(2*3.14159265f*1000.f*i/48000.f);
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = c.ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float cl = dbToLin(c.ceilDb);
        float mx = 0; int over = 0, firstAt = -1;
        for (int i = 4800; i < N; ++i) {
            float v = std::fabs(io[(size_t)i*2]);
            mx = std::max(mx, v);
            if (v > cl*1.0005f) { ++over; if (firstAt<0) firstAt=i; }
        }
        printf("  %-28s ceil=%-8.5f max=%-10.5f 超限=%+.2f dB 样本=%-6d %s\n",
               c.tag, cl, mx, 20*std::log10(mx/cl), over,
               over ? "<<< 失守" : "OK");
        if (firstAt >= 0) printf("       首次超限 @%d (t=%.1f ms)，输入该处幅度=%.4f\n",
                                  firstAt, firstAt/48.0, c.amp);
    }
    printf("\n");
}

static void clipFalsePositive() {
    printf("=== B) track() 的 |x|>=0.999 削顶判据：误报 ===\n");
    printf("  dsp.h:451-452: if (aa >= 0.999f) ++clip;\n");
    printf("  用户把「最大输出」拖到 100%% (=0 dBFS)，合法输出就可以到 1.0。\n\n");
    struct Case { const char* tag; float ceilDb; bool bOn; float amp; };
    const Case cs[] = {
        {"ceil=-6dB, 满幅正弦",     -6.f, true,  1.0f},
        {"ceil= 0dB (100%), 满幅正弦", 0.f, true,  1.0f},
        {"ceil= 0dB (100%), -0.1dBFS", 0.f, true,  0.9886f},
        {"ceil= 0dB (100%), -1dBFS",   0.f, true,  0.8913f},
        {"ceil= 0dB (100%), bypass",   0.f, false, 1.0f},
        {"ceil= 0dB (100%), -3dBFS",   0.f, true,  0.7079f},
    };
    const int N = 48000*3;
    for (auto& c : cs) {
        std::vector<float> io((size_t)N*2);
        for (int i = 0; i < N; ++i) {
            float s = c.amp*(float)std::sin(2*3.14159265f*1000.f*i/48000.f);
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = false; p.bOn = c.bOn; p.ceilDb = c.ceilDb; p.bypass = !c.bOn;
        d.SetParams(p);
        d.Process(io.data(), N);
        float mx = 0;
        for (int i = 4800; i < N; ++i) mx = std::max(mx, std::fabs(io[(size_t)i*2]));
        long cl = d.ClippedSamples();
        printf("  %-32s 输出max=%.5f  clipCount=%-8ld %s\n",
               c.tag, mx, cl,
               cl ? "<<< 误报（完全正常的信号被记为削顶）" : "OK");
    }
    printf("\n  影响：main.cpp:1456-1459 每 60s 读一次 clipCount，>0 就写日志\n");
    printf("        「检出 N 个削顶/非有限样本」。用户设 100%% 输出 + 正常满幅内容\n");
    printf("        -> 每分钟都刷一条假告警，掩盖真正的削顶。\n\n");
}

static void clipRealDetection() {
    printf("=== C) 反向验证：真削顶能不能被抓到 ===\n");
    const int N = 48000;
    {
        // 真削顶：ceil 设 -18dB，但灌入远超的信号，且确认主通路是否兜住
        std::vector<float> io((size_t)N*2);
        for (int i = 0; i < N; ++i) {
            float s = (float)std::sin(2*3.14159265f*1000.f*i/48000.f);
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = -18.f;
        d.SetParams(p);
        d.Process(io.data(), N);
        float mx = 0; for (auto v : io) mx = std::max(mx, std::fabs(v));
        printf("  ceil=-18dB 满幅正弦: max=%.5f clip=%ld  %s\n", mx, d.ClippedSamples(),
               d.ClippedSamples() ? "OK (检出)" : "（没检出，但输出确实被限住了）");
    }
    {
        // 真 NaN 输入
        std::vector<float> io((size_t)N*2, 0.f);
        for (int i = 0; i < N; ++i) {
            float s = (i%1000==0) ? std::nanf("") : 0.1f;
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; d.SetParams(p);
        d.Process(io.data(), N);
        int nanOut = 0; for (auto v : io) if (std::isnan(v)) ++nanOut;
        printf("  NaN 输入: 输出 NaN=%d clip=%ld  %s\n", nanOut, d.ClippedSamples(),
               (nanOut==0 && d.ClippedSamples()==0) ? "被静默吞掉（isfinite 保护生效）" : "?");
        printf("     注意：isfinite 把 NaN 变成 0，clip 判据**看不到**输入异常，\n");
        printf("     也就是说「非有限值也计入」这句注释（dsp.h:444）在本通路是失效的。\n");
    }
    printf("\n");
}

static void bypassClip() {
    printf("=== D) bypass 通路的削顶行为对照 ===\n");
    printf("  dsp.h:183-184 有 clampf(dl*gain_, -ceil_, ceil_) 硬夹，\n");
    printf("  所以 bypass=true 时永远不会超 ceil。验证。\n\n");
    const int N = 48000;
    for (float ceilDb : {-18.f, -6.f, 0.f}) {
        for (bool byp : {false, true}) {
            std::vector<float> io((size_t)N*2);
            std::mt19937 rng(9);
            std::uniform_real_distribution<float> ud(-1.f,1.f);
            for (int i = 0; i < N; ++i) {
                float s = ud(rng)*2.f;
                if (i>=20000 && i<20064) s = ((i&1)?1.f:-1.f);
                io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
            }
            DspCore d; d.Prepare(48000.f);
            DspParams p; p.aOn=false; p.bOn=true; p.ceilDb=ceilDb; p.bypass=byp;
            d.SetParams(p);
            d.Process(io.data(), N);
            float cl = dbToLin(ceilDb), mx=0;
            for (auto v : io) mx = std::max(mx, std::fabs(v));
            int over=0; for (auto v : io) if (std::fabs(v) > cl*1.0005f) ++over;
            printf("  ceilDb=%+6.1f bypass=%-5s : max=%.5f ceil=%.5f 超限=%-6d %s\n",
                   ceilDb, byp?"true":"false", mx, cl, over, over?"<<<":"OK");
        }
    }
    printf("\n");
}

int main() {
    printf("############################################################\n");
    printf("# 审计 4: 限幅超限量化 + clipCount_ 误报\n");
    printf("############################################################\n\n");
    worstOvershoot();
    clipFalsePositive();
    clipRealDetection();
    bypassClip();
    return 0;
}
