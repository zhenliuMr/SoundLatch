// audit_dsp_env.cpp — 审计 Env 包络 + dbToLin/linToDb 的数值安全性
//
// 疑点 1 (dsp.h:79-80)：a = expf(-1/(fs*attackMs*0.001))
//   ms 很小或 fs 很大时，1/(fs*ms*0.001) 变大 -> expf(-大数) -> 下溢到 0（不是溢出，是欠溢）。
//   但如果 ms = 0 就直接 1/0 = inf -> expf(-inf) = 0，a=0。
//   a=0 时 e = v + 0*(e-v) = v，即「无平滑」，直接跟随瞬时值 -> 判据变成峰值检测。
//   还要看 a 接近 1（ms 很大）时的**灾难性抵消**：e = v + c*(e-v)，c≈1 时 (e-v) 极小，
//   float 有效位数被吃掉 -> 包络「粘住」不下降。
//
// 疑点 2 (dsp.h:82)：reset() 只把 e 归零，a/r 保留。
//   如果 Env 从没被 set() 过（a=r=0 默认值），proc() 里 e = v，判据完全失效。
//   Prepare() 里 envB_/envFast_/... 都 set() 了，这条 OK；但要验证 Prepare 之外的路径。
//
// 疑点 3 (dsp.h:21)：dbToLin(+1000) = powf(10, 50) —— float 上限 3.4e38，10^50 > 上限 -> inf。
//   dsp.h:379 gCur_[b] = dbToLin(gdb)，gdb 上界是 boostDb[b] = stepAmt*12*wgt，
//   正常 <= 12 dB，安全。但 linToDb 负数 / Env 出 NaN 呢？
//
// 疑点 4 (dsp.h:22)：linToDb(v) = 20*log10f(v + 1e-9f)
//   v 传进来是 envB_[b].e，Env 保证 e>=0（因为 e = v + c*(e-v), v=|x|>=0, c in [0,1]）。
//   但如果 e 是 NaN，log10f(NaN)=NaN，会污染 tilt -> envGate -> gdb -> 全链。
//   测试方法：往 Process 灌 NaN，看 isfinite 保护是否真的挡住了。
//
// 疑点 5：Env 的 a/r 系数精度。float 下 exp(-1/(fs*ms*0.001)) 的有效位数。
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <limits>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

static const float INF = std::numeric_limits<float>::infinity();
static const float NAN_ = std::numeric_limits<float>::quiet_NaN();

static void envCoeffs() {
    printf("=== A) Env 系数 a/r 的取值与精度 ===\n");
    struct C { float fs, atk, rel; const char* tag; };
    const C cs[] = {
        {48000.f, 5.f,   120.f, "envB_ (dsp.h:226)"},
        {48000.f, 3.f,   3.f,   "envFast_ (dsp.h:227)"},
        {48000.f, 150.f, 150.f, "envSlow_ (dsp.h:228)"},
        {48000.f, 0.0f,  0.0f,  "ms=0 边界"},
        {48000.f, 1e-6f, 1e-6f, "ms=1e-6 极端小"},
        {48000.f, 1e6f,  1e6f,  "ms=1e6 极端大"},
        {192000.f,5.f,   120.f, "fs=192k 高采样率"},
        {8000.f,  5.f,   120.f, "fs=8k 低采样率"},
    };
    printf("%-24s %8s %8s %14s %14s %14s %14s\n",
           "tag","fs","ms", "a", "r", "1-a (攻击步长)", "有效位损失");
    for (auto& c : cs) {
        Env e; e.set(c.fs, c.atk, c.rel);
        double oneMinusA = (double)1.0 - (double)e.a;
        // float 下 1-a 的相对精度损失：eps(1.0)/|1-a|
        double lostBits = (1.1920929e-7) / std::max(oneMinusA, 1e-30) / std::log(2.0);
        printf("%-24s %8.0f %8.4g %14.9f %14.9f %14.3e %14.1f %s\n",
               c.tag, c.fs, c.atk, e.a, e.r, oneMinusA, lostBits,
               lostBits > 12 ? "<<< 精度崩溃" : (lostBits>4 ? "注意" : "OK"));
    }
    printf("\n  注：Env::proc 里 e = v + c*(e-v)。c≈1 时 (e-v) 是两个相近数相减，\n");
    printf("      相对误差被 (1-c) 放大。lostBits = eps/(1-c)/ln2 即放大的二进制位。\n\n");
}

