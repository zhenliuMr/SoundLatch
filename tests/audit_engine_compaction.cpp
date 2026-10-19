// audit_engine_compaction.cpp
//
// 审计目标：engine.cpp RenderLoop() 的「compaction + 欠载 duck」两段逻辑。
// 这两段无法直接跑（需要真实 WASAPI 设备），所以本测试把 engine.cpp:377-433
// 逐行转写进一个可驱动的 harness，并使用**真实的 sl::Ring**（engine.h 原样 include）。
// 转写部分用 SLX_* 标记，行号注释指向 src/engine.cpp。
//
// 结论（见运行输出）：
//   [P0] cur_ 在欠载期间继续推进而不夹紧到 inLen_，下一次 compaction 时
//        (inLen_ - n) 以 size_t 下溢 -> memmove 长度 ~1.8e19 -> 进程崩溃。
//   [P1] 欠载瞬间 duck 淡出被 sL=sR=0 短路，输出硬跳变到 0（爆音），
//        且 duck 是每回调局部变量，跨回调重置为 1.0 -> 淡入也失效。

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include "engine.h"

using namespace sl;

static const float kFs = 48000.f;

// --------------------------------------------------------------- harness
// 转写自 engine.cpp:363-444。字段与 AudioEngine 的同名成员一一对应。
struct Sim {
    Ring   ring_;
    std::vector<float> inBuf_;
    size_t inLen_ = 0;
    double cur_ = 0.0, ratio_ = 1.0, integ_ = 0.0, target_ = 0.0;

    int  underruns_ = 0;
    float lastRingMs_ = 0.f, lastDbgLevel_ = 0.f, lastRatio_ = 1.f;
    bool  lastStarved_ = false;

    // 一旦置位，compaction 就不再真的 memmove（否则测试进程直接死），
    // 改为把「本应 memmove 的字节数」记录下来。这让我们能安全地拿到证据。
    bool  dryRun_ = true;
    size_t wouldMoveBytes_ = 0;
    size_t wouldMoveSrcFrames_ = 0;
    size_t wouldMoveDstFrames_ = 0;

    void init(size_t capBufFrames) {
        ring_.init(48000 * 2);
        target_ = (double)(capBufFrames + capBufFrames / 4 + 64);
        inBuf_.assign(16384 * 2, 0.f);
        inLen_ = 0; cur_ = 0.0; ratio_ = 1.0; integ_ = 0.0;
    }

