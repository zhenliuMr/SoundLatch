// Offline DSP verification (console)
#include "../src/dsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <chrono>
#include <random>

using namespace sl;

static std::vector<float> mono(const std::vector<float>& inter) {
    std::vector<float> v(inter.size() / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = inter[2 * i];
    return v;
}

static double goertzelAmp(const std::vector<float>& x, double freq, double fs, size_t from, size_t n) {
    double w = 2.0 * 3.14159265358979 * freq / fs;
    double c = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + n && i < x.size(); ++i) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

int main() {
    const double fs = 48000.0;
    const int N = 48000 * 2;

    // ---- 1. limiter ceiling
    {
        DspCore d; d.Prepare((float)fs);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = -6.f;
        p.bypass = false;
        d.SetParams(p);
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; ++i) {
            float s = sinf(2 * 3.14159265f * 997.f * i / fs);
            buf[2 * i] = s; buf[2 * i + 1] = s;
        }
        d.Process(buf.data(), N);
        float mx = 0; for (float v : buf) mx = std::max(mx, fabsf(v));
        float ceil = dbToLin(-6.f);
        printf("[T1] limiter ceiling -6dB -> max %.4f (ceil %.4f) %s\n", mx, ceil,
               mx <= ceil + 1e-3f ? "PASS" : "FAIL");
    }

    // ---- 2. flat response with both switches off
    {
        DspCore d; d.Prepare((float)fs);
        DspParams p; p.aOn = false; p.bOn = false;
        d.SetParams(p);
        std::vector<float> in(N * 2), out(N * 2);
        for (int i = 0; i < N; ++i) {
            float s = 0.5f * sinf(2 * 3.14159265f * 1000.f * i / fs);
            in[2 * i] = in[2 * i + 1] = s;
        }
        out = in;
        d.Process(out.data(), N);
        auto mi = mono(in), mo = mono(out);
        double a0 = goertzelAmp(mi, 1000, fs, N / 2, 8192);
        double a1 = goertzelAmp(mo, 1000, fs, N / 2, 8192);
        double err = 20 * log10(a1 / a0);
        printf("[T2] crossover flatness @1k: %.2f dB %s\n", err, fabs(err) < 0.5 ? "PASS" : "FAIL");
    }

    // ---- 3. 环境抑制基本通路：低频独大时压低频，且中高频不受牵连
// 注意：判据已从「绝对 dBFS 阈值」改为「谱倾斜」，所以测试信号必须低频明显大于
// 中高频才该被压。等幅信号现在正确地不被压（见 T3b）。
    {
        DspCore d; d.Prepare((float)fs);
        DspParams p; p.aOn = true; p.bOn = false;
        p.envAmt = 1.f; p.stepAmt = 0.f;
        d.SetParams(p);
        std::vector<float> in(N * 2), out(N * 2);
        for (int i = 0; i < N; ++i) {
            float s = 0.5f * sinf(2 * 3.14159265f * 60.f * i / fs)
                    + 0.05f * sinf(2 * 3.14159265f * 1000.f * i / fs);  // 低频独大 20dB
            in[2 * i] = in[2 * i + 1] = s;
        }
        out = in;
        d.Process(out.data(), N);
        auto mi2 = mono(in), mo2 = mono(out);
        double b0 = goertzelAmp(mi2, 60, fs, N / 2, 16384);
        double b1 = goertzelAmp(mo2, 60, fs, N / 2, 16384);
        double m0 = goertzelAmp(mi2, 1000, fs, N / 2, 16384);
        double m1 = goertzelAmp(mo2, 1000, fs, N / 2, 16384);
        double g60 = 20 * log10(b1 / b0), g1k = 20 * log10(m1 / m0);
        printf("[T3] 低频独大时 60Hz %+.1f dB (want <= -12) %s | 1kHz %+.1f dB (want |d|<3) %s\n",
               g60, g60 <= -12 ? "PASS" : "FAIL",
               g1k, fabs(g1k) < 3 ? "PASS" : "FAIL");
    }

    // ---- 3b. 谱倾斜判据：低频独大才压，两者在同一绝对电平下对比
    // 环境抑制现在不看绝对电平，只看「低频相对中高频是否独大」（tilt > 6dB 才介入）。
    // 下面两个信号峰值相同，唯一区别是频谱倾斜，必须得到完全不同的增益。
    {
        printf("[T3b] 谱倾斜判据（tiltOn=%.0f dB, tiltFull=%.0f dB）\n",
               sl::judge::kTiltOn, sl::judge::kTiltFull);
        struct Case { const char* tag; float lowAmp; float midAmp; float expectSupp; };
        // 低频独大 20dB → 应被明显压制；等幅 → 不该被压
        const Case cases[] = {
            { "低频独大 20dB", 0.5f, 0.05f, true  },
            { "低频独大 12dB", 0.5f, 0.125f, true },
            { "低频独大 16dB", 0.5f, 0.063f, true },
            { "等幅 0dB",      0.5f, 0.5f,  false },
            { "中频更大",      0.2f, 0.5f,  false },
        };
        bool allPass = true;
        for (const Case& c : cases) {
            DspCore d; d.Prepare((float)fs);
            DspParams p; p.aOn = true; p.bOn = false;
            p.envAmt = 1.f; p.stepAmt = 0.f;
            d.SetParams(p);
            std::vector<float> in(N * 2), out(N * 2);
            for (int i = 0; i < N; ++i) {
                float s = c.lowAmp * sinf(2 * 3.14159265f * 60.f * i / fs)
                        + c.midAmp * sinf(2 * 3.14159265f * 1000.f * i / fs);
                in[2 * i] = in[2 * i + 1] = s;
            }
            out = in;
            d.Process(out.data(), N);
            auto mi = mono(in), mo = mono(out);
            double g0 = goertzelAmp(mi, 60, fs, N / 2, 16384);
            double g1 = goertzelAmp(mo, 60, fs, N / 2, 16384);
            double g = 20 * log10(g1 / g0);
            bool suppressed = (g < -3.0f);
            bool ok = (suppressed == c.expectSupp);
            allPass = allPass && ok;
            printf("      %-14s 60Hz %+6.1f dB  期望%s  %s\n",
                   c.tag, g, c.expectSupp ? "压制" : "不压", ok ? "PASS" : "FAIL");
        }
        printf("[T3b] 谱倾斜判据整体 %s\n", allPass ? "PASS" : "FAIL");
    }

    // ---- 4. transient (footstep-like) boost
    {
        DspCore d; d.Prepare((float)fs);
        DspParams p; p.aOn = true; p.bOn = false;
        p.envAmt = 0.f; p.stepAmt = 1.f;
        d.SetParams(p);
        std::vector<float> in(N * 2), out(N * 2);
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0, 1);
        // bursts of band-limited noise, 120 ms every 500 ms, -30 dBFS
        std::vector<float> noise(N);
        float lp = 0, hp = 0;
        for (int i = 0; i < N; ++i) {
            float n = nd(rng) * 0.03f;
            lp += 0.35f * (n - lp);            // crude LP @~3kHz
            float hpv = lp - hp; hp += 0.02f * hpv;  // crude HP @~150Hz
            noise[i] = hpv * 3.0f;
        }
        for (int i = 0; i < N; ++i) {
            int ph = i % 24000;
            float env = (ph > 300 && ph < 300 + 5760) ? 1.f : 0.f;  // 120 ms burst
            in[2 * i] = in[2 * i + 1] = noise[i] * env;
        }
        out = in;
        d.Process(out.data(), N);
        // compare peak of a burst region (last burst)
        size_t from = N - 24000 + 300;
        float p0 = 0, p1 = 0;
        for (size_t i = from; i < from + 3000; ++i) {
            p0 = std::max(p0, fabsf(in[2 * i]));
            p1 = std::max(p1, fabsf(out[2 * i]));
        }
        printf("[T4] transient boost: %+.1f dB (want >= +6) %s\n",
               20 * log10(p1 / p0), 20 * log10(p1 / p0) >= 6 ? "PASS" : "FAIL");
        // steady-state part should NOT be boosted much
        float s1 = 0;
        size_t fs0 = N - 24000 + 12000;
        for (size_t i = fs0; i < fs0 + 3000; ++i) s1 = std::max(s1, fabsf(out[2 * i]));
        printf("      steady part peak %.5f (should be ~0)\n", s1);
    }

    // ---- 6. 整体限幅：爆炸被压到上限，整个信号（含 1k 脚步带）同比例下降，音色不变
    {
        DspCore d; d.Prepare((float)fs);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = -6.f;
        d.SetParams(p);
        std::vector<float> buf(N * 2);
        const int N3 = (int)(fs * 3.0);
        buf.assign((size_t)N3 * 2, 0.f);
        for (int i = 0; i < N3; ++i) {
            double t = i / fs;
            float foot = 0.03f * sinf(2 * 3.14159265f * 1000.f * (float)t);       // -30 dBFS
            float boom = (t > 1.0 && t < 2.0) ? 0.9f * sinf(2 * 3.14159265f * 60.f * (float)t) : 0.f;
            buf[2 * i] = buf[2 * i + 1] = foot + boom;
        }
        d.Process(buf.data(), N3);
        float mx = 0; for (float v : buf) mx = std::max(mx, fabsf(v));
        auto monoOut = mono(buf);
        double a1 = goertzelAmp(monoOut, 1000.0, fs, (size_t)(0.4 * fs), (size_t)(0.4 * fs));  // 爆炸前
        double a2 = goertzelAmp(monoOut, 1000.0, fs, (size_t)(1.3 * fs), (size_t)(0.4 * fs));  // 爆炸中
        double a3 = goertzelAmp(monoOut, 1000.0, fs, (size_t)(2.2 * fs), (size_t)(0.4 * fs));  // 爆炸后
        double d2 = 20 * log10(a2 / a1), d3 = 20 * log10(a3 / a1);
        // 整体限幅：爆炸期间所有频段同比例下降，1k 也会跟着降；
        // 只要求下降有界（<= 8 dB）且爆炸后恢复原状——音色不变比绝对响度重要。
        printf("[T6] 1kHz(脚步带) 爆炸中 %+.1f dB / 爆炸后 %+.1f dB (want |d| <= 8) %s\n",
               d2, d3, (fabs(d2) <= 8.0 && fabs(d3) < 1.5) ? "PASS" : "FAIL");
        printf("      total peak %.4f (ceil %.4f) %s\n", mx, dbToLin(-6.f),
               mx <= dbToLin(-6.f) + 1e-3f ? "PASS" : "FAIL");
    }

    // ---- 5. realtime cost
    {        DspCore d; d.Prepare((float)fs);
        DspParams p; d.SetParams(p);
        int blocks = 2000, frames = 480;
        std::vector<float> buf(frames * 2);
        std::mt19937 rng(1);
        std::normal_distribution<float> nd(0, 0.1f);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int b = 0; b < blocks; ++b) {
            for (auto& v : buf) v = nd(rng);
            d.Process(buf.data(), frames);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double audioMs = blocks * frames / fs * 1000.0;
        printf("[T5] processed %.1f s audio in %.1f ms -> %.2f%% of one core\n",
               audioMs / 1000.0, ms, ms / audioMs * 100.0);
    }
    return 0;
}
