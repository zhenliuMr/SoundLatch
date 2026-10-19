// duck_test.cpp — 验证欠载淡出淡入（duck）的波形连续性
//
// 复刻修复后的engine.cpp 逻辑，对比修复前后的爆炸能量。
// 判据：相邻样本跳变的最大值。1kHz@48k 正弦、0.5 幅度时正常跳变约0.065，
// 远大于这个值的跳变 = 可听爆音。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

static const float kFs = 48000.f;

// 修复前：starve 时重复 inBuf_[inLen_-1]
static std::vector<float> oldWay(const std::vector<float>& in, int starveAt, int n) {
    std::vector<float> out((size_t)n, 0.f);
    for (int i = 0; i < starveAt; ++i) out[(size_t)i] = in[(size_t)i];
    float hold = in[(size_t)(starveAt - 1)];
    for (int i = starveAt; i < n; ++i) out[(size_t)i] = hold;   // 重复末样本（直流）
    return out;
}

// 修复后：欠载时 5ms 淡出到 0，恢复时 5ms 淡入
static std::vector<float> newWay(const std::vector<float>& in, int starveAt, int n,
                                 int starveEnd) {
    std::vector<float> out((size_t)n, 0.f);
    const float kFadeStep = 1.0f / (5.0f * 0.001f * kFs);   // 每样本 1/240
    float duck = 1.0f;
    for (int i = 0; i < n; ++i) {
        bool starved = (i >= starveAt && i < starveEnd);
        float s;
        if (starved) { duck -= kFadeStep; if (duck < 0.f) duck = 0.f; s = 0.f; }
        else {
            if (duck < 1.0f) { duck += kFadeStep; if (duck > 1.0f) duck = 1.0f; }
            s = in[(size_t)i];
        }
        out[(size_t)i] = s * duck;
    }
    return out;
}

static void report(const char* tag, const std::vector<float>& v) {
    double maxJump = 0; int at = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        double d = fabs((double)v[i] - (double)v[i - 1]);
        if (d > maxJump) { maxJump = d; at = (int)i; }
    }
    // 宽带爆炸能量：二阶差分（高通）的绝对值和，量化"咔哒"的能量
    double burst = 0;
    for (size_t i = 1; i + 1 < v.size(); ++i)
        burst += fabs((double)v[i] * 2.0 - (double)v[i - 1] - (double)v[i + 1]);
    printf("  %-22s 最大跳变=%.5f @%d  二阶差分总和=%.1f\n", tag, maxJump, at, burst);
}

