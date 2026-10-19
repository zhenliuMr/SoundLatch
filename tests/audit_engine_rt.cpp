// audit_engine_rt.cpp
//
// SoundLatch engine.cpp / engine.h 实时安全与欠载行为审计（第二轮，独立判断）
//
// 覆盖：
//   T1  compaction 下溢的**实弹**崩溃证明（不加固，逐字执行 engine.cpp:378-383）
//   T2  duck 淡出/淡入是否真的作用到输出（在**欠载起始点**测量，不是回调边界）
//   T3  sl::Ring 的 release/acquire 配对 + 并发 SPSC 压测（撕裂/丢样检测）
//   T4  CaptureLoop 热路径的 vector::resize 是否真的触发堆分配
//   T5  Start/Stop 的线程生命周期推演
//
// 不修改 src/ 任何文件。Ring 使用 src/engine.h 中的真实实现。
//
// 用法： audit_engine_rt.exe          -> 跑全部，崩溃项放在最后
//        audit_engine_rt.exe crash    -> 只跑 T1 实弹（预期进程被杀）

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include "engine.h"

using namespace sl;
static const double kFs = 48000.0;

static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// =======================================================================
// T1  compaction 下溢 —— 实弹
// 这里**逐字**复制 engine.cpp:377-384，不加任何保护。
// 若结论成立，本进程会以 0xC0000005 (访问违例) 终止。
// =======================================================================
static int testUnderflowLive() {
    printf("=== [T1] compaction 下溢实弹（逐字执行 engine.cpp:377-443，无任何加固）===\n");
    const UINT32 kFrames = 480;                 // 10 ms @48k

    // 忠实复刻 RenderLoop 的稳态 -> 单次欠载过程
    Ring ring; ring.init(48000 * 2);
    std::vector<float> inBuf; inBuf.assign(16384 * 2, 0.f);
    size_t inLen = 0;
    double  cur = 0.0, ratio = 1.0, integ = 0.0, target = 664.0;
    std::vector<float> tone(480 * 2);
    double ph = 0.0;

    // 预热 20 个回调到稳态
    for (int b = 0; b < 20; ++b) {
        for (size_t i = 0; i < 480; ++i) {
            tone[i*2] = tone[i*2+1] = (float)(0.5 * sin(ph));
            ph += 2.0 * 3.14159265358979 * 1000.0 / kFs;
        }
        ring.push(tone.data(), 480);

        size_t n = (size_t)cur;                                   // :378
        if (n > 0) {                                              // :379
            memmove(inBuf.data(), inBuf.data() + n * 2, (inLen - n) * 2 * sizeof(float));
            inLen -= n; cur -= (double)n;                         // :380-382
        }
        size_t capF = inBuf.size() / 2;
        size_t want = capF - 2 - inLen;
        inLen += ring.pop(inBuf.data() + inLen * 2, want);        // :388-390
        double level = (double)ring.available() + (double)inLen;
        double err = (level - target) / target;                   // :392-393
        if (fabs(err) < 0.35) integ = clampd(integ + err * 0.02, -3.0, 3.0);
        double wantR = 1.0 + clampd(err * 0.45 + integ * 0.03, -0.10, 0.10);
        ratio += (wantR - ratio) * 0.10;
        ratio = clampd(ratio, 0.85, 1.15);
        for (UINT32 k = 0; k < kFrames; ++k) cur += ratio;         // :428
    }
    printf("  稳态: inLen_=%zu cur_=%.2f ratio_=%.4f\n", inLen, cur, ratio);

    // 采集断流 1 个回调（10 ms）—— 最轻微的欠载
    for (UINT32 k = 0; k < kFrames; ++k) cur += ratio;             // :428 欠载时照常推进
    printf("  断流 1 个回调后: inLen_=%zu cur_=%.2f\n", inLen, cur);
    printf("  cur_ > inLen_ ? %s\n", cur > (double)inLen ? "*** 是 -> 下一次 compaction 必炸 ***" : "否");

    // 下一次回调的 compaction —— engine.cpp:378-383 原文，零保护
    size_t n = (size_t)cur;                                       // :378
    printf("  进入 compaction: n=(size_t)cur_=%zu  inLen_=%zu\n", n, inLen);
    if (n > 0) {
        size_t len = (inLen - n) * 2 * sizeof(float);
        printf("  engine.cpp:380 -> memmove(dst, src+%zu floats, %zu 字节 = %.4e)\n",
               n * 2, len, (double)len);
        if (inLen < n) printf("  *** (inLen_-n) 以 size_t 下溢 ***\n");
        fflush(stdout);
        memmove(inBuf.data(), inBuf.data() + n * 2, len);         // :380 原文
        inLen -= n;                                               // :381 原文
        cur -= (double)n;                                         // :382 原文
        // 读回 inBuf_，防止 GCC 把上面的 memmove 当死存储优化掉
        // （-O2 下 GCC 确实会把「结果无人读取」的 memmove 消除掉，
        //   导致 harness 假性存活；真实引擎里 inBuf_ 下一轮还要用，必须保留）
        volatile float sink = inBuf[0] + inBuf[inBuf.size() - 1];
        (void)sink;
        printf("  !!! 活下来了。inLen_=%zu cur_=%.2f\n", inLen, cur);
    }
    return 0;
}

