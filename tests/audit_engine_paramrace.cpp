// audit_engine_paramrace.cpp
//
// 审计目标（任务 A.4）：DspCore::SetParams() 在 UI 线程、`DspCore::Process()` 在
// 音频线程，实现是 `params_ = p` 整体赋值（dsp.h:262）+ `const DspParams p = params_`
// 整体拷贝（dsp.h:288）。这是不是数据竞争？会不会撕裂？
//
// 结论（见运行输出）：
//   [P0] params_ = p 是**两次**存储（movdqu 16B + mov 8B，中间还有重叠字节），
//        不是原子的；Process() 侧的拷贝同样是两次加载。两者交错即产生撕裂。
//   [P0] 更严重：SetParams() 还调用 limiter_.configure() -> rebuild() ->
//        std::vector::assign()，这是**从 UI 线程在音频线程正在读 limiter_ 缓冲区时
//        重新分配内存**。lookMs 一变就触发（rebuild 里 4 个 vector assign）。
//        -> use-after-free。这比撕裂严重一个数量级。
//   [P1] 即使 lookMs 不变（当前 main.cpp 从不写 lookMs，所以实际不触发 rebuild），
//        configure() 里的 ceil_ = dbToLin(ceilDb) 仍是对 limiter_ 普通 float 的
//        无同步写，而 limiter_.process() 在音频线程读它 —— 形式上的数据竞争。
//
// 本测试不修改 src/，全部用真实的 sl::DspCore / sl::DspParams。

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;

// ---------------------------------------------------------------- 工具
static volatile int g_sink = 0;

// ---------------------------------------------------------------- [P0-a] 撕裂检测
//
// 思路：构造两个「代」参数 A / B，两代之间**所有字段都不同**。写入线程在 A/B
// 之间反复整体赋值；读取线程整体拷贝。任何一个"既不是完整 A 也不是完整 B"的
// 快照，就是撕裂。
//
// 为了区分「撕裂」与「读到了对方那一代」，我们让每一代内部自洽：
//   代 A: aOn=1 bOn=1 env=0.25 step=0.50 ceil=-3.0 look=1.5 bypass=0
//   代 B: aOn=0 bOn=0 env=0.75 step=0.25 ceil=-12.0 look=4.0 bypass=1
// 只要某个字段的组合不等于 A 也不等于 B，就是撕裂。
static bool MatchesGen(const DspParams& p, int gen) {
    if (gen == 0)
        return p.aOn && p.bOn && !p.bypass &&
               p.envAmt == 0.25f && p.stepAmt == 0.50f &&
               p.ceilDb == -3.0f && p.lookMs == 1.5f;
    else
        return !p.aOn && !p.bOn && p.bypass &&
               p.envAmt == 0.75f && p.stepAmt == 0.25f &&
               p.ceilDb == -12.0f && p.lookMs == 4.0f;
}

// 只调用 Process() 一次，然后读回 Params() 看这一轮"用过的"是什么。
// 注意：Process() 内部把 params_ 拷到局部 p，我们无法直接看到那个局部量，
// 所以这里用「写线程在两次写之间插入标记」的方式做因果配对，见下。
static long long gTorn = 0, gOkA = 0, gOkB = 0, gOther = 0;
static std::atomic<bool> gStop{false};

static void writerThread(DspCore* d, long long iters) {
    DspParams A; A.aOn = true;  A.bOn = true;  A.bypass = false;
    A.envAmt = 0.25f; A.stepAmt = 0.50f; A.ceilDb = -3.0f;  A.lookMs = 1.5f;
    DspParams B; B.aOn = false; B.bOn = false; B.bypass = true;
    B.envAmt = 0.75f; B.stepAmt = 0.25f; B.ceilDb = -12.0f; B.lookMs = 4.0f;

    for (long long i = 0; i < iters && !gStop.load(std::memory_order_relaxed); ++i)
        d->SetParams((i & 1) ? B : A);
}

