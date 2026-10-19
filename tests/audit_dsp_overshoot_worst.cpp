// audit_dsp_overshoot_worst.cpp — 主通路限幅超限的**理论上界**与最坏构造
//
// 推导（dsp.h:403-424）：
//   块 k:  limPrev_ = limCur_(上一块留下的)
//          limCur_  = (target_k < limCur_) ? target_k            // 攻击：瞬时
//                                         : lerp(limCur_, target_k, relLimCoef_)  // 释放：慢
//          输出 pendL_(=块 k-1 的样本) * lerp(limPrev_, limCur_, t)
//
//   设块 k-1 峰值 pk_{k-1}，触发攻击 -> limPrev_ = ceil/pk_{k-1}
//   若块 k 安静 -> target_k = 1.0 -> limCur_ = limPrev_ + (1-limPrev_)*relLimCoef_
//   块 k-1 输出尾部峰值 = pk_{k-1} * limCur_
//        = ceil + (pk_{k-1} - ceil) * relLimCoef_
//   => 超限量 = (pk_{k-1} - ceil) * relLimCoef_
//   relLimCoef_ = 1-exp(-32/(fs*0.08)) = 0.008299
//
//   所以：**上一块越响，超限越多**，上界正比于输入动态范围。
//   pk=4, ceil=0.5 -> 0.5 + 3.5*0.0083 = 0.529  (+0.48 dB)
//   pk=100, ceil=0.5 -> 0.5 + 99.5*0.0083 = 1.326 (+8.5 dB)  <-- 灾难
//
// 本测试构造这个最坏情形并验证。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;

static float REL = 1.0f - std::exp(-32.0f/(48000.0f*0.080f));

int main() {
    printf("relLimCoef_ = %.6f\n", REL);
    printf("理论上界：超限 = (pk_prev - ceilLin) * relLimCoef_\n\n");

    struct Case { const char* tag; float ceilDb; float pk; };
    const Case cs[] = {
        {"pk=1.0  ceil=-18dB", -18.f, 1.0f},
        {"pk=4.0  ceil=-18dB", -18.f, 4.0f},
        {"pk=20   ceil=-18dB", -18.f, 20.f},
        {"pk=100  ceil=-18dB", -18.f, 100.f},
        {"pk=1000 ceil=-18dB", -18.f, 1000.f},
        {"pk=100  ceil=-6dB",   -6.f, 100.f},
        {"pk=1.0  ceil=0dB",     0.f, 1.0f},
        {"pk=2.0  ceil=0dB",     0.f, 2.0f},
        {"pk=10   ceil=0dB",     0.f, 10.f},
    };
    printf("%-22s %10s %10s %12s %12s %10s %s\n",
           "case", "ceilLin", "理论超限", "实测max", "实测超限dB", "超出±1?", "判定");
    for (auto& c : cs) {
        const int BLK = 8;              // 8 个块 = 256 样本
        const int N = BLK*32;
        std::vector<float> io((size_t)N*2, 0.f);
        // 块 0..5 满幅方波(峰值 c.pk)，块 6 静音(触发 target=1.0)，块 7 观察输出
        for (int b = 0; b < 6; ++b)
            for (int i = 0; i < 32; ++i) {
                int k = b*32+i;
                float s = (i%2) ? c.pk : -c.pk;
                io[(size_t)k*2] = s; io[(size_t)k*2+1] = s;
            }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn = false; p.bOn = true; p.ceilDb = c.ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float cl = dbToLin(c.ceilDb);
        float mx = 0;
        for (int i = 0; i < N; ++i) mx = std::max(mx, std::fabs(io[(size_t)i*2]));
        double theory = cl + (c.pk - cl) * REL;
        double overDb = 20*std::log10(mx/cl);
        printf("%-22s %10.5f %10.5f %12.5f %12.3f %10s %s\n",
               c.tag, cl, theory, mx, overDb, mx > 1.0 ? "是" : "否",
               mx > cl*1.0005 ? (mx > 1.0 ? "<<< 严重超限且越过 ±1.0" : "<<< 超限") : "OK");
    }

    printf("\n=== 逐块追踪 pk=100, ceil=-18dB ===\n");
    {
        const float pk = 100.f, ceilDb = -18.f;
        const int N = 256;
        std::vector<float> io((size_t)N*2, 0.f);
        for (int b = 0; b < 6; ++b)
            for (int i = 0; i < 32; ++i) {
                int k = b*32+i; float s = (i%2)?pk:-pk;
                io[(size_t)k*2]=s; io[(size_t)k*2+1]=s;
            }
        std::vector<float> orig = io;
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn=false; p.bOn=true; p.ceilDb=ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float cl = dbToLin(ceilDb);
        printf("ceilLin=%.5f\n", cl);
        printf("%-5s %-12s %-12s %-14s %s\n","块","输入峰值","输出峰值","超限dB","");
        for (int b = 0; b < 8; ++b) {
            float pin=0, pout=0;
            for (int i=0;i<32;++i){int k=b*32+i;
                pin=std::max(pin,std::fabs(orig[(size_t)k*2]));
                pout=std::max(pout,std::fabs(io[(size_t)k*2]));}
            printf("%-5d %-12.4f %-12.5f %-14.3f %s\n", b, pin, pout,
                   20*std::log10(std::max(pout,1e-9f)/cl),
                   pout > cl*1.0005f ? "<<< 超限" : "");
        }
        printf("\n=> 满幅块的数据在**下一个块**才被输出，而那时的 target 来自安静块(=1.0)，\n");
        printf("   所以这些 %g 幅度的样本几乎原样输出（只被 relLimCoef_ 放松了一点点）。\n", pk);
        printf("   gain_ 是按「下一块」算的，这个前视的方向**反了**：\n");
        printf("   要保护块 k 的样本，必须用块 k 自己的峰值（或更早块的峰值），而不是更晚的。\n");
    }
    return 0;
}
