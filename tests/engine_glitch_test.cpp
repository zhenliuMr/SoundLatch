// engine_glitch_test.cpp — 审计非 DSP 路径的爆音（咔哒/炸响）
//
// 爆音不只来自限幅器没压住。以下几条都能产生可听爆音：
//  A. 重采样欠载时重复末样本 → 波形阶跃不连续 → 咔哒
//  B. 分帧边界处状态不连续（滤波器/包络被重置）
//  C. NaN/Inf 泄漏到输出
//  D. 限幅器块边界增益跳变 →  zipper noise
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>
#include <algorithm>
#include "dsp.h"

using namespace sl;

// 检查一串样本内部的最大跳变（相邻样本差）。正弦在 1kHz/48k 下
// 每样本约 0.13 (0dBFS)，若出现远大于此的跳变就是阶跃不连续。
static double maxJump(const std::vector<float>& v) {
    double m = 0; size_t at = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        double d = fabs((double)v[i] - (double)v[i - 1]);
        if (d > m) { m = d; at = i; }
    }
    printf("      最大相邻跳变 %.5f @样本 %zu  (%.1f dBFS 满幅=1.0)\n", m, at, 20 * log10(m + 1e-12));
    return m;
}

// ---- A. 重采样器欠载行为复现（照抄 engine.cpp 的逻辑）
static std::vector<float> resampleStarve(const std::vector<float>& in, double ratio, int outFrames) {
    std::vector<float> out;
    out.reserve(outFrames);
    double cur = 0.0;
    const int inLen = (int)in.size();
    for (int k = 0; k < outFrames; ++k) {
        int i0 = (int)cur;
        double fr = cur - i0;
        float v;
        if (i0 + 1 >= inLen) {              // ← engine.cpp 的 starve 分支
            v = in.empty() ? 0.f : in[(size_t)(inLen - 1)];
        } else {
            float a = in[(size_t)i0], b = in[(size_t)(i0 + 1)];
            v = (float)(a + (b - a) * fr);
        }
        out.push_back(v);
        cur += ratio;
    }
    return out;
}