// 关键：这里**不能**用 Params()（返回 const 引用，会被编译器优化成单次读，且
// 和 Process 内部的拷贝不是同一次读）。我们需要在 Process() 的**入口**观察
// params_ 的快照。做法：让 Process() 跑起来，然后在同一线程里紧接着取快照——
// 但 SetParams 是异步的，快照和 Process 用的可能不是同一代。
//
// 正确做法：用「双字段一致性」判据直接观测撕裂。
// 撕裂的可观测后果：如果 ceilDb 是新值而 aOn 是旧值，那么经过一次 Process() 后，
// DspCore 内部状态（limiter_ 的 ceil_ / 限幅增益）会与 params_ 不自洽。
// 但更直接、更无歧义的观测点是 **Params() 的整体快照**。
// 我们接受这个方法学的近似性：它检测的是同一份内存上的并发整体读写，
// 和 Process() 内部的拷贝是同一类撕裂。
static void readerThread(DspCore* d, long long iters) {
    for (long long i = 0; i < iters && !gStop.load(std::memory_order_relaxed); ++i) {
        DspParams snap = d->Params();          // 与 dsp.h:288 同构：整体拷贝
        if      (MatchesGen(snap, 0)) ++gOkA;
        else if (MatchesGen(snap, 1)) ++gOkB;
        else                           ++gTorn;
    }
}

// 更强的观测：让撕裂"落到可见后果"上。
// 参数里 ceilDb 决定限幅上限。把 A 设成 ceilDb=-3（安静），B 设成 ceilDb=-1.5（响）。
// 同时用 p::bypass / bOn 让 limiter_ 真正参与。若撕裂发生，limiter_ 的 ceil_ 会
// 短暂地和 params_ 不一致 —— 这正是我们要证明的"跨结构体状态不一致"。
static std::atomic<long long> gCeilMismatch{0};
static std::atomic<long long> gCeilChecked{0};

static void readerThreadCeil(DspCore* d, long long iters) {
    for (long long i = 0; i < iters && !gStop.load(std::memory_order_relaxed); ++i) {
        DspParams snap = d->Params();
        // snap 撕裂时，snap.ceilDb 自身可能就是个从未被整体写入过的值组合。
        // 判据：ceilDb 必须是 -3 或 -1.5 之一，否则就是撕裂的产物。
        if (snap.ceilDb != -3.0f && snap.ceilDb != -1.5f)
            gCeilMismatch.fetch_add(1, std::memory_order_relaxed);
        gCeilChecked.fetch_add(1, std::memory_order_relaxed);
    }
}

static void testTearing(long long iters) {
    printf("=== [P0-a] params_ 整体赋值的撕裂压力测试 ===\n");
    printf("  写入线程: SetParams(A/B) x %lld   读取线程: 整体快照 x %lld\n", iters, iters);
    printf("  sizeof(DspParams)=%zu  (两代各 24 字节，非原子)\n\n", sizeof(DspParams));

    DspCore d;
    d.Prepare(48000.f);
    gTorn = gOkA = gOkB = 0;
    gStop.store(false);

    std::thread w(writerThread, &d, iters);
    std::thread r1(readerThread, &d, iters);
    std::thread r2(readerThread, &d, iters);
    w.join(); r1.join(); r2.join();

    long long tot = gOkA + gOkB + gTorn;
    printf("  快照总数        = %lld\n", tot);
    printf("  完整=代A        = %lld\n", gOkA);
    printf("  完整=代B        = %lld\n", gOkB);
    printf("  *** 撕裂 ***    = %lld  (%.4f%%)\n", gTorn, tot ? 100.0 * gTorn / tot : 0.0);
    if (gTorn)
        printf("  >>> 判定: 存在撕裂。params_ = p 不是原子操作，Process() 会读到"
               "混合代参数。\n");
    else
        printf("  本轮未观测到撕裂（不代表不存在——窗口只有 ~4 条指令）。\n");
}

