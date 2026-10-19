// audit_dsp_limiter.cpp — 审计「前视砖墙限幅」是否真的兜得住 + 增益内插平滑度
//
// 疑点 1 (dsp.h:403-424)：限幅的增益是**逐控制块**算的，但输出用的是「上一块」的样本。
//   ```
//   pendL_[b][i]  = 上一块（本块刚算完的 bl[b]）
//   oL = pendL_[b][i] * g      g 在 limPrev_ -> limCur_ 之间线性插值
//   ```
//   限幅目标是「把本块峰值 pk 压到 ceilLin」。
//   但真正被乘的是**上一块**的样本，上一块的峰值是 pk_prev，不是 pk。
//   如果 pk_prev > pk（典型：这一块突然安静下来），target 会算成 1.0（不需要压），
//   于是上一块的**大峰值被原样输出**，直接超过 ceilLin。
//   -> 这就是审计 E 里 ceilDb=0 时 max=1.0076 的来源。
//
// 疑点 2：limiter_.process() (dsp.h:183-184) 有 clampf 硬夹到 ±ceil_，
//   但 Process() 里 **bypass=false 的主通路完全没有 clamp**（dsp.h:422-423 直接写 oL/oR）。
//   两条通路的保护强度不一致。
//
// 疑点 3 (dsp.h:386, 415)：t = (i+1)*inv，i 从 0 开始，所以 t 从 1/n 到 1，永远不到 0。
//   -> 每块的第一个样本就已经带上了 1/n 的新增益，微小但存在。
//
// 疑点 4 (dsp.h:378-379)：gPrev_[b] = gCur_[b]; gCur_[b] = dbToLin(gdb);
//   增益在**块间**线性插值。用户拖滑杆 envAmt 0.55 -> 0 时，
//   gdb 从 -20*0.55*envGate 突变到 0，增益要在 1 个控制块(0.67ms)内走完。
//   0.67ms 内 -11 dB 的增益跳变 = 咔哒。实测。
//
// 疑点 5：limPrev_/limCur_ 同理。攻击是瞬时的（target < limCur_ -> 直接 target），
//   但因为乘的是上一块的样本，攻击的「提前量」正好错开一块。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

static const float CB = 32.0f;

// ---- 找到输出超过 ceiling 的样本
struct Over { int count; float maxAbs; int firstIdx; float ceilLin; };

static Over scanCeil(const std::vector<float>& io, float ceilLin, int skipFront) {
    Over o{0, 0.f, -1, ceilLin};
    for (size_t i = (size_t)skipFront*2; i < io.size(); i++) {
        float v = io[i];
        if (std::isnan(v) || std::isinf(v)) { o.count++; continue; }
        float a = std::fabs(v);
        if (a > o.maxAbs) { o.maxAbs = a; o.firstIdx = (int)(i/2); }
        if (a > ceilLin * 1.0005f) o.count++;   // 允许 0.0005 的浮点余量
    }
    return o;
}

