// audit_dsp_coeffs.cpp — 审计 Biquad/LR4 系数 + allpass 相位补偿是否让 4 段之和真正平坦
//
// 疑点 1 (dsp.h:69-73)：LR4 = 两个**相同** Q=0.7071 的 2 阶级联。
//   Linkwitz-Riley 4 阶的正确构造是 2 个**完全相同**的 Butterworth(2 阶, Q=0.7071)，
//   这样 |LP|^2+|HP|^2 == 1（功率互补）。这里看起来对，要实测确认。
//
// 疑点 2 (dsp.h:327-330)：allpass 相位补偿的**结构**。
//   band0 (最低频) 挂了 AP600 + AP3k 两个 allpass；
//   band1 挂了 AP3k 一个；
//   band2/band3 一个都没挂。
//   而 ap3kb_ (dsp.h:224) 和 ap3k_ 用的是**完全相同**的参数 (setAP(fs,3000,0.7071))，
//   名字带 b 像是「band1 专用」，但参数一模一样 —— 复制粘贴嫌疑。
//   4 段树的正确补偿应该是：band k 挂它**上游每一个分频点**的全通，
//   且 LR4 的 LP+HP 之和是 4 阶全通（= 两个 2 阶全通级联），不是 1 个 2 阶全通。
//
// 疑点 3 (dsp.h:55)：setAP 里 b2 = 1.0f 而非 (1+alpha)/a0。
//   需要确认这是「因为 a0=1+alpha 所以 (1+alpha)/a0 恰好 = 1」的合法写法，还是笔误。
//
// 输出：
//   A) 系数表 + 极点半径（稳定性）
//   B) setAP 是否真的是全通（|H(f)| 是否 == 1）
//   C) LR4 的 |LP|^2+|HP|^2 是否 == 1（功率互补）
//   D) 真实 DspCore 通路 20Hz-20kHz 扫频，看 4 段之和的幅度起伏   <-- 核心
//   E) 对照：若干候选补偿结构的解析幅度响应
#include <cstdio>
#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;
using cplx = std::complex<double>;

static constexpr double PI = 3.14159265358979323846;
static const double FS = 48000.0;

// ---- 解析传递函数 H(e^jw) = (b0 + b1 z^-1 + b2 z^-2)/(1 + a1 z^-1 + a2 z^-2)
static cplx H(const Biquad& b, double f, double fs) {
    double w = 2 * PI * f / fs;
    cplx zinv = cplx(std::cos(-w), std::sin(-w));
    cplx num = (double)b.b0 + (double)b.b1 * zinv + (double)b.b2 * zinv * zinv;
    cplx den = 1.0 + (double)b.a1 * zinv + (double)b.a2 * zinv * zinv;
    return num / den;
}
static cplx H(const LR4& b, double f, double fs) { return H(b.s1, f, fs) * H(b.s2, f, fs); }

static double poleRadius(const Biquad& b) {
    // 解 z^2 + a1 z + a2 = 0 的最大模
    cplx disc = std::sqrt(cplx((double)b.a1 * b.a1 - 4.0 * b.a2, 0.0));
    cplx p1 = (-(double)b.a1 + disc) / 2.0, p2 = (-(double)b.a1 - disc) / 2.0;
    return std::max(std::abs(p1), std::abs(p2));
}

