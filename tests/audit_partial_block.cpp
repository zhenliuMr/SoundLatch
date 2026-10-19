// audit_partial_block.cpp — 审计 frames 不是 kCB(32) 整数倍时的前视缓存 pendL_
//
// 疑点（dsp.h:425-428）：
//     memcpy(pendL_[b], bl[b], sizeof(float) * (size_t)n);
// 只写了 n 个样本。若某个 Process() 调用的最后一个控制块 n < kCB，
// 则 pendL_[b][n .. kCB-1] 保留的是**更早**那个块的残留数据。
// 下一次 Process() 的首个块（n'=kCB）会输出 pendL_[b][0..kCB-1]，
// 其中 [0..n) 是上一调用的末块（较新），[n..kCB) 是倒数第二个块（较旧）
// → 输出时间轴里「新样本在前、老样本在后」，顺序被打乱，听感上是周期性咔哒。
//
// 生产环境：engine.cpp:368  frames = bufFrames - pad（pad 连续变化），
// 几乎不可能恒为 32 的整数倍。现有测试全用 32/480/512/1024/2048（都是 32 倍数），
// 覆盖不到这条路径。
//
// 验证方法：输入一个严格单调递增的斜坡（样本值 = 序号/1000）。
// 正确实现下输出必然也是单调递增的（只是延迟 1 个控制块）。
// 任何「向下的跳变」都直接证明时间顺序错乱。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;

static void run(int framesPerCall, const char* tag) {
    const int total = 48000 * 3;
    std::vector<float> ramp((size_t)total);
    for (int i = 0; i < total; ++i) ramp[(size_t)i] = (float)i * 1e-4f;  // 0 .. 4.8, 单调

    DspCore d; d.Prepare(48000.f);
    DspParams p; p.aOn = false; p.bOn = false; p.ceilDb = 0.f;   // 只留 4 段分频+叠加
    d.SetParams(p);

    std::vector<float> io((size_t)total * 2);
    for (int i = 0; i < total; ++i) io[2 * i] = io[2 * i + 1] = ramp[(size_t)i];
    for (int off = 0; off < total; off += framesPerCall) {
        int m = std::min(framesPerCall, total - off);
        d.Process(io.data() + 2 * off, m);
    }

    // 只统计稳定段（跳过起始瞬态）
    const int from = 48000, to = total - 48000;
    int backJump = 0;            // 输出相对输入「倒退」的样本数
    float worstBack = 0.f;        // 最严重的一次倒退量（单位：输入样本）
    int firstBad = -1;
    for (int i = from; i < to; ++i) {
        // 输出 io[i] 应该约等于 ramp[i - D]，D≈控制块延迟。用局部比例估计 D：
        // 直接比较相邻差分是否保持正向即可。
        float dOut = io[2 * (i + 1)] - io[2 * i];
        float dIn  = ramp[(size_t)(i + 1)] - ramp[(size_t)i];
        // 差分为负 = 波形倒退 = 时间顺序错乱
        if (dOut < -1e-9f && dIn > 0.f) {
            ++backJump;
            if (-dOut > worstBack) worstBack = -dOut;
            if (firstBad < 0) firstBad = i;
        }
    }
    printf("  frames=%-5d %-22s 倒退样本数=%-7d 最大倒退=%-10.6f 首次@%-7d %s\n",
           framesPerCall, tag, backJump, worstBack, firstBad,
           backJump ? "<<< 时间顺序错乱" : "ok");
}

int main() {
    printf("=== pendL_ 前视缓存：frames 非 32 倍数时的完整性 ===\n");
    printf("输入 = 严格单调斜坡；输出差分出现负值 = 时间轴倒流 = 咔哒\n\n");
    for (int f : {32, 64, 128, 480, 512, 1024}) run(f, "(32 的整数倍)");
    printf("\n");
    for (int f : {16, 40, 96, 100, 441, 500}) run(f, "(非 32 倍数)");
    printf("\n注：frames=16 是最恶劣情形——每次调用只推进半个控制块。\n");
    return 0;
}
