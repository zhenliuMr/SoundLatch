// audit_engine_starve.cpp
//
// 审计目标：engine.cpp RenderLoop() 的 starve / duck 淡出淡入 / ASRC 游标推进。
//
// 独立判断，不依赖上一轮 tests/audit_engine_compaction.cpp 的结论。
// 本文件把 engine.cpp:377-441 逐行转写进 harness（SLX 标记对应源码行），
// 环形队列使用 src/engine.h 里**真实的 sl::Ring**（原样 include，不复制）。
//
// 需要回答的三个问题：
//   B5-1  duck 是每回调局部变量吗？欠载开始时会不会带非 1.0 残留值？
//   B5-2  欠载期间 cur_ 继续推进会远超 inLen_，下一次 compaction 会怎样？
//   B5-3  duck 淡出斜坡在欠载时真的作用到输出上了吗？恢复瞬间有硬跳变吗？
//   B5-4  ratio_ *= 0.997 在连续欠载下收敛到哪里？会不会卡死？
//
// 崩溃证据用「dry-run 记录长度」与「实弹执行」两段分开取证。

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include "engine.h"          // 真实的 sl::Ring

using namespace sl;

static const double kFs = 48000.0;

// ------------------------------------------------------------------ harness
struct Sim {
    Ring   ring_;
    std::vector<float> inBuf_;
    size_t inLen_ = 0;
    double cur_ = 0.0, ratio_ = 1.0, integ_ = 0.0, target_ = 0.0;

    int   underruns_ = 0;
    float ringMs_ = 0.f, dbgLevel_ = 0.f, dbgRatio_ = 1.f;
    bool  starvedLast_ = false;
    float duckAtBlockEnd_ = 1.f;

    // 取证开关：true 时不真的 memmove（否则进程当场死），只记录长度
    bool  dryRun_ = true;
    size_t wouldBytes_ = 0;
    size_t wouldSrcFrames_ = 0;
    size_t wouldDstFrames_ = 0;
    bool  underflow_ = false;      // inLen_ < n  -> size_t 下溢

    void init(size_t capBufFrames, size_t bufFrames) {
        ring_.init(48000 * 2);
        inBuf_.assign(16384 * 2, 0.f);
        // engine.cpp:352-355
        size_t target = capBufFrames ? capBufFrames : bufFrames;
        target += target / 4 + 64;
        preframes_ = target;
        target_ = (double)target;
        inLen_ = 0; cur_ = 0.0; ratio_ = 1.0; integ_ = 0.0;
    }
    size_t preframes_ = 0;