static void dumpCoeffs() {
    printf("=== A) 系数表 (fs=48000, Q=0.70710678) ===\n");
    printf("%-6s %-4s %12s %12s %12s %12s %12s %10s %12s %12s\n",
           "type", "f", "b0", "b1", "b2", "a1", "a2", "|pole|", "sum b", "b2_norm");
    struct C { const char* t; float f; };
    const C cs[] = { {"LP",150.f},{"HP",150.f},{"LP",600.f},{"HP",600.f},
                     {"LP",3000.f},{"HP",3000.f},{"AP",600.f},{"AP",3000.f} };
    for (auto& c : cs) {
        Biquad b;
        if (!strcmp(c.t,"LP")) b.setLP((float)FS, c.f, 0.70710678f);
        else if (!strcmp(c.t,"HP")) b.setHP((float)FS, c.f, 0.70710678f);
        else b.setAP((float)FS, c.f, 0.70710678f);
        // 全通的正确 b2 应该是 (1+alpha)/a0
        double w0 = 2*PI*c.f/FS, alpha = std::sin(w0)/(2*0.70710678), a0 = 1+alpha;
        double b2_norm = (1+alpha)/a0;
        printf("%-6s %-4.0f %12.8f %12.8f %12.8f %12.8f %12.8f %10.6f %12.6f %12.8f%s\n",
               c.t, c.f, b.b0, b.b1, b.b2, b.a1, b.a2, poleRadius(b),
               b.b0+b.b1+b.b2, b2_norm,
               (std::fabs(b.b2 - b2_norm) < 1e-6f) ? "" : "   <== 与全通标准式不符");
    }
    printf("\n");
}

static void checkAllpass() {
    printf("=== B) setAP 是否真的是全通（|H| 应恒为 1） ===\n");
    for (float f : {600.f, 3000.f}) {
        Biquad b; b.setAP((float)FS, f, 0.70710678f);
        double mn = 1e9, mx = -1e9;
        double argmin = 0, argmax = 0;
        for (double x = 1.0; x <= 20000.0; x *= 1.02) {
            double m = std::abs(H(b, x, FS));
            if (m < mn) { mn = m; argmin = x; }
            if (m > mx) { mx = m; argmax = x; }
        }
        printf("  AP@%-5.0f : |H| min=%.6f@%.0fHz  max=%.6f@%.0fHz  波动=%.4f dB  %s\n",
               f, mn, argmin, mx, argmax, 20*std::log10(mx/mn),
               (20*std::log10(mx/mn) < 0.01) ? "全通 OK" : "<<< 不是全通");
    }
    printf("\n");
}

static void checkLR4() {
    printf("=== C) LR4 性质检验 ===\n");
    printf("  [C1] |LP|^2+|HP|^2 —— 注意这对 LR4 **不成立**（-6dB 交叉点，fc 处=0.5），\n");
    printf("       成立的是 LR2。此项只是确认我们用的确实是 LR4 而非 LR2：\n");
    for (float f : {150.f, 600.f, 3000.f}) {
        LR4 lp, hp; lp.setLP((float)FS, f); hp.setHP((float)FS, f);
        double mn = 1e9, mx = -1e9;
        for (double x = 1.0; x <= 20000.0; x *= 1.02) {
            double p = std::norm(H(lp,x,FS)) + std::norm(H(hp,x,FS));
            mn = std::min(mn,p); mx = std::max(mx,p);
        }
        double g6 = 20*std::log10(std::abs(H(lp,(double)f,FS)));
        printf("       @%-5.0f : fc 处 LP=%+.2f dB (LR4 应为 -6dB)  |L|^2+|H|^2 min=%.4f max=%.4f %s\n",
               f, g6, mn, mx, std::fabs(g6+6.0) < 0.05 ? "=> LR4 正确" : "<<< 不是 LR4");
    }
    // [C2] LR4 的真正恒等式：LP+HP 是全通（|LP+HP| == 1）
    printf("  [C2] |LP+HP| 应恒为 1（LR4 的 LP+HP 是全通）:\n");
    for (float f : {150.f, 600.f, 3000.f}) {
        LR4 lp, hp; lp.setLP((float)FS, f); hp.setHP((float)FS, f);
        double mn = 1e9, mx = -1e9;
        for (double x = 1.0; x <= 20000.0; x *= 1.02) {
            double m = std::abs(H(lp,x,FS) + H(hp,x,FS));
            mn = std::min(mn,m); mx = std::max(mx,m);
        }
        printf("       @%-5.0f : min=%.6f max=%.6f  波动 %.4f dB  %s\n",
               f, mn, mx, 20*std::log10(mx/mn),
               20*std::log10(mx/mn) < 0.01 ? "OK" : "<<< 非常通");
    }
    // [C3] setAP 的注释声称「same magnitude/phase as an LR4 LP+HP pair」，验证相位
    printf("  [C3] setAP 的相位 是否 == LR4 的 (LP+HP) 全通相位？\n");
    for (float f : {600.f, 3000.f}) {
        LR4 lp, hp; lp.setLP((float)FS, f); hp.setHP((float)FS, f);
        Biquad ap; ap.setAP((float)FS, f, 0.70710678f);
        double worst = 0, wf = 0;
        for (double x = 20.0; x <= 20000.0; x *= 1.02) {
            cplx lr = H(lp,x,FS) + H(hp,x,FS);
            double d = std::arg(H(ap,x,FS)) - std::arg(lr);
            while (d >  PI) d -= 2*PI;
            while (d < -PI) d += 2*PI;
            if (std::fabs(d) > std::fabs(worst)) { worst = d; wf = x; }
        }
        printf("       AP@%-5.0f 与 LR4(LP+HP) 最大相位差 = %+.4f rad (%+.2f deg) @%.0fHz %s\n",
               f, worst, worst*180/PI, wf,
               std::fabs(worst) < 0.02 ? "相位一致 OK" : "<<< 相位不同（注释不准确）");
    }
    printf("\n");
}