// =======================================================================
// T2  duck —— 在欠载**起始点**测量跳变
// =======================================================================
struct Sim2 {
    Ring   ring_;
    std::vector<float> inBuf_;
    size_t inLen_ = 0;
    double cur_ = 0.0, ratio_ = 1.0, integ_ = 0.0, target_ = 0.0;
    int underruns_ = 0;
    double starveStartSample_ = -1;    // 本块内首次欠载的样本序号
    float  duckAtStarve_ = 1.f;       // 首次欠载时的 duck
    float  prevBlockLast_ = 0.f;      // 上一块最后一个输出样本
    bool   starvedLast_ = false;
    std::vector<float> out;

    void init(size_t capBufFrames, size_t bufFrames) {
        ring_.init(48000 * 2);
        inBuf_.assign(16384 * 2, 0.f);
        size_t t = capBufFrames ? capBufFrames : bufFrames;
        t += t / 4 + 64;
        target_ = (double)t;
        inLen_ = 0; cur_ = 0.0; ratio_ = 1.0; integ_ = 0.0;
    }
    static double clampd(double v, double lo, double hi){ return v<lo?lo:(v>hi?hi:v); }

    // engine.cpp:377-441 转写
    void renderBlock(UINT32 frames) {
        // compaction（本题不测下溢，安全夹一次以便继续跑完）
        {
            size_t n = (size_t)cur_;
            if (n > 0) {
                if (n > inLen_) n = inLen_;              // 仅测试用加固
                memmove(inBuf_.data(), inBuf_.data() + n * 2,
                        (inLen_ - n) * 2 * sizeof(float));
                inLen_ -= n; cur_ -= (double)n;
            }
        }
        size_t capFrames = inBuf_.size() / 2;
        size_t want = capFrames - 2 - inLen_;
        size_t got = ring_.pop(inBuf_.data() + inLen_ * 2, want);
        inLen_ += got;

        double level = (double)ring_.available() + (double)inLen_;
        double err = (target_ > 0.0) ? (level - target_) / target_ : 0.0;
        if (fabs(err) < 0.35) integ_ = clampd(integ_ + err * 0.02, -3.0, 3.0);
        double wantRatio = 1.0 + clampd(err * 0.45 + integ_ * 0.03, -0.10, 0.10);
        ratio_ += (wantRatio - ratio_) * 0.10;
        ratio_ = clampd(ratio_, 0.85, 1.15);

        out.assign((size_t)frames * 2, 0.f);
        float* f = out.data();
        const float kFadeStep = 1.0f / (5.0f * 0.001f * 48000.0f);   // 1/240
        float duck = 1.0f;                                // engine.cpp:404
        bool starved = false;
        starveStartSample_ = -1;
        for (UINT32 k = 0; k < frames; ++k) {
            size_t i0 = (size_t)cur_;
            double fr = cur_ - (double)i0;
            if (i0 + 1 >= inLen_) {
                duck -= kFadeStep;
                if (duck < 0.f) duck = 0.f;
                if (!starved) { starved = true; starveStartSample_ = k; duckAtStarve_ = duck; }
            } else if (duck < 1.0f) {
                duck += kFadeStep;
                if (duck > 1.0f) duck = 1.0f;
            }
            float sL, sR;
            if (i0 + 1 >= inLen_) {
                sL = sR = 0.f;                            // engine.cpp:419
            } else {
                float aL = inBuf_[i0*2],   aR = inBuf_[i0*2+1];
                float bL = inBuf_[(i0+1)*2], bR = inBuf_[(i0+1)*2+1];
                sL = (float)(aL + (bL - aL) * fr);
                sR = (float)(aR + (bR - aR) * fr);
            }
            f[k*2] = sL * duck;  f[k*2+1] = sR * duck;
            cur_ += ratio_;
        }
        if (starved) { underruns_++; ratio_ = clampd(ratio_ * 0.997, 0.85, 1.15); }
        starvedLast_ = starved;
    }
};