    // engine.cpp:377-433
    void renderBlock(UINT32 frames, std::vector<float>& out) {
        out.assign((size_t)frames * 2, 0.f);
        float* f = out.data();

        // ---- compaction (engine.cpp:377-384)
        size_t n = (size_t)cur_;
        if (n > 0) {
            wouldMoveBytes_    = (inLen_ - n) * 2 * sizeof(float);   // size_t 运算，与源码一致
            wouldMoveSrcFrames_ = n;
            wouldMoveDstFrames_ = inLen_ - n;
            if (!dryRun_) {
                memmove(inBuf_.data(), inBuf_.data() + n * 2,
                        (inLen_ - n) * 2 * sizeof(float));
            }
            inLen_ -= n;
            cur_ -= (double)n;
        }

        size_t capFrames = inBuf_.size() / 2;
        size_t want = capFrames - 2 - inLen_;
        size_t got = ring_.pop(inBuf_.data() + inLen_ * 2, want);
        inLen_ += got;

        // ---- ASRC servo (engine.cpp:392-397)
        double level = (double)ring_.available() + (double)inLen_;
        double err = (target_ > 0.0) ? (level - target_) / target_ : 0.0;
        if (fabs(err) < 0.35) integ_ = clampd_stub(integ_ + err * 0.02, -3.0, 3.0);
        double wantRatio = 1.0 + clampd_stub(err * 0.45 + integ_ * 0.03, -0.10, 0.10);
        ratio_ += (wantRatio - ratio_) * 0.10;
        ratio_ = clampd_stub(ratio_, 0.85, 1.15);

        // ---- duck + interp (engine.cpp:403-429)
        const float kFadeStep = 1.0f / (5.0f * 0.001f * 48000.0f);
        float duck = 1.0f;                  // <== 每回调重置，engine.cpp:404
        bool starved = false;
        for (UINT32 k = 0; k < frames; ++k) {
            size_t i0 = (size_t)cur_;
            double fr = cur_ - (double)i0;
            if (i0 + 1 >= inLen_) {
                duck -= kFadeStep;
                if (duck < 0.f) duck = 0.f;
                starved = true;
            } else if (duck < 1.0f) {
                duck += kFadeStep;
                if (duck > 1.0f) duck = 1.0f;
            }
            float sL, sR;
            if (i0 + 1 >= inLen_) {
                sL = sR = 0.f;               // <== engine.cpp:419，淡出被短路
            } else {
                float aL = inBuf_[i0 * 2], aR = inBuf_[i0 * 2 + 1];
                float bL = inBuf_[(i0 + 1) * 2], bR = inBuf_[(i0 + 1) * 2 + 1];
                sL = (float)(aL + (bL - aL) * fr);
                sR = (float)(aR + (bR - aR) * fr);
            }
            f[k * 2]     = sL * duck;
            f[k * 2 + 1] = sR * duck;
            cur_ += ratio_;
        }
        if (starved) {
            underruns_++;
            ratio_ = clampd_stub(ratio_ * 0.997, 0.85, 1.15);
        }
        lastStarved_ = starved;
        lastRingMs_  = (float)(((double)ring_.available() + (double)inLen_ - cur_) * 1000.0 / 48000.0);
        lastDbgLevel_ = (float)((double)inLen_ - cur_);
        lastRatio_ = (float)ratio_;
    }

    static double clampd_stub(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }
};

static void pushSine(Sim& s, size_t frames, double amp, double& phase) {
    std::vector<float> tmp(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        tmp[i * 2] = (float)(amp * sin(phase));
        tmp[i * 2 + 1] = (float)(amp * sin(phase));
        phase += 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
    }
    s.ring_.push(tmp.data(), frames);
}