// ---- 真实 DspCore 通路的幅度响应（关掉所有动态与限幅，只留 4 段分频+叠加）
static void sweepRealPath() {
    printf("=== D) 真实 DspCore 通路 20Hz-20kHz 扫频（aOn=off bOn=off）===\n");
    printf("     只留 4 段 LR4 分频 + allpass 补偿 + 叠加，看幅度是否平坦\n\n");
    const double f0 = 20.0, f1 = 20000.0;
    const int NP = 400;
    std::vector<double> freqs, gains;
    const int N = 96000;
    std::vector<float> in((size_t)N * 2), out((size_t)N * 2);
    double worstDev = 0; double worstF = 0;
    double gAt1k = 0, gAt60 = 0, gAt15k = 0;
    for (int i = 0; i < NP; ++i) {
        double f = f0 * std::pow(f1/f0, (double)i/(NP-1));
        freqs.push_back(f);
        for (int k = 0; k < N; ++k) {
            float s = 0.5f * (float)std::sin(2*PI*f*k/FS);
            in[(size_t)k*2] = in[(size_t)k*2+1] = s;
        }
        out = in;
        DspCore d; d.Prepare((float)FS);
        DspParams p; p.aOn = false; p.bOn = false; p.ceilDb = 0.f; p.bypass = false;
        d.SetParams(p);
        // 一次性整块送入（32 的整数倍，避开 partial block 干扰），只取稳态段测量
        d.Process(out.data(), N);
        // Goertzel 于稳态段
        const int from = N/2, len = 16384;
        double w = 2*PI*f/FS, c = 2*std::cos(w), s1=0, s2=0;
        for (int k = from; k < from+len; ++k) {
            double s0 = out[(size_t)k*2] + c*s1 - s2; s2 = s1; s1 = s0;
        }
        double re = s1 - s2*std::cos(w), im = s2*std::sin(w);
        double amp = 2*std::sqrt(re*re+im*im)/len / 0.5;   // 归一化到输入幅度
        double g = 20*std::log10(amp);
        gains.push_back(g);
        if (std::fabs(g) > std::fabs(worstDev)) { worstDev = g; worstF = f; }
        if (i == NP/8) gAt60 = g;
        if (std::fabs(f-1000.0) < f*std::pow(f1/f0, 0.5/(NP-1))) gAt1k = g;
        if (f > 15000.0 && gAt15k == 0) gAt15k = g;
    }
    printf("  最大起伏: %+.3f dB @ %.1f Hz\n", worstDev, worstF);
    printf("  60Hz=%.3f dB  1kHz=%.3f dB  >15kHz=%.3f dB\n", gAt60, gAt1k, gAt15k);
    printf("\n  频率(dB) 曲线：\n");
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (i % 12) continue;
        printf("   %8.1f Hz %+7.2f dB %s\n", freqs[i], gains[i],
               std::fabs(gains[i]) > 1.0 ? "#" : " ");
    }
    printf("\n  判定：最大起伏 %s（平坦度判据 1.0 dB）\n",
           std::fabs(worstDev) < 1.0 ? "PASS" : "<<< FAIL —— 4 段之和幅度不平坦");
    printf("\n");
}

