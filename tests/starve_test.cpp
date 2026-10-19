// starve_test.cpp — 量化重采样欠载时的听感后果
//
// engine.cpp:404 starve 分支：
//     f[k*2] = inBuf_[(inLen_ - 1) * 2];   // 重复"最后一个样本"
// 但 inBuf_ 是 staging 缓冲，inLen_ 之后是**几十毫秒前的旧数据**（capFrames-2 深度）。
// 设备卡顿 / 游戏切场景 / 声卡被抢占 时就会走到这条路径。
//
// 疑问：重复一个陈旧样本 vs 直接输出静音，哪个更不容易"炸"？
//以及：当前实现下欠载期间输出的是什么内容。
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>
#include <algorithm>
#include "dsp.h"

using namespace sl;

int main() {
    const float fs = 48000.f;

    printf("=== 1. starve 时inBuf_ 里到底是什么===\n");
    {
        // 模拟：先喂 100ms 的 1kHz 正弦，然后停供（inLen_ 停在旧数据上）
        const int cap = 4800;                    // staging 深度 100ms @48k
        std::vector<float> buf((size_t)cap * 2, 0.f);
        int inLen = 0;
        // 只填了 480 个样本（10ms），之后是上一轮搬运过来的旧数据
        for (int i = 0; i < 480; ++i) {
            buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(2 * 3.14159265f * 1000.f * i / fs);
        }
        inLen = 480;
        // 模拟下一轮：旧数据整体前移，新数据只到一半
        int n = 240;
        memmove(buf.data(), buf.data() + n * 2, (size_t)(inLen - n) * 2 * sizeof(float));
        inLen -= n;
        // 模拟 starve：cur_ 超出 inLen_
        double cur = (double)inLen + 50.0;       // 超出 50 个样本
        int i0 = (int)cur;
        float v = (i0 + 1 >= inLen) ? buf[(size_t)(inLen - 1) * 2] : buf[(size_t)i0 * 2];
        printf("    inLen_=%d，cur_=%.0f（超出 %d）\n", inLen, cur, (int)cur - inLen + 1);
        printf("    starve 输出值 = %.5f （=inBuf_ 中 %.2f ms 前的样本）\n",
               v, (inLen - (i0 + 1)) / fs * 1000.0);
        printf("    若该值非零 → 欠载期间会听到一段被拉长/循环的陈旧音频\n");
        printf("    在 1kHz 正弦上，「重复末样本」= 输出直流 %.4f\n", v);
        printf("    → 人耳会听到「嘟」的一声卡住，随后新音频进来时相位跳变= 咔哒\n");
    }

    printf("\n=== 2. 重复末样本 vs 补零：波形连续性 ===\n");
    {
        // 更贴近实际：欠载发生在音频中间，不是末尾
        const int N = 2400;
        std::vector<float> tone(N);
        for (int i = 0; i < N; ++i)
            tone[(size_t)i] = 0.5f * sinf(2 * 3.14159265f * 1000.f * i / fs);
        const int starveAt = 1200;
        std::vector<float> hold = tone, zero = tone;
        for (int i = starveAt; i < N; ++i) {
            hold[(size_t)i]  = tone[(size_t)(starveAt - 1)];   // 保持末样本
            zero[(size_t)i]  = 0.f;                            // 补零
        }
        auto report = [&](const char* tag, const std::vector<float>& v) {
            // 恢复点处的跳变（最大）
            double jump = fabs((double)v[(size_t)starveAt] - (double)v[(size_t)(starveAt - 1)]);
            // 高频能量：突变处宽带成分的粗略指标 = 二阶差分
            double d2 = 0;
            for (int i = 1; i < N - 1; ++i)
                d2 += fabs(v[(size_t)i] * 2.0 - v[(size_t)(i - 1)] - v[(size_t)(i + 1)]);
            d2 /= (N - 2);
            printf("    %-18s 断点跳变=%.5f  平均曲率=%.6f\n", tag, jump, d2);
            return jump;
        };
        double jh = report("保持末样本", hold);
        double jz = report("补零", zero);
        printf("    →保持末样本在断点处的跳变是补零的 %.2f 倍\n", jz > 1e-12 ? jh / jz : 0.0);
        printf("    注：补零在断点处是 %.5f 的阶跃（0.5→0），本身也是可听的\"啪\"\n", jz);
        printf("    两��都会炸，只是炸在不同位置：保持→断流瞬间；补零→恢复瞬间\n");
    }

    printf("\n=== 3. 真正平滑的做法：欠载时把包络拉到0===\n");
    {
        // 正确做法不是「保持」也不是「硬补零」，而是把增益平滑降到 0，
        // 恢复时再平滑升回——这样断点和恢复点都没有阶跃。
        const int N = 2400, starveAt = 1200;
        std::vector<float> tone(N);
        for (int i = 0; i < N; ++i)
            tone[(size_t)i] = 0.5f * sinf(2 * 3.14159265f * 1000.f * i / fs);
        // 5ms 淡出 → 保持静音 → 5ms 淡入
        const int fade = 240;
        std::vector<float> fadeOut(N, 0.f);
        for (int i = 0; i < N; ++i) {
            float g = 1.f;
            if (i >= starveAt - fade && i < starveAt) g = 1.f - (float)(i - (starveAt - fade)) / fade;
            else if (i >= starveAt && i < starveAt + fade) g = 1.f - (float)(i - starveAt) / fade * 0.f;  // 淡出后保持
            else if (i >= N - fade) g = (float)(i - (N - fade)) / fade;
            if (i >= starveAt && i < N - fade) g = 0.f;
            fadeOut[(size_t)i] = tone[(size_t)i] * g;
        }
        double jump = 0, d2 = 0;
        for (int i = 1; i < N - 1; ++i)
            d2 += fabs(fadeOut[(size_t)i] * 2.0 - fadeOut[(size_t)(i - 1)] - fadeOut[(size_t)(i + 1)]);
        d2 /= (N - 2);
        for (int i = starveAt - fade; i < N - fade; ++i)
            jump = std::max(jump, fabs((double)fadeOut[(size_t)i] - (double)fadeOut[(size_t)i - 1]));
        printf("    %-18s 最大跳变=%.5f  平均曲率=%.6f\n", "淡出/淡入", jump, d2);
        printf("    对比：保持末样本曲率 ~0.004，补零曲率更高\n");
        printf("    → 淡出淡入把爆炸能量摊到5ms，人耳基本听不出来\n");
    }

    printf("\n=== 4. 欠载在真实运行中多频繁？===\n");
    {
        // 统计日志里under 字段的含义（engine.cpp 每60s 记一次）
        printf("    engine.cpp 每 60 s 记一次 lat/cap/ring/ren/lvl/under\n");
        printf("    under > 0 说明确实发生过欠载——检查日志：\n");
        printf("      %%TEMP%%\\soundlatch.log  里的 under= 字段\n");
        printf("    触发场景：声卡被别的程序独占、游戏切场景读盘卡顿、\n");
        printf("              笔记本省电策略切换、蓝牙耳机重连\n");
    }
    return 0;
}