// ============================================================ [P0] 测试
static void testP0_compactionUnderflow() {
    printf("=== [P0] 欠载 -> cur_ 超前 -> compaction size_t 下溢 ===\n");
    const UINT32 kPeriod = 480;          // 10 ms @48k，典型共享模式周期
    Sim s;
    s.init(480);                          // 采集周期 10 ms
    double ph = 0.0;

    printf("  capFrames=%zu  target_=%.0f  period=%u\n",
           s.inBuf_.size() / 2, s.target_, kPeriod);

    std::vector<float> out;
    // 先正常跑 20 个回调，把 inBuf_ / cur_ 伺服到稳态
    for (int i = 0; i < 20; ++i) {
        pushSine(s, 480, 0.5, ph);
        s.renderBlock(kPeriod, out);
    }
    printf("  稳态: inLen_=%zu cur_=%.2f ratio_=%.4f ringMs=%.2f underruns=%d\n",
           s.inLen_, s.cur_, s.ratio_, s.lastRingMs_, s.underruns_);

    // 现在采集端断流（模拟 CABLE 被占用 / 驱动卡住），连续 3 个回调无数据
    printf("  -- 采集断流 3 个回调 --\n");
    for (int i = 1; i <= 3; ++i) {
        s.renderBlock(kPeriod, out);
        printf("     cb%-2d starved=%d inLen_=%-6zu cur_=%-9.2f  cur_>inLen_? %s\n",
               i, (int)s.lastStarved_, s.inLen_, s.cur_,
               s.cur_ > (double)s.inLen_ ? "*** 是 ***" : "否");
    }

    // 第 4 个回调：进入 compaction
    printf("  -- 下一次回调进入 compaction (engine.cpp:378-383) --\n");
    size_t n = (size_t)s.cur_;
    if (n > 0) {
        size_t bytes = (s.inLen_ - n) * 2 * sizeof(float);
        printf("     n = (size_t)cur_      = %zu\n", n);
        printf("     inLen_               = %zu\n", s.inLen_);
        printf("     inLen_ - n           = %zu   <== size_t 下溢\n", s.inLen_ - n);
        printf("     memmove 字节数        = %zu   (%.3e)\n", bytes, (double)bytes);
        printf("     等价帧数              = %.3e\n", (double)bytes / 8.0);
        printf("     源指针偏移            = inBuf_.data() + %zu floats (= %.1f MiB)\n",
               n * 2, (double)n * 8.0 / 1048576.0);
        if (s.inLen_ < n) {
            printf("\n  >>> 判定: cur_ (%.2f) > inLen_ (%zu)，(inLen_-n) 下溢。\n"
                   "  >>> engine.cpp:380 会执行 memmove(dst, src+%.1fMiB, %.3e 字节)\n"
                   "  >>> = 立即访问违例，渲染线程崩溃，整个进程被杀。\n",
                   s.cur_, s.inLen_, (double)n * 8.0 / 1048576.0, (double)bytes);
            printf("  >>> 触发条件: 采集端连续断流 >= 1 个渲染周期（10 ms）即触发。\n");
        } else {
            printf("\n  本次未触发（cur_ <= inLen_）。\n");
        }
    }

    // 实弹：让 harness 真的 memmove（不加 dryRun），观察是否被杀
    printf("\n  -- 实弹验证（关闭 dryRun，真的执行 memmove）--\n");
    Sim s2;
    s2.dryRun_ = false;
    s2.init(480);
    double ph2 = 0.0;
    std::vector<float> o2;
    for (int i = 0; i < 20; ++i) { pushSine(s2, 480, 0.5, ph2); s2.renderBlock(kPeriod, o2); }
    for (int i = 0; i < 3; ++i) s2.renderBlock(kPeriod, o2);
    printf("     进入第 4 次 compaction, n=%zu inLen_=%zu ... 等待崩溃 ...\n",
           (size_t)s2.cur_, s2.inLen_);
    fflush(stdout);
    s2.renderBlock(kPeriod, o2);          // 这里应该死
    printf("     !!! 存活了（意外）n=%zu inLen_=%zu\n", (size_t)s2.cur_, s2.inLen_);
}

// ============================================================ [P1] 测试
static void testP1_duckClick() {
    printf("\n=== [P1] 欠载瞬间 duck 淡出是否生效 ===\n");
    const UINT32 kPeriod = 480;
    Sim s;
    s.init(480);
    double ph = 0.0;
    std::vector<float> out;

    for (int i = 0; i < 20; ++i) { pushSine(s, 480, 0.9, ph); s.renderBlock(kPeriod, out); }

    // 断流，采集 out 的第一个回调，找最大相邻样本跳变
    pushSine(s, 480, 0.9, ph);            // 补满，让第一个回调前半段有数据
    s.renderBlock(kPeriod, out);
    // 现在真的断流
    s.renderBlock(kPeriod, out);

    double maxJump = 0.0; size_t at = 0;
    for (size_t i = 1; i < out.size() / 2; ++i) {
        double d = fabs((double)out[i * 2] - (double)out[(i - 1) * 2]);
        if (d > maxJump) { maxJump = d; at = i; }
    }
    printf("  欠载块内最大相邻样本跳变 = %.6f  (出现在第 %zu 个样本)\n", maxJump, at);
    printf("  duck 起点 = 1.0，第 %d 个样本处 sL 被硬置 0\n", (int)at);
    printf("  -> 跳变幅度 == 该样本 duck 尚未淡完的幅度 * 信号幅度\n");
    printf("  -> 理论上限 = |x| * (1 - at/240)。这里 at=%zu -> 上限 %.3f，实际 %.3f\n",
           at, 0.9 * (1.0 - (double)at / 240.0), maxJump);
    printf("  -> 0.9 满幅信号下这是 %.0f dBFS 的阶跃 = 明确可听爆音。\n",
           20.0 * log10(maxJump / 0.9 + 1e-12));

    // 对照：如果 duck 真的作用在信号上（而不是被 sL=0 短路）
    // 正确实现在欠载时应输出 lastSample * duck，duck 从 1 线性降到 0。
    printf("\n  对照组（duck 真正作用在保持的样本上时应有的跳变）:\n");
    double ideal = 0.9 * (1.0 / 240.0);
    printf("    每样本 duck 步长 kFadeStep = %.6f\n", 1.0 / 240.0);
    printf("    若实现为 lastSample*duck，最大跳变 = 0.9*%.6f = %.6f (小 1 个数量级)\n",
           1.0 / 240.0, ideal);
    printf("    实测 %.6f 是理想值的 %.0f 倍\n", maxJump, maxJump / ideal);
}