static void envTransient() {
    printf("=== B) Env 启动瞬态（e 初值 0）与阶跃响应 ===\n");
    // B1: 恒定输入下的充电曲线
    {
        Env e; e.set(48000.f, 5.f, 120.f);
        printf("  [B1] 阶跃输入 1.0，attack=5ms，e 从 0 起来：\n      ");
        for (int i = 0; i <= 4800; i += 240) {
            if (i == 4800) e.proc(1.f);
            else e.proc(1.f);
            if (i % 480 == 0) printf("t=%5.1fms e=%.4f  ", i/48.0, e.e);
            if (i == 4800) break;
        }
        printf("\n      e 的**上升沿是渐进的**（不是瞬间跳到 1.0），这是 exp 平滑的正常行为。\n");
    }
    // B2: 关键 —— e=0 时 linToDb(e) = -180 dB，判据的起点
    {
        Env e; e.set(48000.f, 5.f, 120.f);
        printf("  [B2] reset 后 e=%.1f -> linToDb(e)=%.1f dB\n", e.e, linToDb(e.e));
        printf("      第一个样本 proc(1.0) 后 e=%.6f -> %.2f dB\n",
               (e.proc(1.f), e.e), linToDb(e.e));
        printf("      => 判据起点是 -180 dB，不是 -inf，clamp 后 envGate=0，不会误触发。%s\n\n",
               linToDb(0.f) < -100 ? "OK" : "<<<");
    }
    // B3: release 时间常数验证（一个时间常数后应降到 1/e = 0.3679）
    printf("  [B3] release 时间常数验证（充到 ~1.0 后输入 0，跑 1 个时间常数）：\n");
    for (float ms : {120.f, 1000.f, 10000.f}) {
        Env e; e.set(48000.f, 5.f, ms);
        for (int i = 0; i < 4800; i++) e.proc(1.f);
        float before = e.e;
        int samples = (int)(48000.0f * ms * 0.001f);
        for (int i = 0; i < samples; i++) e.proc(0.f);
        printf("      release=%-7.0fms  充到 %.6f -> 静默 %-7.0fms 后 e=%.6f (理论 1/e=0.367879) %s\n",
               ms, before, ms, e.e,
               std::fabs(e.e - 0.3678794) < 1e-4 ? "OK" : "<<< 偏离");
    }
    // B4: 生产实际用到的系数精度是否够（c 接近 1 的抵消）
    printf("  [B4] 生产系数下的包络更新精度（c≈1 时 e 的有效位）：\n");
    for (float ms : {3.f, 5.f, 120.f, 150.f}) {
        Env e; e.set(48000.f, 5.f, ms);
        for (int i = 0; i < 48000; i++) e.proc(1.f);   // 充到稳态
        double e0 = e.e;
        e.proc(0.f);                                   // 一步下降
        double actual = e.e;
        double ideal  = e0 * e.r;                      // 理论一步
        printf("      ms=%-6.0f c=%.9f  e: %.9f -> %.9f  理论 %.9f  相对误差 %.2e %s\n",
               ms, e.r, e0, actual, ideal, std::fabs(actual-ideal)/std::max(ideal,1e-30),
               std::fabs(actual-ideal)/ideal < 1e-3 ? "OK" : "<<< 精度不足");
    }
    printf("\n");
}

static void convFns() {
    printf("=== C) dbToLin / linToDb 的数值边界 ===\n");
    printf("  [C1] dbToLin 大 db：\n");
    for (float db : {0.f, 12.f, 100.f, 308.f, 320.f, 400.f, 1000.f, 1e9f}) {
        float v = dbToLin(db);
        printf("      dbToLin(%-10g) = %-14g %s\n", db, v,
               std::isinf(v) ? "<<< inf！" : (std::isnan(v) ? "<<< NaN！" : ""));
    }
    printf("\n  [C2] linToDb 边界：\n");
    for (float v : {1.f, 1e-9f, 0.f, -1e-12f, -0.5f, -1.f, NAN_, INF, -INF}) {
        float d = linToDb(v);
        printf("      linToDb(%-10g) = %-14g %s\n", v, d,
               std::isnan(d) ? "<<< NaN！" : (std::isinf(d) ? "<<< Inf" : ""));
    }
    printf("\n  [C3] 关键：Env 能否产生负的 e（linToDb 负数 -> NaN 的唯一路径）\n");
    {
        // e = v + c*(e-v), v>=0, c in [0,1] => e 始终 >= 0。数值验证：
        bool neg = false;
        Env e; e.set(48000.f, 5.f, 120.f);
        std::vector<float> sig(10000);
        for (size_t i = 0; i < sig.size(); ++i)
            sig[i] = (i%7==0) ? 1e-30f : ((i%11==0)? -5.f : 0.f);
        float mn = 1e30f;
        for (float s : sig) { e.proc(s); mn = std::min(mn, e.e); }
        printf("      随机含负输入的信号下 e 的最小值 = %g  %s\n", mn,
               mn < 0 ? "<<< e 变负了！" : "(>=0，linToDb 安全)");
        if (mn < 0) neg = true;
        // a/r 未初始化时（Env 默认构造）
        Env d;   // a=r=0
        d.proc(1.f); d.proc(0.f);
        printf("      默认构造 Env (a=r=0): proc(1)->e=%.3f, 再 proc(0)->e=%.3f  %s\n",
               1.f, d.e, "e 变成 0，衰减到 0 用了 1 样本" );
        (void)neg;
    }
    printf("\n");
}

