// probe_bandsel.cpp — 如果改成「只压 2–5 kHz 枪声带」，代价是什么
//
// 不需要先改 DSP：直接用真实素材的功率谱做「假设性频带增益」推演，
// 得到三项硬指标：
//   1) 2–5k 占多少能量 / 占多少听觉权重（A 加权）—— 决定「压它」能不能压住炸耳感
//   2) 压 2–5k 之后谱心、A 加权电平、总 RMS 变成多少 —— 决定音色往哪偏
//   3) 枪声在 250–2k（脚步带）还有多少能量 —— 决定脚步的掩蔽有没有被解决
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "dsp.h"

using namespace sl;
static const double kPi = 3.14159265358979323846;

static bool loadWav(const char* path, std::vector<float>& L, std::vector<float>& R, float& fs) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open fail: %s\n", path); return false; }
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
            if (ch < 1 || bps < 1 || csz < 4) { off += 8 + csz + (csz & 1); continue; }
            int frames = (int)(csz / (size_t)(ch * bps));
            L.assign((size_t)frames, 0.f); R.assign((size_t)frames, 0.f);
            for (int i = 0; i < frames; ++i) {
                const unsigned char* q = p + (size_t)i * ch * bps;
                float v[2] = {0.f, 0.f};
                for (int c = 0; c < (ch < 2 ? 1 : 2); ++c) {
                    const unsigned char* r = q + c * bps;
                    float x;
                    if (fmt == 3 && bps == 4) x = *(const float*)r;
                    else if (fmt == 3 && bps == 8) x = (float)*(const double*)r;
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

static void fft(std::vector<double>& re, std::vector<double>& im) {
    size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * kPi / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (size_t k = 0; k < len / 2; ++k) {
                double ur = re[i+k], ui = im[i+k];
                double vr = re[i+k+len/2]*cr - im[i+k+len/2]*ci;
                double vi = re[i+k+len/2]*ci + im[i+k+len/2]*cr;
                re[i+k] = ur+vr; im[i+k] = ui+vi;
                re[i+k+len/2] = ur-vr; im[i+k+len/2] = ui-vi;
                double ncr = cr*wr - ci*wi; ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
}

// A 加权（IEC 61672），返回线性权重
static double aWeight(double f) {
    if (f < 1) f = 1;
    double f2 = f*f, f4 = f2*f2;
    double ra = (12194.0*12194.0*f4) /
                ((f2 + 20.6*20.6) * sqrt((f2 + 107.7*107.7)*(f2 + 737.9*737.9)) *
                 (f2 + 12194.0*12194.0));
    return ra * ra;                              // 功率权重
}

struct Spec {
    std::vector<double> p;      // 每个 bin 的功率
    double df = 0;
    double total() const { double s = 0; for (double v : p) s += v; return s; }
    double band(double f1, double f2) const {
        double s = 0;
        for (size_t k = 1; k < p.size(); ++k) {
            double f = k * df;
            if (f >= f1 && f < f2) s += p[k];
        }
        return s;
    }
    // 施加频带增益后重算：谱心 / A 加权能量 / 总能量
    void metrics(const double* gains, int nG, const double* gf, double& cen, double& aw, double& tot) const {
        double num = 0, den = 0, awS = 0, totS = 0;
        for (size_t k = 1; k < p.size(); ++k) {
            double f = k * df;
            double g = 1.0;
            for (int b = 0; b < nG; ++b) {
                double lo = gf[b], hi = gf[b+1];
                if (f >= lo && f < hi) { g = gains[b]; break; }
            }
            double pv = p[k] * g * g;
            num += f * pv; den += pv;
            awS += pv * aWeight(f);
            totS += pv;
        }
        cen = den > 0 ? num / den : 0;
        aw = awS;
        tot = totS;
    }
};

// Welch 平均功率谱
static Spec spectrum(const std::vector<float>& x, size_t from, size_t n, int N) {
    Spec s; s.df = 48000.0 / N;
    s.p.assign(N/2, 0.0);
    int frames = (int)((n - N) / (N/2) + 1);
    if (frames < 1) frames = 1;
    for (int fr = 0; fr < frames; ++fr) {
        size_t off = from + (size_t)fr * (N/2);
        if (off + N > x.size()) break;
        std::vector<double> re(N), im(N, 0.0);
        for (int i = 0; i < N; ++i) re[i] = x[off+i] * (0.5 - 0.5*cos(2*kPi*i/(N-1)));
        fft(re, im);
        for (int k = 1; k < N/2; ++k) s.p[k] += re[k]*re[k] + im[k]*im[k];
    }
    return s;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "C:/Users/Administrator/AppData/Local/Temp/sl";
    char path[1024];
    struct Item { const char* name; const char* file; };
    Item items[] = {
        {"枪声 A 短促", "w3144728661.wav"},
        {"枪声 B 长尾", "w2522402869.wav"},
        {"脚步 近",     "w2253900037.wav"},
        {"脚步 中",     "w2205415477.wav"},
        {"环境 低频",   "w2833643493.wav"},
    };

    printf("=========== 1. 各声音的能量分布（2–5 kHz 到底占多少） ===========\n");
    printf("%-12s %7s %7s %7s %7s %7s %8s %8s %8s\n",
           "声音", "<150", "150-600", "0.6-2k", "2-5k", ">5k", "2-5k权重", "谱心", "A加权2-5k");
    for (auto& it : items) {
        snprintf(path, sizeof(path), "%s/%s", dir, it.file);
        std::vector<float> L, R; float fs = 48000.f;
        if (!loadWav(path, L, R, fs)) { printf("%-12s  <读取失败>\n", it.name); continue; }
        Spec s = spectrum(L, 0, L.size(), 4096);
        double T = s.total();
        double b0 = s.band(20,150), b1 = s.band(150,600), b2 = s.band(600,2000),
               b3 = s.band(2000,5000), b4 = s.band(5000,20000);
        double cen, aw, tot;
        double g1[] = {1,1,1,1,1}, gf[] = {20,150,600,2000,5000,20000};
        s.metrics(g1, 5, gf, cen, aw, tot);
        double aw3 = 0;
        for (size_t k = 1; k < s.p.size(); ++k) {
            double f = k*s.df;
            if (f >= 2000 && f < 5000) aw3 += s.p[k]*aWeight(f);
        }
        printf("%-12s %6.1f%% %6.1f%% %6.1f%% %6.1f%% %6.1f%% %7.1f%% %7.0f %8.1f%%\n",
               it.name, 100*b0/T, 100*b1/T, 100*b2/T, 100*b3/T, 100*b4/T,
               100*aw3/aw, cen, 100*aw3/aw);
    }

    printf("\n=========== 2. 只把 2–5k 压下去，声音会变成什么 ===========\n");
    printf("（对『2-5k 之外』完全不动；这是理想选择性限幅的上限效果）\n\n");
    printf("%-12s %-10s %8s %9s %9s %10s %10s\n",
           "声音", "2-5k压缩", "A加权", "总RMS", "谱心Hz", "谱心偏移", "250-2k占比");
    for (auto& it : items) {
        snprintf(path, sizeof(path), "%s/%s", dir, it.file);
        std::vector<float> L, R; float fs = 48000.f;
        if (!loadWav(path, L, R, fs)) continue;
        Spec s = spectrum(L, 0, L.size(), 4096);
        double cen0, aw0, tot0;
        double hiEdge = (argc > 2) ? atof(argv[2]) : 5000.0;
        double gf[] = {20,150,600,2000,hiEdge,20000};
        double g0[] = {1,1,1,1,1};
        s.metrics(g0, 5, gf, cen0, aw0, tot0);
        double b2 = s.band(250,2000), T = s.total();
        for (double cut : {6.0, 12.0, 18.0}) {
            double g[] = {1,1,1, dbToLin((float)-cut), 1};
            double cen1, aw1, tot1;
            s.metrics(g, 5, gf, cen1, aw1, tot1);
            printf("%-12s %-10.0f %8.1f %9.1f %9.0f %10.0f %9.1f%%\n",
                   it.name, cut,
                   10*log10(aw1/aw0 + 1e-30), 10*log10(tot1/tot0 + 1e-30),
                   cen1, cen1 - cen0, 100*b2/T);
        }
        printf("\n");
    }

    printf("=========== 3. 掩蔽源还剩多少（掩蔽脚步的是枪声的这些频段） ===========\n");
    printf("%-12s %8s %8s %8s %8s\n", "枪声", "<250", "250-2k", "2-5k", ">5k");
    for (int i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/%s", dir, items[i].file);
        std::vector<float> L, R; float fs = 48000.f;
        if (!loadWav(path, L, R, fs)) continue;
        Spec s = spectrum(L, 0, L.size(), 4096);
        double T = s.total();
        printf("%-12s %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", items[i].name,
               100*s.band(20,250)/T, 100*s.band(250,2000)/T,
               100*s.band(2000,5000)/T, 100*s.band(5000,20000)/T);
    }
    return 0;
}