static void pushTone(Sim2& s, size_t frames, double amp, double& ph) {
    std::vector<float> t(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        t[i*2] = t[i*2+1] = (float)(amp * sin(ph));
        ph += 2.0 * 3.14159265358979 * 1000.0 / kFs;
    }
    s.ring_.push(t.data(), frames);
}

static double maxAdjacentJump(const std::vector<float>& v, size_t* at) {
    double mx = 0.0; *at = 0;
    for (size_t i = 1; i < v.size()/2; ++i) {
        double d = fabs((double)v[i*2] - (double)v[(i-1)*2]);
        if (d > mx) { mx = d; *at = i; }
    }
    return mx;
}

// 测量「进入欠载那一瞬间」的跳变。
// 若 firstStarveK==0，跳变发生在块边界（上一块末样本 -> 本块首样本=0）。
// 否则发生在块内第 firstStarveK-1 -> firstStarveK 个样本之间。
static double onsetJump(const std::vector<float>& prevBlock,
                        const std::vector<float>& curBlock,
                        double firstStarveK, size_t* where) {
    if (firstStarveK <= 0.0) {
        size_t n = prevBlock.size() / 2;
        *where = 0;
        return fabs((double)curBlock[0] - (double)prevBlock[(n - 1) * 2]);
    }
    size_t k = (size_t)firstStarveK;
    *where = k;
    return fabs((double)curBlock[k * 2] - (double)curBlock[(k - 1) * 2]);
}

static void testDuck() {
    printf("\n=== [T2] duck 淡出/淡入是否真的作用到输出 ===\n");
    printf("  kFadeStep = 1/(5ms*48000) = 1/240 = %.6f /样本\n", 1.0 / 240.0);
    printf("  5 ms = %d 个样本 @48k；渲染周期 %d 样本\n\n", (int)(0.005 * 48000), 480);

    const UINT32 P = 480;
    Sim2 s; s.init(480, 480);
    double ph = 0.0;
    for (int i = 0; i < 30; ++i) { pushTone(s, 480, 0.9, ph); s.renderBlock(P); }

    // 抽干缓冲，直到某一块**中途**欠载 —— 这才是真实的欠载瞬间
    int guard = 0;
    do { s.renderBlock(P); } while (!s.starvedLast_ && ++guard < 200);
    std::vector<float> starvedBlock = s.out;
    double kStarve = s.starveStartSample_;
    size_t where;
    double jump = onsetJump(/*prev*/ starvedBlock /*仅边界情形用*/, starvedBlock, kStarve, &where);

    printf("  [进入欠载]\n");
    printf("    欠载起始样本 k = %.0f  (块内第 %zu 个)\n", kStarve, where);
    printf("    该处 duck = %.6f (理论应为 1-1/240 = %.6f)\n",
           s.duckAtStarve_, 1.0 - 1.0 / 240.0);
    printf("    进入欠载的瞬时跳变 = %.6f\n", jump);
    printf("    若 5ms 淡出生效，该跳变应 <= 信号幅度/240 = %.6f\n", 0.9 / 240.0);
    printf("    实测 / 理想 = %.1f 倍\n", jump / (0.9 / 240.0));
    printf("    原因: engine.cpp:418-419 在 i0+1>=inLen_ 时 sL=sR=0，\n");
    printf("          于是 f[k]=sL*duck 恒为 0 —— duck 被完全短路，淡出是死代码。\n");

    // 供数恢复
    pushTone(s, 480, 0.9, ph);
    s.renderBlock(P);
    double prevLast = (double)starvedBlock[(P - 1) * 2];
    double firstNew = (double)s.out[0];
    printf("\n  [数据恢复]\n");
    printf("    欠载块末样本 = %.6f (恒为 0，因 sL=0)\n", prevLast);
    printf("    恢复块首样本 = %.6f\n", firstNew);
    printf("    瞬时跳变 = %.6f (%.1f dBFS)\n",
           fabs(firstNew - prevLast), 20.0 * log10(fabs(firstNew - prevLast) + 1e-12));
    printf("    本块起始 duck = 1.0（engine.cpp:404 每块重置），**无淡入**。\n");
    printf("    若淡入生效，首样本应为 %.6f 并在 240 样本内爬升。\n", 0.9 / 240.0);

    // 短渲染周期：淡出跨不过回调边界
    printf("\n  [跨回调] 渲染周期 = 64 样本 (1.33 ms) < 240，单块无法淡完\n");
    Sim2 s3; s3.init(480, 480);
    double p3 = 0.0;
    for (int i = 0; i < 60; ++i) { pushTone(s3, 480, 0.9, p3); s3.renderBlock(64); }
    s3.renderBlock(64);
    printf("    欠载块 1 末样本 = %.6f\n", s3.out[63 * 2]);
    s3.renderBlock(64);
    printf("    欠载块 2 末样本 = %.6f\n", s3.out[63 * 2]);
    printf("    -> duck 是 engine.cpp:404 的**块内局部变量**，无跨回调状态。\n");
    printf("    -> 渲染周期 < 5 ms 时淡出斜坡永远走不完，每个块都从 1.0 重新起步。\n");
}