// ---- 候选补偿结构的解析对比
struct Node { LR4 lp, hp; Biquad ap1, ap2; };

static void compareStructures() {
    printf("=== E) 对照：不同 allpass 补偿结构的解析幅度响应 ===\n");
    const float F[3] = {150.f, 600.f, 3000.f};
    LR4 lp[3], hp[3]; Biquad ap[3][2];
    for (int k = 0; k < 3; ++k) {
        lp[k].setLP((float)FS, F[k]); hp[k].setHP((float)FS, F[k]);
        ap[k][0].setAP((float)FS, F[k], 0.70710678f);
        ap[k][1].setAP((float)FS, F[k], 0.70710678f);
    }
    struct Case { const char* tag; int mode; };
    Case cs[] = {{"现状 (band0:AP6+AP3k, band1:AP3k)", 0},
                 {"教科书 (band k 挂上游全部全通x2)", 1},
                 {"完全不补偿", 2}};
    for (auto& c : cs) {
        double mn = 1e9, mx = -1e9, fmn=0, fmx=0;
        for (double x = 20.0; x <= 20000.0; x *= 1.01) {
            double f = x;
            cplx b0, b1, b2, b3;
            if (c.mode == 0) {          // 现状
                b0 = H(lp[0],f,FS) * H(ap[1][0],f,FS) * H(ap[2][0],f,FS);
                b1 = H(hp[0],f,FS) * H(lp[1],f,FS) * H(ap[2][0],f,FS);
                b2 = H(hp[0],f,FS) * H(hp[1],f,FS) * H(lp[2],f,FS);
                b3 = H(hp[0],f,FS) * H(hp[1],f,FS) * H(hp[2],f,FS);
            } else if (c.mode == 1) {   // 教科书
                b0 = H(lp[0],f,FS);
                b1 = H(ap[0][0],f,FS)*H(ap[0][1],f,FS) * H(hp[0],f,FS) * H(lp[1],f,FS);
                b2 = H(ap[0][0],f,FS)*H(ap[0][1],f,FS) * H(ap[1][0],f,FS)*H(ap[1][1],f,FS)
                    * H(hp[0],f,FS) * H(hp[1],f,FS) * H(lp[2],f,FS);
                b3 = H(ap[0][0],f,FS)*H(ap[0][1],f,FS) * H(ap[1][0],f,FS)*H(ap[1][1],f,FS)
                    * H(hp[0],f,FS) * H(hp[1],f,FS) * H(hp[2],f,FS);
            } else {                    // 不补偿
                b0 = H(lp[0],f,FS);
                b1 = H(hp[0],f,FS) * H(lp[1],f,FS);
                b2 = H(hp[0],f,FS) * H(hp[1],f,FS) * H(lp[2],f,FS);
                b3 = H(hp[0],f,FS) * H(hp[1],f,FS) * H(hp[2],f,FS);
            }
            double m = std::abs(b0+b1+b2+b3);
            if (m<mn){mn=m;fmn=x;} if (m>mx){mx=m;fmx=x;}
        }
        printf("  %-36s min=%.4f@%-7.0f max=%.4f@%-7.0f 波动 %+6.2f dB  %s\n",
               c.tag, mn, fmn, mx, fmx, 20*std::log10(mx/mn),
               20*std::log10(mx/mn) < 0.5 ? "平坦" : "<<< 有起伏");
    }
    printf("\n");
}

int main() {
    printf("############################################################\n");
    printf("# 审计 1: Biquad/LR4 系数 + allpass 相位补偿 + 全频段平坦度\n");
    printf("############################################################\n\n");
    dumpCoeffs();
    checkAllpass();
    checkLR4();
    compareStructures();
    sweepRealPath();
    return 0;
}