int main() {
    const int n = 24000;                       // 0.5s
    const int starveAt = 12000;                // 0.25s 处断流
    const int starveEnd = 16800;               // 0.35s 恢复

    printf("=== 0. 为什么纯正弦测不出来 ===\n");
    printf("  正弦在断流点的瞬时值恰好接近 0（跨过零点），所以「重复末样本」≈ 0，\n");
    printf("  和补零看不出差别。真实音频（爆炸/枪声）在任意时刻都可能是峰值，\n");
    printf("  必须用宽带信号测。下面用带限噪声，并做多相位扫描取最坏情况。\n");

    // 宽带素材：3kHz 带限噪声（脚步/爆炸的频谱形态）
    std::vector<float> wide((size_t)n);
    {
        std::mt19937 rng(11);
        std::normal_distribution<float> nd(0, 1);
        float lp = 0.f, hp = 0.f;
        for (int i = 0; i < n; ++i) {
            float x = nd(rng);
            lp += 0.45f * (x - lp);                 // ~ LP
            float h = lp - hp; hp += 0.08f * h;     // ~ HP @ 1kHz
            wide[(size_t)i] = h * 0.6f;
        }
    }

    printf("\n=== 1. 宽带素材，中途欠载 100ms（多相位取最坏）===\n");
    {
        double worstOld = 0, worstNew = 0; int atOld = 0, atNew = 0;
        for (int ph = 0; ph < 480; ++ph) {          // 扫描一个 10ms 周期内的所有相位
            int sa = starveAt + ph, se = starveEnd + ph;
            if (se + 2400 >= n) break;
            std::vector<float> a = oldWay(wide, sa, n);
            std::vector<float> b = newWay(wide, sa, n, se);
            for (int i = sa - 400; i < se + 2400 && i < n; ++i) {
                double ja = fabs((double)a[(size_t)i] - (double)a[(size_t)i - 1]);
                double jb = fabs((double)b[(size_t)i] - (double)b[(size_t)i - 1]);
                if (ja > worstOld) { worstOld = ja; atOld = i; }
                if (jb > worstNew) { worstNew = jb; atNew = i; }
            }
        }
        // 素材自身的正常跳变（不断流时）
        double base = 0;
        for (int i = 1; i < n; ++i)
            base = std::max(base, fabs((double)wide[(size_t)i] - (double)wide[(size_t)i - 1]));
        printf("  素材正常跳变           = %.5f\n", base);
        printf("  修复前(重复末样本)     = %.5f  @样本 %d   → 超正常 %.1f 倍 %s\n",
               worstOld, atOld, worstOld / base, worstOld / base > 1.5 ? "<<< 爆音" : "ok");
        printf("  修复后(淡出淡入)       = %.5f  @样本 %d   → 超正常 %.1f 倍 %s\n",
               worstNew, atNew, worstNew / base, worstNew / base > 1.5 ? "<<< 爆音" : "ok");
    }

    printf("\n=== 2. 宽带素材：连续抖动欠载（每 20ms 断 5ms）===\n");
    {
        std::vector<double> resOld, resNew;
        for (int ph = 0; ph < 480; ++ph) {
            // 旧：直接重复末样本（每个周期独立）
            std::vector<float> a((size_t)n);
            float hold = 0.f;
            for (int i = 0; i < n; ++i) {
                int p = (i + ph) % 1200;
                if (p < 960) { a[(size_t)i] = wide[(size_t)i]; hold = a[(size_t)i]; }
                else a[(size_t)i] = hold;
            }
            // 新：带 duck 的连续淡出淡入
            std::vector<float> b((size_t)n);
            const float kFade = 1.0f / 240.0f;
            float duck = 1.0f;
            for (int i = 0; i < n; ++i) {
                int p = (i + ph) % 1200;
                bool starved = (p >= 960);
                if (starved) { duck -= kFade; if (duck < 0.f) duck = 0.f; }
                else { if (duck < 1.f) { duck += kFade; if (duck > 1.f) duck = 1.f; } }
                b[(size_t)i] = starved ? 0.f : wide[(size_t)i] * duck;
            }
            double ja = 0, jb = 0;
            for (int i = 1; i < n; ++i) {
                ja = std::max(ja, fabs((double)a[(size_t)i] - (double)a[(size_t)i - 1]));
                jb = std::max(jb, fabs((double)b[(size_t)i] - (double)b[(size_t)i - 1]));
            }
            resOld.push_back(ja); resNew.push_back(jb);
        }
        auto avgMax = [](std::vector<double>& v) {
            double s = 0, m = 0; for (double x : v) { s += x; m = std::max(m, x); }
            return std::make_pair(s / v.size(), m);
        };
        auto a = avgMax(resOld), b = avgMax(resNew);
        printf("  修复前: 平均 %.5f  最坏 %.5f\n", a.first, a.second);
        printf("  修复后: 平均 %.5f  最坏 %.5f\n", b.first, b.second);
        printf("  → 最坏情况改善 %.1f 倍\n", a.second / std::max(1e-12, b.second));
    }

    printf("\n=== 3. 恢复后包络是否回到原位===\n");
    {
        std::vector<float> v = newWay(wide, starveAt, n, starveEnd);
        // 恢复后 20ms 的 RMS 应接近素材常态 RMS
        auto rms = [&](int a, int b) {
            double s = 0; for (int i = a; i < b && i < n; ++i) s += (double)v[(size_t)i] * v[(size_t)i];
            return sqrt(s / (b - a));
        };
        double r0 = rms(1000, 3000), r1 = rms(starveEnd + 1200, starveEnd + 3200);
        printf("  欠载前 RMS=%.4f  恢复后 RMS=%.4f  差 %+.1f%%  %s\n",
               r0, r1, (r1 - r0) / r0 * 100.0, fabs(r1 - r0) / r0 < 0.05f ? "ok" : "<<< 没恢复");
    }
    return 0;
}