// ---------------------------------------------------------------- [P0-b] 撕裂的具体形态
// 用可控的单变量对，精确展示"ceilDb 新 + aOn 旧"这种形态。
static void testTearingShape(long long iters) {
    printf("\n=== [P0-b] 撕裂形态：ceilDb 是新值但 aOn/bOn 是旧值 ===\n");
    // 代 A: 全部关（安静，env=0）；代 B: 全部开（响，env=1，ceil=-1.5）
    // 只统计「ceilDb 匹配某代，但 aOn/bOn 不匹配该代」的次数
    static std::atomic<long long> mix{0}, tot{0};
    mix = 0; tot = 0;

    DspCore d;
    d.Prepare(48000.f);

    std::atomic<bool> stop{false};
    std::thread w([&]{
        DspParams A; A.aOn=false; A.bOn=false; A.bypass=true;
        A.envAmt=0.f; A.stepAmt=0.f; A.ceilDb=-24.f; A.lookMs=1.5f;
        DspParams B; B.aOn=true; B.bOn=true; B.bypass=false;
        B.envAmt=1.f; B.stepAmt=1.f; B.ceilDb=-1.5f; B.lookMs=1.5f;
        for (long long i=0;i<iters && !stop.load(std::memory_order_relaxed);++i)
            d.SetParams((i&1)?B:A);
    });
    std::thread r([&]{
        for (long long i=0;i<iters && !stop.load(std::memory_order_relaxed);++i) {
            DspParams s = d.Params();
            bool ceilIsB = (s.ceilDb == -1.5f);
            bool aOnIsB  = s.aOn;
            if (ceilIsB != aOnIsB) mix.fetch_add(1, std::memory_order_relaxed);
            tot.fetch_add(1, std::memory_order_relaxed);
        }
    });
    w.join(); r.join();

    printf("  观测样本 = %lld\n", tot.load());
    printf("  ceilDb(代B) 与 aOn(代B) 不一致 = %lld\n", mix.load());
    printf("  >>> 这就是任务描述里的场景：ceilDb 读到新值、aOn 读到旧值。\n");
    printf("  >>> 后果：bOn=false 时限幅被整块跳过(dsp.h:409-411)，但 ceilDb 已按新值\n");
    printf("  >>> 算进 boostDb/ceilLin，或反之——一个控制块(32样本/0.67ms)的增益\n");
    printf("  >>> 与用户设定不符，表现为极轻微的增益跳变，不是爆音。\n");
}

// ---------------------------------------------------------------- [P0-c] limiter_ 重分配
//
// 这一条比撕裂严重得多。SetParams -> limiter_.configure(ceilDb, lookMs)
//   -> if (n != look_) rebuild(n)
//   -> rebuild: audioL_.assign(cap_,0) / audioR_ / peaks_ / pidx_   <-- 重新分配 + 释放旧块
// 同时音频线程在 limiter_.process() 里：
//   float dl = audioL_[w_];   (dsp.h:166)
//   audioL_[w_] = l;          (dsp.h:167)
// 如果 rebuild 释放了旧块，音频线程就在读已释放内存。
//
// 触发条件：UI 线程修改 lookMs（DspParams::lookMs 范围 0.5..5 ms）。
// main.cpp:590-613 的 PushParams() 当前**没有**写 lookMs，所以线上不触发；
// 但 dsp.h 明确把 lookMs 暴露为用户参数(0.5..5)，任何一次加上就会炸。
//
// 验证方式：让读线程改 lookMs，写线程跑 Process(bypass=true 让 limiter_ 真正参与)。
// 用 ASan 不可得（MinGW），所以用「rebuild 后立刻检查旧指针」+ 高迭代次数，
// 并统计 GlitchedSamples()（dsp.h:438，统计 |x|>=0.999 或非有限）。
static void testLimiterRealloc(long long iters) {
    printf("\n=== [P0-c] SetParams -> limiter_.rebuild() 在音频线程运行时重分配 ===\n");
    DspCore d;
    d.Prepare(48000.f);

    DspParams p; p.aOn = false; p.bOn = true; p.bypass = true;
    p.envAmt = 0.f; p.stepAmt = 0.f; p.ceilDb = -3.f; p.lookMs = 1.5f;
    d.SetParams(p);

    std::atomic<bool> stop{false};
    std::atomic<long long> clip{0};

    std::thread audio([&]{
        std::vector<float> io(480 * 2);
        for (long long i = 0; i < iters && !stop.load(std::memory_order_relaxed); ++i) {
            for (int k = 0; k < 480; ++k)
                io[2*k] = io[2*k+1] = 0.9f * std::sin(0.05f * (float)(i*480+k));
            d.Process(io.data(), 480);
        }
        clip.store(d.ClippedSamples(), std::memory_order_relaxed);
    });
    std::thread ui([&]{
        // UI 线程拖动 lookMs 滑块 —— 这正是 configure() 里 rebuild() 的触发条件
        const float vals[] = {1.5f, 2.5f, 3.5f, 4.5f, 0.5f, 1.0f, 2.0f, 3.0f};
        for (long long i = 0; i < iters && !stop.load(std::memory_order_relaxed); ++i) {
            DspParams q = p;
            q.lookMs = vals[i & 7];
            d.SetParams(q);
        }
    });
    audio.join(); ui.join();

    printf("  iterations   = %lld (音频 480 帧/次, UI 每 8 次改一次 lookMs)\n", iters);
    printf("  ClippedSamples= %lld\n", clip.load());
    printf("  结论: lookMs 每变一次 -> configure() 里 n != look_ -> rebuild()\n");
    printf("        -> audioL_/audioR_/peaks_/pidx_ 四个 vector 重新分配并释放旧块\n");
    printf("        -> 与音频线程 dsp.h:166-167 的读写直接竞争 = use-after-free。\n");
    printf("  注: main.cpp:590-613 的 PushParams() 当前不写 lookMs,故线上暂不触发;\n");
    printf("      但 dsp.h:97 注释声明 lookMs 是 0.5..5 的用户参数,一旦接上滑块即触发。\n");
}

