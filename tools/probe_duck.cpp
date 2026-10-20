// probe_duck.cpp — 标定「枪声事件 duck」的两个判据，并在真实素材上验证选择性
//
// 打印每 50 ms 的 f3（>3k 能量占比）、tAll（全带快慢包络差）、duck（生效比例）、trSm（脚步判据）
// 用法: probe_duck <wav...>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;

static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R, float& fs) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    size_t off = 12; int ch = 2, bits = 16, fmt = 3; long sr = 48000;
    while (off + 8 <= (size_t)sz) {
        uint32_t id = (uint32_t)buf[off] | ((uint32_t)buf[off+1]<<8) |
                      ((uint32_t)buf[off+2]<<16) | ((uint32_t)buf[off+3]<<24);
        uint32_t csz = (uint32_t)buf[off+4] | ((uint32_t)buf[off+5]<<8) |
                       ((uint32_t)buf[off+6]<<16) | ((uint32_t)buf[off+7]<<24);
        const unsigned char* p = buf.data() + off + 8;
        if (off + 8 + (size_t)csz > (size_t)sz) break;
        if (id == 0x20746D66 && csz >= 16) {
            fmt = p[0] | (p[1]<<8); ch = p[2] | (p[3]<<8);
            sr = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            int bps = bits / 8;
            if (ch < 1 || bps < 1) { off += 8 + csz + (csz & 1); continue; }
            int frames = (int)(csz / (size_t)(ch * bps));
            L.assign((size_t)frames, 0.f); R.assign((size_t)frames, 0.f);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i * ch * bps;
                float v[2] = {0.f, 0.f};
                for (int c = 0; c < (ch < 2 ? 1 : 2); ++c) {
                    const unsigned char* r = q + c * bps;
                    float x;
                    if (fmt == 3 && bps == 4) x = *(const float*)r;
                    else if (bps == 2) x = *(const int16_t*)r / 32768.0f;
                    else if (bps == 3) x = (float)((int32_t)(r[0] | (r[1]<<8) | ((int8_t)r[2]<<16))) / 8388608.0f;
                    else x = (float)(*(const int32_t*)r) / 2147483648.0f;
                    v[c] = x;
                }
                if (ch < 2) v[1] = v[0];
                L[(size_t)i] = v[0]; R[(size_t)i] = v[1];
            }
            fs = (float)sr;
            return true;
        }
        off += 8 + (size_t)csz + (csz & 1);
    }
    return false;
}

static void resample(std::vector<float>& L, std::vector<float>& R, float fsIn, float fsOut) {
    if (fabsf(fsIn - fsOut) < 1.f) return;
    size_t nIn = L.size();
    size_t nOut = (size_t)((double)nIn * fsOut / fsIn);
    std::vector<float> l(nOut), r(nOut);
    double step = (double)fsIn / fsOut;
    for (size_t i = 0; i < nOut; ++i) {
        double x = i * step; size_t i0 = (size_t)x;
        if (i0 >= nIn) i0 = nIn - 1;
        size_t i1 = std::min(i0 + 1, nIn - 1);
        double fr = x - (double)i0;
        l[i] = (float)(L[i0] + (L[i1] - L[i0]) * fr);
        r[i] = (float)(R[i0] + (R[i1] - R[i0]) * fr);
    }
    L.swap(l); R.swap(r);
}

int main(int argc, char** argv) {
    for (int a = 1; a < argc; ++a) {
        std::vector<float> L, R; float fs = 48000.f;
        if (!loadWav(argv[a], L, R, fs)) { printf("%s: 读取失败\n", argv[a]); continue; }
        resample(L, R, fs, 48000.f);
        std::vector<float> io(L.size()*2);
        for (size_t i = 0; i < L.size(); ++i) { io[2*i] = L[i]; io[2*i+1] = R[i]; }
        DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f;
        p.ceilDb = 0.f; p.lookMs = 1.5f; p.bypass = false;
        DspCore d; d.Prepare(48000.f); d.SetParams(p);

        const char* base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        printf("\n=== %s (%.2f s) ===\n", base, L.size()/48000.0);
        printf("%7s %6s %7s %6s %6s\n", "t(s)", "f3", "tAll", "duck", "trSm");
        double maxDuck = 0, maxF3 = 0, maxT = 0;
        const size_t chunk = 480;  // 10 ms
        for (size_t i = 0; i + chunk <= io.size()/2; i += chunk) {
            d.Process(io.data() + i*2, (int)chunk);
            maxDuck = std::max(maxDuck, (double)d.LastDuck());
            if (d.LastDuck() > 0.2f) {           // 只在真的触发时打点，避免刷屏
                maxF3 = std::max(maxF3, (double)d.LastF3());
                maxT  = std::max(maxT,  (double)d.LastTAll());
            }
            if ((i / chunk) % 5 == 0)
                printf("%7.2f %6.3f %7.1f %6.3f %6.3f\n",
                       (double)i/48000.0, d.LastF3(), d.LastTAll(), d.LastDuck(), d.LastTrSm());
        }
        printf("-> maxDuck %.3f  触发时 f3 峰值 %.3f  tAll 峰值 %.1f dB\n", maxDuck, maxF3, maxT);
    }
    return 0;
}
