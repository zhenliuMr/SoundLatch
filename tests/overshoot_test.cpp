// overshoot_test.cpp — 审计限幅器是否会漏过峰值（爆音/削顶）
//
// 疑点：Process() 输出的是 pendL_（上一控制块的音频），而施加的增益
// limCur_ 是按**当前块**的峰值算的，并在块内从 limPrev_ 线性插值到 limCur_。
// 如果 limCur_ > limPrev_（当前块安静 → 增益开始恢复），块内插值会把增益
// 往上带；此时若上一块的峰值恰好落在块尾，那个样本就用了偏大的增益 → 溢出。
//
// 做法：构造最恶劣的信号（响 → 静 交替），量输出峰值 vs 用户设的上限。
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

struct Result { float peakDb; float ceilDb; float over; };

static Result runCase(const std::string& tag, const std::vector<float>& mono, float fs,
                      float ceilDb, int blockSamples) {
    DspCore d; d.Prepare(fs);
    DspParams p;
    p.aOn = false;          // 只看限幅通路，避免 A 段增益干扰
    p.bOn = true;
    p.ceilDb = ceilDb;
    d.SetParams(p);

    const int n = (int)mono.size();
    std::vector<float> buf((size_t)n * 2);
    for (int i = 0; i < n; ++i) buf[2 * i] = buf[2 * i + 1] = mono[(size_t)i];

    // 模拟真实 WASAPI 回调：按 blockSamples 一块喂
    float peak = 0.f;
    for (int off = 0; off < n; off += blockSamples) {
        int m = std::min(blockSamples, n - off);
        d.Process(buf.data() + 2 * off, m);
    }
    for (int i = 0; i < n * 2; ++i) peak = std::max(peak, fabsf(buf[(size_t)i]));

    Result r;
    r.peakDb = linToDb(peak);
    r.ceilDb = ceilDb;
    r.over = r.peakDb - ceilDb;      // > 0 就是溢出
    printf("  %-34s peak=%7.2f dBFS  ceil=%6.2f  溢出 %+6.2f dB  %s\n",
           tag.c_str(), r.peakDb, ceilDb, r.over, r.over > 0.05f ? "<<< OVER" : "ok");
    return r;
}

int main() {
    const float fs = 48000.f;
    const float ceilDb = -6.0f;
    const int N = (int)(fs * 4);

    // 定位块边界：dsp 内部控制块是 32 样本，WASAPI 回调常见 480(10ms)/512
    const int CB = 32;

    printf("=== 1. 响→静 交替（考验增益恢复期）===\n");
    {
        std::vector<float> s(N);
        // 每 5 ms 一段：10ms 满幅正弦，接 40 ms 静音
        for (int i = 0; i < N; ++i) {
            int t = i % (int)(fs * 0.05f);
            s[(size_t)i] = (t < (int)(fs * 0.01f))
                ? 2.0f * sinf(2 * 3.14159265f * 80.f * i / fs)   // 超过 0dBFS，逼限幅器干活
                : 0.f;
        }
        runCase("80Hz 突发(2.0) + 静音间隔", s, fs, ceilDb, CB);
    }

    printf("\n=== 2. 相位扫描：峰值落在块内不同位置 ===\n");
    {
        // 固定一段响噪声，紧跟静音；把响段起点在块内逐样本平移，
        // 找哪种对齐会让峰值样本落在块尾（插值把增益带上去）
        float worst = -99.f; int worstShift = 0;
        for (int shift = 0; shift < CB; ++shift) {
            std::vector<float> s(N, 0.f);
            for (int i = 0; i < (int)(fs * 0.02f); ++i) {
                int idx = shift + i;                    // 响段起点 = shift
                if (idx < N)
                    s[(size_t)idx] = 1.5f * sinf(2 * 3.14159265f * 120.f * idx / fs);
            }
            DspCore d; d.Prepare(fs);
            DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = ceilDb;
            d.SetParams(p);
            std::vector<float> buf((size_t)N * 2);
            for (int i = 0; i < N; ++i) buf[2 * i] = buf[2 * i + 1] = s[(size_t)i];
            for (int off = 0; off < N; off += CB) d.Process(buf.data() + 2 * off, CB);
            float pk = 0.f;
            for (int i = 0; i < N * 2; ++i) pk = std::max(pk, fabsf(buf[(size_t)i]));
            float over = linToDb(pk) - ceilDb;
            if (over > worst) { worst = over; worstShift = shift; }
        }
        printf("  扫描 0..%d 样本的所有对齐：最大溢出 %+.2f dB 出现在 shift=%d\n",
               CB - 1, worst, worstShift);
        printf("  %s\n", worst > 0.05f ? "<<< MISS" : "ok");
    }

    printf("\n=== 3. 真实回调块长（480/512/1024）===\n");
    {
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i) {
            int t = i % (int)(fs * 0.03f);
            s[(size_t)i] = (t < (int)(fs * 0.005f))
                ? 3.0f * sinf(2 * 3.14159265f * 60.f * i / fs) : 0.f;
        }
        for (int bs : {480, 512, 1024, 2048}) runCase("60Hz 爆发(3.0), 回调块=" + std::to_string(bs), s, fs, ceilDb, bs);
    }

    printf("\n=== 4. 满幅直流 + 交替（最坏线性叠加）===\n");
    {
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i) {
            int seg = (i / CB) & 1;                  // 每控制块交替满幅/零
            s[(size_t)i] = seg ? 4.0f : 0.f;
        }
        runCase("块级方波(4.0)", s, fs, ceilDb, CB);
    }

    printf("\n=== 5. 静音上限（ceil=静音）===\n");
    {
        std::vector<float> s(N);
        for (int i = 0; i < N; ++i)
            s[(size_t)i] = 1.0f * sinf(2 * 3.14159265f * 440.f * i / fs);
        runCase("440Hz 连续, ceil=-120(静音)", s, fs, -120.0f, CB);
    }

    printf("\n=== 6. A/B 段开启时（增益斜坡会改变峰值）===\n");
    {
        DspCore d; d.Prepare(fs);
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 1.0f; p.stepAmt = 1.0f;
        p.ceilDb = ceilDb;
        d.SetParams(p);
        // 脚步状中频突发（会触发 trSm_ 抬升）
        std::vector<float> s(N);
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0, 1);
        float lp = 0.f, hp = 0.f;
        for (int i = 0; i < N; ++i) {
            float nz = nd(rng) * 0.05f;
            lp += 0.35f * (nz - lp);
            float h = lp - hp; hp += 0.02f * h;
            int t = i % (int)(fs * 0.4f);
            float env = (t < 2000) ? 1.f : 0.f;
            s[(size_t)i] = h * 3.0f * env;
        }
        std::vector<float> buf((size_t)N * 2);
        for (int i = 0; i < N; ++i) buf[2 * i] = buf[2 * i + 1] = s[(size_t)i];
        for (int off = 0; off < N; off += CB) d.Process(buf.data() + 2 * off, CB);
        float pk = 0.f;
        for (int i = 0; i < N * 2; ++i) pk = std::max(pk, fabsf(buf[(size_t)i]));
        printf("  %-34s peak=%7.2f dBFS  ceil=%6.2f  溢出 %+6.2f dB  %s\n",
               "脚步突发 + A/B 全开", linToDb(pk), ceilDb, linToDb(pk) - ceilDb,
               linToDb(pk) - ceilDb > 0.05f ? "<<< OVER" : "ok");
    }
    return 0;
}