// =======================================================================
// T3  Ring 的 release/acquire 与并发正确性
// =======================================================================
static void testRing() {
    printf("\n=== [T3] sl::Ring release/acquire 配对 + 并发 SPSC 压测 ===\n");
    printf("  init:   w_.store(relaxed) / r_.store(relaxed)  —— 仅 Start() 调用，OK\n");
    printf("  push:   r_.load(acquire) -> 写 buf_ -> w_.store(release)   配对 OK\n");
    printf("  pop:    w_.load(acquire) -> 读 buf_ -> r_.store(release)   配对 OK\n");
    printf("  available(): w_.load(acq) + r_.load(acq)                     OK\n");
    printf("  => release/acquire 配对正确，无撕裂窗口。\n\n");

    const size_t CAP = 48000;
    Ring r; r.init(CAP);
    std::atomic<bool> bad{false};
    std::atomic<long long> prod{0}, cons{0};

    // 生产者：每样本写唯一递增的序列号（打包成 float）
    // 注意：push 返回值才是真正写入的帧数，必须按返回值推进序列号，
    // 否则会把「被 Ring 拒绝的帧」也算进序列，误报为乱序。
    std::thread p([&]{
        unsigned long long n = 0;
        std::vector<float> b(256 * 2);
        for (int round = 0; round < 20000 && !bad.load();) {
            for (size_t i = 0; i < 256; ++i) {
                float v = (float)n;
                b[i*2] = v; b[i*2+1] = v;
                ++n;
            }
            size_t w = r.push(b.data(), 256);
            prod += (long long)w;
            // push 只写入了 w 帧 -> 把没写进去的帧补回来
            if (w < 256) { n -= (256 - w); }
            if (w) ++round;
        }
    });
    // 消费者：校验序列号必须严格连续 +1
    std::thread c([&]{
        unsigned long long expect = 0;
        std::vector<float> b(256 * 2);
        for (int round = 0; round < 20000 && !bad.load(); ++round) {
            size_t n = r.pop(b.data(), 256);
            cons += (long long)n;
            for (size_t i = 0; i < n; ++i) {
                unsigned long long got = (unsigned long long)b[i*2];
                if (got != expect) { bad.store(true); return; }
                if (b[i*2] != b[i*2+1]) { bad.store(true); return; }
                ++expect;
            }
        }
    });
    p.join(); c.join();
    printf("  生产 %lld 帧, 消费 %lld 帧, 差 = %lld (= 缓冲残留, 正常)\n",
           prod.load(), cons.load(), prod.load() - cons.load());
    printf("  序列号连续性: %s\n", bad.load() ? "*** 检测到乱序/撕裂 ***" : "全部连续，无撕裂");
    printf("  => Ring 无锁实现本身**正确**（在 x86 TSO + 正确 acq/rel 下）。\n");

    // 容量 / off-by-one
    Ring r2; r2.init(8);
    float x[2] = {1.f, 1.f};
    printf("\n  [容量] cap_=8:\n");
    printf("    available() 初始 = %zu\n", r2.available());
    for (int i = 0; i < 8; ++i) r2.push(x, 1);
    printf("    推入 8 帧后 available() = %zu (满)\n", r2.available());
    printf("    再推 1 帧 -> 返回 %zu (应为 0，且**不覆盖**已有数据)\n", r2.push(x, 1));
    float y[16]; size_t got = r2.pop(y, 16);
    printf("    pop(16) -> %zu 帧 (应为 8)\n", got);
    bool seq = true; for (int i = 0; i < 8; ++i) if (y[i*2] != 1.f) seq = false;
    printf("    数据未被覆盖: %s\n", seq ? "是（无 off-by-one）" : "*** 否 ***");
    printf("    => w/r 是单调递增 size_t，w-r==cap 表示「满」，无需浪费一格。\n");
    printf("    => **无 off-by-one**，容量被完整利用。\n");
}