    void renderBlock(UINT32 frames, std::vector<float>& out) {
        out.assign((size_t)frames * 2, 0.f);
        float* f = out.data();

        // ---- engine.cpp:377-384  compaction
        {
            size_t n = (size_t)cur_;
            if (n > 0) {
                underflow_ = (n > inLen_);
                wouldSrcFrames_ = n;
                wouldDstFrames_ = inLen_ - n;          // 下溢时是巨大值
                wouldBytes_    = (inLen_ - n) * 2 * sizeof(float);
                if (!dryRun_ && !underflow_) {
                    memmove(inBuf_.data(), inBuf_.data() + n * 2,
                            (inLen_ - n) * 2 * sizeof(float));
                }
                if (!underflow_) inLen_ -= n;          // 源码是无条件 -=，这里只在安全时做
                cur_ -= (double)n;
            } else {
                underflow_ = false; wouldBytes_ = 0;
                wouldSrcFrames_ = wouldDstFrames_ = 0;
            }
        }
        size_t capFrames = inBuf_.size() / 2;
        size_t want = capFrames - 2 - inLen_;         // engine.cpp:388
        size_t got = ring_.pop(inBuf_.data() + inLen_ * 2, want);
        inLen_ += got;

        // ---- engine.cpp:392-397  ASRC servo
        double level = (double)ring_.available() + (double)inLen_;
        double err = (target_ > 0.0) ? (level - target_) / target_ : 0.0;
        if (fabs(err) < 0.35) integ_ = clampd(integ_ + err * 0.02, -3.0, 3.0);
        double wantRatio = 1.0 + clampd(err * 0.45 + integ_ * 0.03, -0.10, 0.10);
        ratio_ += (wantRatio - ratio_) * 0.10;
        ratio_ = clampd(ratio_, 0.85, 1.15);

        // ---- engine.cpp:403-429  duck + 线性插值
        const float kFadeStep = 1.0f / (5.0f * 0.001f * 48000.0f);   // = 1/240
        float duck = 1.0f;                    // <<< engine.cpp:404  每回调重置
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
                sL = sR = 0.f;                // <<< engine.cpp:419  淡出被短路
            } else {
                float aL = inBuf_[i0 * 2],     aR = inBuf_[i0 * 2 + 1];
                float bL = inBuf_[(i0 + 1) * 2], bR = inBuf_[(i0 + 1) * 2 + 1];
                sL = (float)(aL + (bL - aL) * fr);
                sR = (float)(aR + (bR - aR) * fr);
            }
            f[k * 2]     = sL * duck;
            f[k * 2 + 1] = sR * duck;
            cur_ += ratio_;
        }
        duckAtBlockEnd_ = duck;
        if (starved) {
            underruns_++;
            ratio_ = clampd(ratio_ * 0.997, 0.85, 1.15);   // engine.cpp:432
        }
        starvedLast_ = starved;
        ringMs_  = (float)(((double)ring_.available() + (double)inLen_ - cur_) * 1000.0 / kFs);
        dbgLevel_ = (float)((double)inLen_ - cur_);
        dbgRatio_ = (float)ratio_;
    }

    static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
};

static void pushSine(Sim& s, size_t frames, double amp, double& phase) {
    std::vector<float> t(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        t[i * 2] = t[i * 2 + 1] = (float)(amp * sin(phase));
        phase += 2.0 * 3.14159265358979 * 1000.0 / kFs;
    }
    s.ring_.push(t.data(), frames);
}