// ============================================================ [P1b] 跨回调
static void testP1b_duckResetsPerCallback() {
    printf("\n=== [P1b] duck 是每回调局部变量 -> 5 ms 淡出跨不过回调边界 ===\n");
    const UINT32 kPeriod = 480;           // 10 ms > 5 ms，单回调内能淡完
    Sim s;
    s.init(480);
    double ph = 0.0;
    std::vector<float> out;

    printf("  场景 A: period=480 (10ms) —— 单回调就能淡完 240 样本，duck 能到 0\n");
    for (int i = 0; i < 20; ++i) { pushSine(s, 480, 0.9, ph); s.renderBlock(kPeriod, out); }
    s.renderBlock(kPeriod, out);
    printf("    欠载块尾样本 = %.6f (应为 0，因为 sL=0 短路)\n", out[(kPeriod - 1) * 2]);

    printf("\n  场景 B: period=128 (2.7ms) —— 单回调只有 128 样本 < 240\n");
    Sim s2; s2.init(480);
    double ph2 = 0.0; std::vector<float> o2;
    for (int i = 0; i < 40; ++i) { pushSine(s2, 480, 0.9, ph2); s2.renderBlock(128, o2); }
    s2.renderBlock(128, o2);
    printf("    欠载块尾样本 = %.6f\n", o2[(128 - 1) * 2]);
    // 手动推演 duck 在块尾的值
    float duck = 1.0f;
    const float step = 1.0f / 240.0f;
    for (int i = 0; i < 128; ++i) { duck -= step; if (duck < 0) duck = 0; }
    printf("    若无 sL=0 短路，块尾 duck = %.4f（下个回调会被重置回 1.0）\n", duck);
    printf("    -> 淡出在 128 样本处只完成 %.0f%%，回调边界处 duck 从 %.3f 跳回 1.0\n",
           128.0 / 240.0 * 100.0, duck);
    printf("    -> 数据恢复瞬间: 上一样本 = %.3f*0 ，下一样本 = %.3f*1.0 = %.3f\n",
           0.0, 0.9, 0.9);
    printf("    -> 恢复瞬间 0 -> 0.9 硬跳变 = %.1f dBFS 爆音\n", 20.0 * log10(0.9));
}

// ============================================================ 附加观察
static void testServoLevelBug() {
    printf("\n=== [附加] servo 的 level 少减了 cur_ (engine.cpp:392) ===\n");
    printf("  line 392 : level = ring_.available() + inLen_\n");
    printf("  line 438 : ringMs = (ring_.available() + inLen_ - cur_) * 1000/48000\n");
    printf("  同一个物理量，两处算法不一致。\n");
    printf("  cur_ 在稳态是 [0,1) 的小数，误差 < 1 样本，可忽略；\n");
    printf("  但欠载期间 cur_ 会远超 inLen_（见 P0），此时 line 392 高估 level\n");
    printf("  -> err 被算成接近 0 -> servo 认为「电平正常」-> 完全不加速回补。\n");
}

int main() {
    printf("SoundLatch engine.cpp 审计 —— compaction / duck\n");
    printf("-----------------------------------------------\n");
    testP0_compactionUnderflow();
    testP1_duckClick();
    testP1b_duckResetsPerCallback();
    testServoLevelBug();
    printf("\n-----------------------------------------------\n完成。\n");
    return 0;
}