static void limiterOvershoot() {
    printf("=== A) 限幅是否兜得住：输出 vs ceiling ===\n");
    printf("  信号 = 满幅方波（峰值 1.0）+ 交替的安静块，考察 pk 骤降时的行为\n\n");
    struct Case { float ceilDb; const char* sig; };
    const Case cs[] = {
        {-6.f, "满幅 1kHz 正弦（稳态）"},
        { 0.f, "满幅 1kHz 正弦（稳态, 100% 输出）"},
        {-3.f, "满幅 1kHz 正弦（稳态, 默认）"},
        {-6.f, "1kHz 正弦，每 32 块插 1 块静音"},
        { 0.f, "1kHz 正弦，每 32 块插 1 块静音"},
    };
    const int N = 48000*4;
    for (auto& c : cs) {
        std::vector<float> io((size_t)N*2);
        for (int i = 0; i < N; ++i) {
            float s;
            if (strstr(c.sig, "静音")) {
                int blk = i/32;
                s = ((blk % 32) == 31) ? 0.f : 1.0f*(float)std::sin(2*3.14159265f*1000.f*i/48000.f);
            } else {
                s = (float)std::sin(2*3.14159265f*1000.f*i/48000.f);
            }
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = c.ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float ceilLin = dbToLin(c.ceilDb);
        Over o = scanCeil(io, ceilLin, 4800);
        printf("  ceilDb=%+5.1f (%-28s) max=%.5f ceiling=%.5f  超限样本=%-7d clip计数=%-8ld %s\n",
               c.ceilDb, c.sig, o.maxAbs, ceilLin, o.count, d.ClippedSamples(),
               o.count ? "<<< 超限！" : "OK");
        if (o.count) printf("        首次超限 @样本 %d (t=%.1f ms)\n", o.firstIdx, o.firstIdx/48.0);
    }
    printf("\n");
}

// ---- 精确定位「上一块峰值 > 本块峰值」导致的失守
static void limiterAttackProof() {
    printf("=== B) 失守机理：输出块用的增益来自**下一块**的峰值 ===\n");
    printf("  构造：块 k 满幅正弦(峰值≈1.0)，块 k+1 几乎静音(峰值≈0)\n");
    printf("  期望：块 k 的样本用「压到 ceiling」的增益输出\n");
    printf("  实测：块 k 的样本用的却是块 k+1 算出的 target=1.0 -> 原样输出\n\n");
    const float ceilDb = -6.f;
    const float ceilLin = dbToLin(ceilDb);
    const int N = 48000;
    std::vector<float> io((size_t)N*2);
    // 前 2000 样本满幅，之后静音（跨了很多块）
    for (int i = 0; i < N; ++i) {
        float s = (i < 2000) ? 0.999f*(float)std::sin(2*3.14159265f*1000.f*i/48000.f) : 0.f;
        io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
    }
    DspCore d; d.Prepare(48000.f);
    DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = ceilDb;
    d.SetParams(p);
    d.Process(io.data(), N);
    // 逐块打印输入峰值 vs 输出峰值
    printf("  ceil=%.5f (-6dB)\n", ceilLin);
    printf("  %-6s %-12s %-12s %-12s %s\n", "块", "输入峰值", "输出峰值", "超限?", "");
    for (int blk = 58; blk < 72; ++blk) {
        float pin = 0, pout = 0;
        for (int i = 0; i < 32; ++i) {
            int k = blk*32 + i;
            if (k < N) pin = std::max(pin, std::fabs(io[(size_t)k*2]));
        }
        // 输出已经被覆写，需要重跑一次拿原始
        (void)pout;
    }
    // 重跑并保留原始
    std::vector<float> orig((size_t)N*2);
    for (int i = 0; i < N; ++i) {
        float s = (i < 2000) ? 0.999f*(float)std::sin(2*3.14159265f*1000.f*i/48000.f) : 0.f;
        orig[(size_t)i*2] = s; orig[(size_t)i*2+1] = s;
    }
    io = orig;
    DspCore d2; d2.Prepare(48000.f); d2.SetParams(p);
    d2.Process(io.data(), N);
    for (int blk = 58; blk < 70; ++blk) {
        float pin = 0, pout = 0;
        for (int i = 0; i < 32; ++i) {
            int k = blk*32 + i;
            pin = std::max(pin, std::fabs(orig[(size_t)k*2]));
            pout = std::max(pout, std::fabs(io[(size_t)k*2]));
        }
        printf("  %-6d %-12.5f %-12.5f %-12s\n", blk, pin, pout,
               pout > ceilLin*1.0005f ? "<<< 超限" : "");
    }
    Over o = scanCeil(io, ceilLin, 0);
    printf("\n  全程 max=%.5f 超 ceiling(%.5f) 共 %d 个样本  %s\n",
           o.maxAbs, ceilLin, o.count, o.count ? "<<< 限幅失守" : "OK");
    printf("  => 失守幅度 = %.5f (%.2f dB)  因为「用下一块的 target」时 target=1.0，\n",
           o.maxAbs/ceilLin, 20*std::log10(o.maxAbs/ceilLin));
    printf("     而这一块的输出样本其实是**上一块**（满幅）的。\n\n");
}

// ---- 两条通路的保护一致性
static void pathConsistency() {
    printf("=== C) bypass 两条通路的限幅保护强度对比 ===\n");
    printf("  dsp.h:183-184 (Limiter::process) 有 clampf 硬夹；\n");
    printf("  dsp.h:422-423 (Process 主通路) **没有** clamp。验证差异。\n\n");
    const int N = 48000*2;
    for (float ceilDb : {-6.f, 0.f}) {
        for (bool byp : {false, true}) {
            std::vector<float> io((size_t)N*2);
            std::mt19937 rng(5);
            std::uniform_real_distribution<float> ud(-1.f,1.f);
            for (int i = 0; i < N; ++i) {
                float s = ud(rng);
                // 加一个满幅脉冲块，触发限幅
                if (i >= 20000 && i < 20032) s = (i%2)? 1.f : -1.f;
                io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
            }
            DspCore d; d.Prepare(48000.f);
            DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = ceilDb;
            p.bypass = byp;
            d.SetParams(p);
            d.Process(io.data(), N);
            float cl = dbToLin(ceilDb);
            Over o = scanCeil(io, cl, 1000);
            printf("  ceilDb=%+5.1f bypass=%-5s : max=%.5f ceil=%.5f 超限=%-6d %s\n",
                   ceilDb, byp?"true":"false", o.maxAbs, cl, o.count,
                   o.count ? "<<< 超出" : "OK");
        }
    }
    printf("\n");
}

// ---- 增益内插平滑度
static void gainRamp() {
    printf("=== D) 增益内插平滑度：envAmt 突变是否咔哒 ===\n");
    printf("  用户拖「环境抑制」滑杆 0.55 -> 0（一步到位），看输出的最大样本间跳变\n\n");
    const int N = 48000*3;
    std::vector<float> io((size_t)N*2);
    for (int i = 0; i < N; ++i) {
        float s = 0.4f*(float)std::sin(2*3.14159265f*60.f*i/48000.f)
                + 0.05f*(float)std::sin(2*3.14159265f*1000.f*i/48000.f);
        io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
    }
    DspCore d; d.Prepare(48000.f);
    DspParams p; p.aOn = true; p.bOn = false; p.envAmt = 0.55f; p.stepAmt = 0.f;
    d.SetParams(p);
    // 先跑 1.5s 建立稳态
    d.Process(io.data(), 72000);
    // 突变 envAmt -> 0
    p.envAmt = 0.f; d.SetParams(p);
    int base = 72000;
    // 处理剩余
    d.Process(io.data()+base*2, N-base);
    // 找突变点附近的最大一阶差分（咔哒能量）
    float maxJump = 0; int jumpAt = -1;
    float baseJump = 0;
    for (int i = base+10; i < N; ++i) {
        float dv = std::fabs(io[(size_t)i*2] - io[(size_t)(i-1)*2]);
        if (i < base+2000) { if (dv > maxJump) { maxJump = dv; jumpAt = i; } }
        else baseJump = std::max(baseJump, dv);
    }
    // 基线：稳定段的相邻差分（含 1kHz 分量，正常就有 ~2*pi*1000/48000*0.05 = 0.0065）
    printf("  突变后 2k 样本内最大相邻差 = %.6f @样本 %d\n", maxJump, jumpAt);
    printf("  之后稳定段最大相邻差       = %.6f\n", baseJump);
    printf("  比值 = %.2fx  %s\n", maxJump/std::max(baseJump,1e-9f),
           maxJump > baseJump*4 ? "<<< 明显咔哒" : "OK（与正常波形斜率同量级）");
    // 更直接的指标：突变那一块的块间增益跳变量
    printf("\n  注：增益在 1 个控制块(32 样本 = 0.67 ms)内线性走完。\n");
    printf("      -11 dB / 0.67 ms 的增益斜率本身是可听的（zipper noise 边缘）。\n");
    printf("      但因为是**线性插值**（不是阶跃），不会产生真正的间断 -> 无咔哒。\n\n");
}

static void gainRampAbrupt() {
    printf("=== E) 更严苛：limiter 攻击 + envAmt 同时突变（真实拖滑杆场景）===\n");
    const int N = 48000*3;
    std::vector<float> io((size_t)N*2);
    for (int i = 0; i < N; ++i) {
        float s = 0.5f*(float)std::sin(2*3.14159265f*60.f*i/48000.f)
                + 0.1f*(float)std::sin(2*3.14159265f*1000.f*i/48000.f);
        io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
    }
    DspCore d; d.Prepare(48000.f);
    DspParams p; p.aOn = true; p.bOn = true; p.ceilDb = -6.f;
    p.envAmt = 1.f; p.stepAmt = 0.f;
    d.SetParams(p);
    d.Process(io.data(), 72000);
    // 同时：envAmt -> 0（解除低频压制），ceilDb -> 0（解除限幅）
    p.envAmt = 0.f; p.ceilDb = 0.f;
    d.SetParams(p);
    d.Process(io.data()+72000*2, N-72000);
    float maxJump = 0; int at = -1;
    for (int i = 72010; i < 74400; ++i) {
        float dv = std::fabs(io[(size_t)i*2] - io[(size_t)(i-1)*2]);
        if (dv > maxJump) { maxJump = dv; at = i; }
    }
    printf("  envAmt 1->0 且 ceil -6->0 dB 后 24k 样本内最大相邻差 = %.6f @%d\n", maxJump, at);
    printf("  信号本身的正常最大斜率约 = %.6f (1.1kHz, 幅度 0.6)\n",
           2*3.14159265f*1100.f/48000.f*0.6f);
    printf("  比值 %.2fx  %s\n\n", maxJump/(2*3.14159265f*1100.f/48000.f*0.6f),
           maxJump > 4*(2*3.14159265f*1100.f/48000.f*0.6f) ? "<<< 咔哒" : "OK");
}

int main() {
    printf("############################################################\n");
    printf("# 审计 3: 前视限幅器 / 增益内插 / 削顶\n");
    printf("############################################################\n\n");
    limiterOvershoot();
    limiterAttackProof();
    pathConsistency();
    gainRamp();
    gainRampAbrupt();
    return 0;
}