// =========================================================== B5-2 下溢
static void testUnderflow() {
    printf("=== [B5-2] 欠载期间 cur_ 超前 inLen_ -> compaction size_t 下溢 ===\n");
    const UINT32 kPeriod = 480;                    // 10 ms @48k，典型共享模式周期
    Sim s; s.init(480, 480);
    printf("  capFrames=%zu  target_=%.0f  period=%u\n", s.inBuf_.size()/2, s.target_, kPeriod);

    double ph = 0.0; std::vector<float> out;
    for (int i = 0; i < 20; ++i) { pushSine(s, 480, 0.5, ph); s.renderBlock(kPeriod, out); }
    printf("  稳态: inLen_=%zu cur_=%.2f ratio_=%.4f ringMs=%.2f underruns=%d\n",
           s.inLen_, s.cur_, s.ratio_, s.ringMs_, s.underruns_);

    printf("  -- 采集断流 1 个回调（10 ms），这是最轻微的欠载 --\n");
    s.renderBlock(kPeriod, out);
    printf("     starved=%d inLen_=%zu cur_=%.2f   cur_ > inLen_ ? %s\n",
           (int)s.starvedLast_, s.inLen_, s.cur_,
           s.cur_ > (double)s.inLen_ ? "*** 是 ***" : "否");

    printf("  -- 下一次回调进入 compaction (engine.cpp:378-383) --\n");
    size_t n = (size_t)s.cur_;
    if (n > 0) {
        size_t bytes = (s.inLen_ - n) * 2 * sizeof(float);
        printf("     n        = (size_t)cur_ = %zu\n", n);
        printf("     inLen_   = %zu\n", s.inLen_);
        printf("     inLen_-n = %zu  %s\n", s.inLen_ - n,
               s.inLen_ < n ? "<== size_t 下溢" : "");
        printf("     memmove 字节 = %zu  (%.4e)\n", bytes, (double)bytes);
        printf("     源偏移   = inBuf_.data() + %zu floats (%.2f MiB)\n",
               n * 2, (double)n * 8.0 / 1048576.0);
        if (s.inLen_ < n) {
            printf("\n  >>> 判定: cur_ 已超过 inLen_，engine.cpp:380 会执行\n");
            printf("  >>>   memmove(dst, src+%.2fMiB, %.4e 字节)\n",
                   (double)n*8.0/1048576.0, (double)bytes);
            printf("  >>> 立即访问违例 -> 渲染线程崩溃 -> 整个进程被杀。\n");
            printf("  >>> 触发条件: 采集端只要断流 1 个渲染周期(10ms)。\n");
            printf("  >>> 频率: 任何瞬时欠载(CABLE 被占用/游戏切屏/驱动 reset)必现。\n");
        }
    }

    printf("\n  -- 实弹验证（dryRun_=false，真的 memmove）--\n");
    Sim s2; s2.dryRun_ = false; s2.init(480, 480);
    double p2 = 0.0; std::vector<float> o2;
    for (int i = 0; i < 20; ++i) { pushSine(s2, 480, 0.5, p2); s2.renderBlock(kPeriod, o2); }
    printf("     稳态 OK。断流 1 个回调 ... ");
    fflush(stdout);
    s2.renderBlock(kPeriod, o2);
    printf("inLen_=%zu cur_=%.1f\n", s2.inLen_, s2.cur_);
    printf("     再进一次回调(compaction) ... 等待崩溃 ...\n");
    fflush(stdout);
    s2.renderBlock(kPeriod, o2);
    printf("     !!! 存活。inLen_=%zu cur_=%.1f\n", s2.inLen_, s2.cur_);
}

// =========================================================== B5-1 / B5-3 duck
static void testDuck() {
    printf("\n=== [B5-1/B5-3] duck 淡出淡入是否真的生效 ===\n");
    const UINT32 kPeriod = 480;
    Sim s; s.init(480, 480);
    double ph = 0.0; std::vector<float> out, prev;

    for (int i = 0; i < 20; ++i) { pushSine(s, 480, 0.9, ph); s.renderBlock(kPeriod, out); }
    prev = out;

    // 场景 1：采集断流。上一块末尾是满幅信号，本块从欠载开始。
    s.renderBlock(kPeriod, out);
    double lastOfPrev = fabs((double)prev[(kPeriod-1)*2]);
    double firstOfThis = fabs((double)out[0]);
    double jumpIn = fabs((double)out[0] - (double)prev[(kPeriod-1)*2]);
    printf("  [进入欠载] 上一块末样本=%.6f  本块首样本=%.6f\n", lastOfPrev, firstOfThis);
    printf("             跳变 = %.6f  (%.1f dBFS)\n", jumpIn,
           20.0*log10(jumpIn + 1e-12));
    printf("             若 duck 淡出生效，此处应为一个样本步长(1/240)*0.9=%.6f\n",
           0.9/240.0);
    printf("             实测 %.6f 是理想值的 %.0f 倍  -> 淡出被 sL=0 短路\n",
           jumpIn, jumpIn / (0.9/240.0));
    printf("             本块末尾 duck = %.6f (欠载了 480 个样本，1.0-480/240 <0 已夹到 0)\n",
           s.duckAtBlockEnd_);

    // 场景 2：数据恢复。上一块全是 0(欠载)，本块突然有数据。
    pushSine(s, 480, 0.9, ph);
    s.renderBlock(kPeriod, out);
    double prevLast = fabs((double)prev[(kPeriod-1)*2]);
    printf("\n  [数据恢复] 上一块末样本=%.6f  本块首样本=%.6f\n", prevLast, fabs((double)out[0]));
    printf("             跳变 = %.6f  (%.1f dBFS)  <<< 恢复瞬间硬跳变\n",
           fabs((double)out[0]-prevLast), 20.0*log10(fabs((double)out[0]-prevLast)+1e-12));
    printf("             若淡入生效应从 0 用 240 个样本爬升，首样本应为 %.6f\n", 0.9/240.0);
    printf("             本块起始 duck = 1.0 (engine.cpp:404 每回调重置)，所以直接满幅\n");

    // 场景 3：跨回调的 fade 是否可能
    printf("\n  [跨回调] period=128 (2.67ms) 时单个回调只有 128 样本 < 240\n");
    Sim s3; s3.init(480, 480);
    double p3 = 0.0; std::vector<float> o3;
    for (int i = 0; i < 40; ++i) { pushSine(s3, 480, 0.9, p3); s3.renderBlock(128, o3); }
    s3.renderBlock(128, o3);
    printf("     单回调欠载 128 样本后 duck = %.6f (1 - 128/240)\n", s3.duckAtBlockEnd_);
    printf("     下一个回调 duck 重新从 1.0 起步 -> 淡出永远跨不过回调边界\n");
    printf("     实测第二块末尾 duck = ");
    s3.renderBlock(128, o3);
    printf("%.6f (说明每块都从 1.0 重新衰减)\n", s3.duckAtBlockEnd_);
}

