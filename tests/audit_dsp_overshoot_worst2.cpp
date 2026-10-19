// audit_dsp_overshoot_worst2.cpp — 修正版最坏超限构造
//
// 上一版用方波，峰值在 4 段分频后被 LR4 衰减了，没打到真正的上界。
// 本版用「带内平坦」的信号（低频正弦 + 宽带噪声，1kHz 附近增益≈1），
// 保证 4 段叠加后 pk_postfilter ≈ 输入幅度，从而暴露真实上界。
//
// 推导见 audit_dsp_overshoot_worst.cpp：
//   超限幅度 = (pk_prev - ceilLin) * relLimCoef_ ,  relLimCoef_ = 0.008299
//   => 输入动态范围越大、ceiling 越低，超限越多。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

static float REL = 1.0f - std::exp(-32.0f/(48000.0f*0.080f));

// 构造：稳态块(峰值 pk) x NB，然后 1 个静音块，再 4 个观察块
static void run(float pk, float ceilDb, int NB) {
    const int BLK = 32;
    int N = (NB + 6) * BLK;
    std::vector<float> io((size_t)N*2, 0.f);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> ud(-1.f,1.f);
    // 带内平坦：60Hz 正弦(占 band0) + 1kHz 正弦(band1/2) + 5kHz(band3)
    for (int i = 0; i < N; ++i) {
        if (i/BLK >= NB) break;                   // NB 之后全静音
        float t = (float)i;
        float s = (pk/3.f)*(float)std::sin(2*3.14159265f*60.f*t/48000.f)
                + (pk/3.f)*(float)std::sin(2*3.14159265f*1000.f*t/48000.f)
                + (pk/3.f)*(float)std::sin(2*3.14159265f*5000.f*t/48000.f);
        io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
    }
    std::vector<float> orig = io;
    DspCore d; d.Prepare(48000.f);
    DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = ceilDb;
    d.SetParams(p);
    d.Process(io.data(), N);
    float cl = dbToLin(ceilDb);
    float mx = 0; int firstOver = -1;
    for (int i = 0; i < N; ++i) {
        float v = std::fabs(io[(size_t)i*2]);
        if (v > mx) mx = v;
        if (v > cl*1.0005f && firstOver < 0) firstOver = i;
    }
    double theory = cl + (pk - cl) * REL;
    printf("  pk=%-8.0f ceilDb=%+6.1f : 实测max=%-10.5f ceil=%-9.5f 超限=%+7.3f dB  越过±1.0:%-3s %s\n",
           pk, ceilDb, mx, cl, 20*std::log10(mx/cl),
           mx > 1.0 ? "是" : "否",
           mx > cl*1.0005 ? (mx>1.0 ? "<<< 严重：既超 ceiling 又越过满刻度" : "<<< 超 ceiling")
                         : "OK");
    if (firstOver >= 0)
        printf("       首次超限 @样本 %d (块 %d)  该处输入=%.4f\n",
               firstOver, firstOver/BLK, std::fabs(orig[(size_t)firstOver*2]));
}

int main() {
    printf("relLimCoef_=%.6f  理论上界超限 = (pk - ceilLin)*relLimCoef_\n\n", REL);
    printf("=== A) 不同输入电平 x 不同 ceiling（带内平坦信号，NB=6 稳态块）===\n");
    for (float ceilDb : {-18.f, -6.f, -3.f, 0.f}) {
        for (float pk : {0.5f, 1.0f, 2.0f, 4.0f, 10.0f}) {
            char buf[64]; snprintf(buf,sizeof buf,"ceilDb=%+6.1f pk=%-6.1f", ceilDb, pk);
            printf("  --- %s ---\n", buf);
            run(pk, ceilDb, 6);
        }
    }
    printf("\n=== B) 逐块追踪 pk=4, ceil=-18dB（NB=6）===\n");
    {
        float pk = 4.f, ceilDb = -18.f;
        int NB = 6, BLK = 32, N = (NB+6)*BLK;
        std::vector<float> io((size_t)N*2, 0.f);
        for (int i = 0; i < N; ++i) {
            if (i/BLK >= NB) break;
            float t = (float)i;
            float s = (pk/3.f)*(float)std::sin(2*3.14159265f*60.f*t/48000.f)
                    + (pk/3.f)*(float)std::sin(2*3.14159265f*1000.f*t/48000.f)
                    + (pk/3.f)*(float)std::sin(2*3.14159265f*5000.f*t/48000.f);
            io[(size_t)i*2]=s; io[(size_t)i*2+1]=s;
        }
        std::vector<float> orig = io;
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn=false; p.bOn=true; p.ceilDb=ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float cl = dbToLin(ceilDb);
        printf("  ceilLin=%.5f  (pk=4, 限幅深度 %.1f dB)\n", cl, 20*std::log10(cl/pk));
        printf("  %-5s %-11s %-11s %-12s %s\n","块","输入峰值","输出峰值","超限dB","");
        for (int b = 0; b < NB+3; ++b) {
            float pin=0,pout=0;
            for (int i=0;i<BLK;++i){int k=b*BLK+i;
                pin=std::max(pin,std::fabs(orig[(size_t)k*2]));
                pout=std::max(pout,std::fabs(io[(size_t)k*2]));}
            printf("  %-5d %-11.4f %-11.5f %-12.3f %s\n", b, pin, pout,
                   20*std::log10(std::max(pout,1e-12f)/cl),
                   pout>cl*1.0005f ? "<<< 超限":"");
        }
        printf("\n  机理：块 b 的样本在块 b+1 才被输出，乘的是块 b+1 算的 limCur_。\n");
        printf("        最后一个稳态块（块 %d）之后是静音块，target 跳到 1.0，\n", NB-1);
        printf("        gain 在 lerp 里被放松 relLimCoef_=%.4f -> 尾部样本超限。\n", REL);
        printf("        实测：块 %d（第一个静音块）输出的是块 %d 的数据，峰值 %.5f > ceil %.5f\n",
               NB, NB-1, [&]{float m=0;for(int i=0;i<BLK;++i)m=std::max(m,std::fabs(io[(size_t)((NB+1)*BLK+i)*2]));return m;}(), cl);
    }
    return 0;
}