static void nanFlood() {
    printf("=== D) NaN/Inf 洪水：Process 的 isfinite 防护是否有效 ===\n");
    const int N = 48000;
    std::vector<float> buf((size_t)N*2);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> ud(-0.5f, 0.5f);

    struct Case { const char* tag; float poison; int every; };
    const Case cs[] = {
        {"每 31 样本一个 NaN（错开块首）", NAN_, 31},
        {"每 32 样本一个 +Inf（块首）",    INF,  32},
        {"每 7 样本一个 NaN",             NAN_, 7},
        {"每 1000 样本一个 -Inf",         -INF, 1000},
        {"全部 NaN",                      NAN_, 1},
    };
    for (auto& c : cs) {
        for (int i = 0; i < N; ++i) {
            float s = (c.every == 1) ? c.poison
                     : ((i % c.every == 0) ? c.poison : ud(rng));
            buf[(size_t)i*2] = s; buf[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 1.f; p.stepAmt = 1.f;
        d.SetParams(p);
        d.Process(buf.data(), N);
        int nanOut = 0, infOut = 0, bigOut = 0; float mx = 0;
        for (float v : buf) {
            if (std::isnan(v)) ++nanOut;
            else if (std::isinf(v)) ++infOut;
            else { mx = std::max(mx, std::fabs(v)); if (std::fabs(v) > 1.0f) ++bigOut; }
        }
        printf("  %-30s 输出: NaN=%-6d Inf=%-6d |x|>1.0=%-6d max=%.4f  %s\n",
               c.tag, nanOut, infOut, bigOut, mx,
               (nanOut||infOut) ? "<<< 泄漏到输出！" : "已清除 OK");
    }
    printf("\n");
}

static void decayStability() {
    printf("=== E) 滤波器对极端输入的自激检测（DC / 满幅 / 长时间） ===\n");
    struct C { const char* tag; float amp; int kind; };
    const C cs[] = {
        {"DC 满幅 1.0", 1.f, 0}, {"DC 幅度 1000", 1000.f, 0},
        {"奈奎斯特方波 1.0", 1.f, 1}, {"满幅白噪 1.0", 1.f, 2},
        {"极低频 5Hz 满幅", 1.f, 3},
    };
    for (auto& c : cs) {
        const int N = 48000*10;
        std::vector<float> buf((size_t)N*2);
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> ud(-1.f,1.f);
        for (int i = 0; i < N; ++i) {
            float s = 0;
            switch (c.kind) {
                case 0: s = c.amp; break;
                case 1: s = (i%2) ? c.amp : -c.amp; break;
                case 2: s = ud(rng)*c.amp; break;
                case 3: s = c.amp*(float)std::sin(2*3.14159265*5.0*i/48000.0); break;
            }
            buf[(size_t)i*2] = s; buf[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 1.f; p.stepAmt = 1.f;
        p.ceilDb = 0.f;
        d.SetParams(p);
        d.Process(buf.data(), N);
        float mx = 0; int nf = 0;
        for (float v : buf) { if(!std::isfinite(v)) ++nf; mx = std::max(mx, std::fabs(v)); }
        printf("  %-22s 10s 后 max=%.4f  非有限=%d  clip计数=%ld  %s\n",
               c.tag, mx, nf, d.ClippedSamples(),
               (mx > 1.05f || nf) ? "<<< 自激/超限" : "OK");
    }
    printf("\n");
}

int main() {
    printf("############################################################\n");
    printf("# 审计 2: Env 包络 / dbToLin / linToDb 数值安全性\n");
    printf("############################################################\n\n");
    envCoeffs();
    envTransient();
    convFns();
    nanFlood();
    decayStability();
    return 0;
}