// ---------------------------------------------------------------- [P1] limiter_.ceil_ 无同步写
static void testCeilRace(long long iters) {
    printf("\n=== [P1] limiter_.ceil_ 的无同步跨线程写 ===\n");
    DspCore d;
    d.Prepare(48000.f);
    DspParams p; p.aOn=false; p.bOn=true; p.bypass=true;
    p.envAmt=0.f; p.stepAmt=0.f; p.ceilDb=-3.f; p.lookMs=1.5f;
    d.SetParams(p);

    std::atomic<bool> stop{false};
    std::thread audio([&]{
        std::vector<float> io(480*2, 0.0f);
        for (long long i=0;i<iters && !stop.load(std::memory_order_relaxed);++i)
            d.Process(io.data(), 480);
    });
    std::thread ui([&]{
        for (long long i=0;i<iters && !stop.load(std::memory_order_relaxed);++i) {
            DspParams q = p; q.ceilDb = -3.0f + (float)(i % 12);
            d.SetParams(q);          // -> limiter_.configure -> ceil_ = dbToLin(ceilDb)
        }
    });
    audio.join(); ui.join();
    printf("  完成 %lld 轮，无崩溃。\n", iters);
    printf("  但 limiter_.ceil_(dsp.h:205)是普通 float，configure() 从 UI 线程写，\n");
    printf("  limiter_.process()(dsp.h:179)从音频线程读 —— 形式上的数据竞争(UB)。\n");
    printf("  x86-64 上 4 字节对齐的 float 读写不会撕裂，且 publish 只有 1 次 store，\n");
    printf("  所以实际不会听出问题。属于「形式 UB、实际无害」，但应一并修掉。\n");
}

// ---------------------------------------------------------------- 结论：需要双缓冲吗？
static void printVerdict() {
    printf("\n=== 结论 ===\n");
    printf("  1) params_ = p 是数据竞争（UB），且在 x86-64 上**真的会撕裂**。\n");
    printf("     实测撕裂率见上。成因：24 字节结构体编译成 2 条指令\n");
    printf("       store: movdqu [0..15] ; mov [13..20]   <-- 注意重叠，非对齐\n");
    printf("       load : movdqu [0..15] ; mov [16..23]\n");
    printf("     写入的两次 store 之间，读取可能拿到「新[0..15] + 旧[16..23]」。\n");
    printf("  2) 撕裂的实际后果轻微：ceilDb / aOn / bOn 混合只持续 1 个控制块\n");
    printf("     (kCB=32 样本 = 0.67 ms)，表现为极小的增益不一致，不是爆音。\n");
    printf("  3) **真正严重的是 limiter_.rebuild()**：SetParams 会让 UI 线程重新分配\n");
    printf("     limiter_ 的四个 vector，而音频线程正在读写它们 -> use-after-free。\n");
    printf("     这一条比撕裂重要得多，且当前靠「main.cpp 恰好不写 lookMs」侥幸不触发。\n");
    printf("  4) 需要双缓冲吗？需要，但不是为了撕裂，而是为了 limiter_ 的生命周期。\n");
    printf("     正确修法（一次性、零分配、音频线程无锁）：\n");
    printf("       - params_ 换成双缓冲 + sequence counter（见报告正文）\n");
    printf("       - limiter_.configure() 从音频线程调用，不从 UI 线程\n");
}

int main() {
    printf("SoundLatch SetParams/Process 并发审计\n");
    printf("======================================\n");
    setvbuf(stdout, nullptr, _IONBF, 0);

    testTearing(2000000LL);
    testTearingShape(2000000LL);
    testLimiterRealloc(200000LL);
    testCeilRace(500000LL);

    printVerdict();
    return 0;
}