int main() {
    const float fs = 48000.f;
    printf("=== A. 重采样欠载：重复末样本 vs 补零 ===\n");
    {
        // 1kHz 正弦，喂到一半突然断流（模拟设备卡顿/游戏切场景）
        const int N = 2400;
        std::vector<float> tone(N);
        for (int i = 0; i < N; ++i) tone[(size_t)i] = 0.5f * sinf(2 * 3.14159265f * 1000.f * i / fs);
        std::vector<float> fed(tone.begin(), tone.begin() + N / 2);   // 只喂一半

        std::vector<float> a = resampleStarve(fed, 1.0, N);
        printf("    当前实现（重复末样本 %.4f）:\n", fed.back());
        double ja = maxJump(a);
        // 补零版（常见做法：欠载时输出静音而非保持）
        std::vector<float> b = resampleStarve(std::vector<float>{}, 1.0, 0);  // 空输入
        b.clear();
        {
            double cur = 0.0;
            const int inLen = (int)fed.size();
            for (int k = 0; k < N; ++k) {
                int i0 = (int)cur; double fr = cur - i0;
                float v = 0.f;
                if (i0 + 1 < inLen) v = (float)(fed[(size_t)i0] + (fed[(size_t)(i0 + 1)] - fed[(size_t)i0]) * fr);
                b.push_back(v);
                cur += 1.0;
            }
        }
        printf("    补零版:\n");
        double jb = maxJump(b);
        printf("    → 重复末样本的跳变是补零的 %.1f 倍\n", jb > 1e-9 ? ja / jb : 0.0);
        printf("    1kHz@48k 正弦的正常单样本跳变约 0.065（0.5 幅度）\n");
    }

    printf("\n=== B. 分帧调用：状态是否连续 ===\n");
    {
        const int N = 48000;
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i)
            s[(size_t)i] = 0.4f * sinf(2 * 3.14159265f * 440.f * i / fs)
                         + 0.2f * sinf(2 * 3.14159265f * 80.f * i / fs);
        // 1) 一次喂完
        DspCore d1; d1.Prepare(fs);
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f; p.ceilDb = -3.f;
        d1.SetParams(p);
        std::vector<float> whole((size_t)N * 2);
        for (int i = 0; i < N; ++i) whole[2 * i] = whole[2 * i + 1] = s[(size_t)i];
        d1.Process(whole.data(), N);

        // 2) 按 480 样本分帧喂（真实回调形态）
        DspCore d2; d2.Prepare(fs);
        d2.SetParams(p);
        std::vector<float> chunked((size_t)N * 2);
        for (int i = 0; i < N; ++i) chunked[2 * i] = chunked[2 * i + 1] = s[(size_t)i];
        for (int off = 0; off < N; off += 480) {
            int m = std::min(480, N - off);
            d2.Process(chunked.data() + 2 * off, m);
        }

        double diff = 0, worst = 0; int at = 0;
        std::vector<float> mono1(N), mono2(N);
        for (int i = 0; i < N; ++i) { mono1[(size_t)i] = whole[2 * i]; mono2[(size_t)i] = chunked[2 * i]; }
        // 只比稳定段（前0.3s 是滤波器收敛 + 前视填充）
        for (int i = (int)(fs * 0.3f); i < N; ++i) {
            double dd = fabs((double)mono1[(size_t)i] - (double)mono2[(size_t)i]);
            if (dd > worst) { worst = dd; at = i; }
            diff += dd * dd;
        }
        printf("    一次喂 vs 480分帧：RMS 差 %.6f，最大差 %.6f @样本 %d (%.2fs)\n",
               sqrt(diff / (N - fs * 0.3f)), worst, at, at / fs);
        printf("    %s（差值来自滤波器状态，量级应 <0.01）\n", worst < 0.01f ? "ok" : "<<< 状态不连续");
        printf("    分帧输出内部最大跳变: "); maxJump(mono2);
    }

    printf("\n=== C. NaN / Inf 注入 ===\n");
    {
        DspCore d; d.Prepare(fs);
        DspParams p; p.aOn = true; p.bOn = true; p.ceilDb = -3.f;
        d.SetParams(p);
        const int N = 4800;
        std::vector<float> buf((size_t)N * 2, 0.f);
        for (int i = 0; i < N; ++i) {
            buf[2 * i] = buf[2 * i + 1] = 0.3f * sinf(2 * 3.14159265f * 500.f * i / fs);
        }
        buf[2 * 500] = NAN;                    // 中间插一个 NaN
        buf[2 * 501] = INFINITY;
        buf[2 * 502] = -INFINITY;
        d.Process(buf.data(), N);
        int nan = 0, inf = 0;
        float pk = 0;
        for (int i = 0; i < N * 2; ++i) {
            if (std::isnan(buf[(size_t)i])) ++nan;
            if (std::isinf(buf[(size_t)i])) ++inf;
            if (!std::isnan(buf[(size_t)i]) && !std::isinf(buf[(size_t)i]))
                pk = std::max(pk, fabsf(buf[(size_t)i]));
        }
        printf("    注入 1NaN+2Inf 后：输出 NaN=%d Inf=%d峰值=%.2f dBFS\n",
               nan, inf, linToDb(pk));
        printf("    %s\n", (nan == 0 && inf == 0) ? "ok（已过滤）" : "<<< NaN/Inf 泄漏到输出");
    }

    printf("\n=== D. 限幅器块边界增益跳变（zipper）===\n");
    {
        // 持续超限信号下，增益每块都在动；看块边界处输出的一阶差是否异常大
        const int N = 48000;
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i) {
            // 幅度缓慢起伏的正弦，逼限幅器持续调整
            float amp = 1.2f * (0.6f + 0.4f * sinf(2 * 3.14159265f * 3.f * i / fs));
            s[(size_t)i] = amp * sinf(2 * 3.14159265f * 200.f * i / fs);
        }
        DspCore d; d.Prepare(fs);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = -6.f;
        d.SetParams(p);
        std::vector<float> buf((size_t)N * 2);
        for (int i = 0; i < N; ++i) buf[2 * i] = buf[2 * i + 1] = s[(size_t)i];
        for (int off = 0; off < N; off += 480) d.Process(buf.data() + 2 * off, std::min(480, N - off));
        std::vector<float> mono(N);
        for (int i = 0; i < N; ++i) mono[(size_t)i] = buf[2 * i];
        // 只看块边界附近(±2 样本) vs 块内
        auto jumpAt = [&](int i) { return fabs((double)mono[(size_t)i] - (double)mono[(size_t)i - 1]); };
        double bndMax = 0, inMax = 0;
        for (int off = 480; off < N - 2; off += 480) {
            for (int k = -2; k <= 2; ++k) bndMax = std::max(bndMax, jumpAt(off + k));
        }
        for (int i = 1000; i < N - 2; ++i) {
            if ((i % 480) > 2 && (i % 480) < 478) inMax = std::max(inMax, jumpAt(i));
        }
        printf("    块边界最大跳变 %.5f  块内最大跳变 %.5f  比率 %.2f\n",
               bndMax, inMax, inMax > 1e-9 ? bndMax / inMax : 0.0);
        printf("    %s（比率接近 1 说明边界无异常）\n",
               (inMax > 1e-9 && bndMax / inMax < 2.0f) ? "ok" : "<<< 边界有增益跳变");
    }
    return 0;
}