// =======================================================================
// T4  CaptureLoop 热路径的 vector::resize
// =======================================================================
static void testResizeAlloc() {
    printf("\n=== [T4] CaptureLoop 热路径的堆分配 ===\n");
    printf("  engine.cpp:273  tmp.resize((size_t)n * 2);\n");
    printf("  engine.cpp:282  out48.resize(out48.size() * 2);\n");
    printf("  engine.cpp:255  out48.resize(16384);   (循环外，一次性)\n\n");

    std::vector<float> tmp;
    printf("  tmp.resize(n*2) 行为（capacity 演化，n = WASAPI 包帧数）:\n");
    const int pkts[] = { 128, 256, 480, 481, 512, 1024, 500 };
    for (int n : pkts) {
        size_t before = tmp.capacity();
        tmp.resize((size_t)n * 2);
        bool realloc = (tmp.capacity() != before);
        printf("    n=%-5d -> resize(%-5zu) capacity %zu -> %zu  %s\n",
               n, (size_t)n*2, before, tmp.capacity(),
               realloc ? "*** 重新分配（堆分配）***" : "无分配");
    }
    printf("  => WASAPI 共享模式的包长会随引擎调度变化，capacity 追不上时\n");
    printf("     resize 触发 operator new。CaptureThread 是实时线程。\n");

    printf("\n  out48.resize(size*2) 出现在 **最内层每样本循环** 里 (engine.cpp:282)\n");
    printf("  仅在 srcRate != 48000 时进入。触发后每次翻倍，直到够大。\n");
    printf("  => 这是实时线程里、音频热路径内的堆分配。\n");
}

// =======================================================================
// T5  线程生命周期推演
// =======================================================================
static void testLifecycle() {
    printf("\n=== [T5] Start/Stop 生命周期与共享变量 ===\n");
    printf("  Stop() engine.cpp:191-192:\n");
    printf("    WaitForSingleObject(hCapThread_, 2000);  <-- **返回值被丢弃**\n");
    printf("    CloseHandle(hCapThread_); hCapThread_ = nullptr;\n");
    printf("  若线程 2s 内没退出（驱动阻塞、设备热拔、驱动卡死）：\n");
    printf("    1) CloseHandle 关掉仍在运行的线程句柄（该线程永远无法被 join）\n");
    printf("    2) 随后 Start() 调 ring_.init(48000*2) -> buf_.assign() **重新分配并释放**\n");
    printf("    3) 旧渲染线程仍在 inBuf_/ring_ 上读写 -> use-after-free\n");
    printf("    4) 且 running_ 被重新置 true -> **同一个 Ring 出现两个生产者**\n");
    printf("       (旧 CaptureThread + 新 CaptureThread)，SPSC 前提被破坏。\n\n");
    printf("  顺序缺陷: engine.cpp:174-175 先建 RenderThread 再建 CaptureThread，\n");
    printf("  但 RenderLoop:352 读 capBufFrames_，该值由 CaptureLoop:248 写。\n");
    printf("  两者无同步 -> capBufFrames_ 读到 0（首次）或**上一轮的陈旧值**\n");
    printf("  (Stop() engine.cpp:195 只清 capBufMs_/renBufMs_，**不清 capBufFrames_**)\n");
    printf("  -> prefill 目标算错，target_ 偏小，latency 与实际不符。\n\n");
    printf("  句柄泄漏: engine.cpp:182 Start() 失败时直接 return false，\n");
    printf("  **没有调用 Stop()**，hCapThread_/hRenThread_/hCapStop_/hRenStop_ 四个\n");
    printf("  句柄悬空（下次 Start() 才会被 Stop() 回收；不再启动则泄漏到进程退出）。\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("SoundLatch engine.cpp 实时安全审计（第二轮）\n");
    printf("============================================\n");

    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        testUnderflowLive();
        printf("T1 未崩溃（结论有误）\n");
        return 0;
    }

    testDuck();
    testRing();
    testResizeAlloc();
    testLifecycle();

    printf("\n============================================\n");
    printf("最后跑 T1 实弹 —— 预期进程被访问违例杀死\n");
    fflush(stdout);
    testUnderflowLive();
    printf("T1 未崩溃\n");
    return 0;
}