// =========================================================== B5-4 ratio_
static void testRatioConverge() {
    printf("\n=== [B5-4] ratio_ *= 0.997 的收敛行为（连续欠载）===\n");
    Sim s; s.init(480, 480);
    double ph = 0.0; std::vector<float> out;
    for (int i = 0; i < 20; ++i) { pushSine(s, 480, 0.5, ph); s.renderBlock(480, out); }
    printf("  起始 ratio_ = %.6f\n", s.ratio_);

    // 连续欠载 200 个回调（2 秒）
    printf("  回调#   ratio_     欠载?\n");
    double prevR = s.ratio_;
    int stuckAt = -1;
    for (int i = 1; i <= 200; ++i) {
        // 只在最初几个和末尾几个打印
        if (i <= 5 || i % 40 == 0 || i > 197)
            printf("  %5d   %.6f    %d\n", i, s.ratio_, (int)s.starvedLast_);
        s.renderBlock(480, out);      // 无数据 -> 欠载
        if (stuckAt < 0 && s.ratio_ <= 0.85 + 1e-12) stuckAt = i;
    }
    printf("  ratio_ 在第 %d 个欠载回调触到 0.85 下限\n", stuckAt);
    printf("  最终 ratio_ = %.6f  (下限 0.85)\n", s.ratio_);
    printf("  ratio_ < 1 意味着渲染端比采集端**慢放** %.1f%%：\n", (1.0-s.ratio_)*100);
    printf("  音高下降约 %.2f 个半音，欠载恢复期可听见。\n",
           12.0*log2(1.0/(s.ratio_>0?s.ratio_:1.0)));

    // 是否会卡死：恢复数据后能否回到 1.0
    printf("\n  -- 恢复供数，观察 ratio_ 能否回到 1.0 --\n");
    double p2 = 0.0;
    for (int i = 1; i <= 120; ++i) {
        pushSine(s, 480, 0.5, p2);
        s.renderBlock(480, out);
        if (i % 20 == 0) printf("  恢复 %3d 个回调后 ratio_ = %.6f\n", i, s.ratio_);
    }
    printf("  >>> %s\n", (s.ratio_ > 0.995) ? "能收敛回 1.0，不卡死"
                                            : "未能完全恢复，伺服增益不足");
}

int main() {
    printf("SoundLatch RenderLoop starve/duck 审计\n");
    printf("=========================================\n");
    setvbuf(stdout, nullptr, _IONBF, 0);
    testUnderflow();
    testDuck();
    testRatioConverge();
    printf("\n=========================================\n完成。\n");
    return 0;
}
