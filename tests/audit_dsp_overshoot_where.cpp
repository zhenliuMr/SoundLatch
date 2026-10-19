// audit_dsp_overshoot_where.cpp — 精确定位主通路限幅超限样本的位置与根因
//
// 待查：随机噪声输入下，超限样本数恒为 102（与 ceilDb 无关）——
// 这个「恒定」很可疑，说明超限不是随机的，而是集中在某处。
//
// 代码路径回顾（dsp.h:403-424）：
//   块 k: pk_k = 块 k 的峰值
//         limPrev_ = limCur_(块 k-1 留下的)
//         limCur_  = f(pk_k)                 <- 目标增益
//         输出 pendL_(=块 k-1 的数据) * lerp(limPrev_, limCur_)
//   => 块 k-1 的数据用的增益是「块 k-1 的目标」渐变到「块 k 的目标」。
//   若块 k 更安静，limCur_ 更大 -> 块 k-1 的样本被逐渐**放松**限幅 -> 超限。
//   松放量上界 = relLimCoef_ = 1-exp(-32/(fs*0.08))。
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
#include "dsp.h"

using namespace sl;

int main() {
    printf("relLimCoef_ 上界 = 1-exp(-32/(48000*0.08)) = %.6f = %.4f dB\n",
           1.0f - std::exp(-32.0f/(48000.0f*0.080f)),
           20*std::log10(1.0f - std::exp(-32.0f/(48000.0f*0.080f))));

    const int N = 48000;
    for (float ceilDb : {-18.f, -6.f, 0.f}) {
        std::vector<float> io((size_t)N*2);
        std::mt19937 rng(9);
        std::uniform_real_distribution<float> ud(-1.f,1.f);
        for (int i = 0; i < N; ++i) {
            float s = ud(rng)*2.f;
            if (i>=20000 && i<20064) s = ((i&1)?1.f:-1.f);
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        std::vector<float> orig = io;
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn=false; p.bOn=true; p.ceilDb=ceilDb;
        d.SetParams(p);
        d.Process(io.data(), N);
        float cl = dbToLin(ceilDb);
        std::vector<int> over;
        for (int i = 0; i < N; ++i)
            if (std::fabs(io[(size_t)i*2]) > cl*1.0005f) over.push_back(i);
        printf("\nceilDb=%+.1f ceil=%.6f  超限样本 %zu 个\n", ceilDb, cl, over.size());
        if (!over.empty()) {
            printf("  位置范围 [%d .. %d]\n", over.front(), over.back());
            // 聚类
            int clusters = 0, start = over[0], prev = over[0];
            for (size_t i = 1; i < over.size(); ++i) {
                if (over[i] > prev+1) { ++clusters; start = over[i]; }
                prev = over[i];
            }
            ++clusters;
            printf("  聚成 %d 段。逐段列出（段首样本 / 该处输入幅度 / 输出幅度 / 超限 dB）：\n", clusters);
            size_t i2 = 0;
            while (i2 < over.size()) {
                size_t j2 = i2;
                while (j2+1 < over.size() && over[j2+1] == over[j2]+1) ++j2;
                int k = over[i2];
                float gin = std::fabs(orig[(size_t)k*2]);
                float gout = std::fabs(io[(size_t)k*2]);
                printf("    [%6d..%6d] len=%-4zu 输入=%.5f 输出=%.5f 超限=%+.3f dB  (t=%.1f ms, 块=%d)\n",
                       over[i2], over[j2], j2-i2+1, gin, gout,
                       20*std::log10(gout/cl), k/48.0, k/32);
                i2 = j2+1;
            }
        }
    }
    printf("\n=== 结论验证：超限是否恒定发生在「块 k-1 有峰、块 k 安静」处 ===\n");
    // 构造纯净的单次事件
    {
        const int M = 4800;
        std::vector<float> io((size_t)M*2, 0.f);
        for (int i = 0; i < M; ++i) {
            float s = 0.f;
            if (i >= 32 && i < 64)  s = 1.0f;    // 块 1：满幅
            if (i >= 64 && i < 96)  s = 0.0f;    // 块 2：静音
            io[(size_t)i*2] = s; io[(size_t)i*2+1] = s;
        }
        DspCore d; d.Prepare(48000.f);
        DspParams p; p.aOn=false; p.bOn=true; p.ceilDb=-6.f;
        d.SetParams(p);
        d.Process(io.data(), M);
        float cl = dbToLin(-6.f);
        printf("  ceil=-6dB (%.5f)\n", cl);
        printf("  块0(静音) 输出峰值=%.5f\n", [&]{float m=0;for(int i=0;i<32;++i)m=std::max(m,std::fabs(io[(size_t)i*2]));return m;}());
        printf("  块1(满幅) 输出峰值=%.5f   <- 这块的数据在块2 才被输出\n",
               [&]{float m=0;for(int i=32;i<64;++i)m=std::max(m,std::fabs(io[(size_t)i*2]));return m;}());
        printf("  块2(静音) 输出峰值=%.5f\n", [&]{float m=0;for(int i=64;i<96;++i)m=std::max(m,std::fabs(io[(size_t)i*2]));return m;}());
        printf("  块3      输出峰值=%.5f\n", [&]{float m=0;for(int i=96;i<128;++i)m=std::max(m,std::fabs(io[(size_t)i*2]));return m;}());
        printf("\n  => 满幅块被搬到了第 2 个控制块才输出（1 块 = 0.67ms 前视），\n");
        printf("     且它用的增益是「静音块算出的 target」，所以只压到 ceil 而非精确压到 ceil。\n");
    }
    return 0;
}
