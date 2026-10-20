// fullscan.cpp — 全量扫描 samples/wem 下每一条 Wwise 流：解码 → 过一遍完整 DSP → 记录指标
//
// 设计要点（快）：
//   · 解码走 `vgmstream-cli -i -w -p`，PCM 直接进管道，不落盘（12022 条 = 不产生几十 GB 临时文件）
//   · -i 忽略循环点：测的是素材本身，不是 "2 遍循环 + 10 s 淡出"
//   · -w 保持原float32，避免 PCM16 量化影响峰值/削顶判定
//   · 多线程按文件并行，线程数默认 = 物理核数
//
// 输出：CSV（每文件一行）+ 汇总（总时长、总耗时、实时倍数、各类触发比例）
// 用法: fullscan <list.txt> <out.csv> [threads] [vgmstream.exe]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include "dsp.h"

#ifdef _WIN32
#include <windows.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

using namespace sl;

struct Metrics {
    std::string name;
    double dur = 0, peakIn = 0, peakOut = 0, rmsIn = 0;
    long   clip = 0;
    double duckMax = 0, duckMs = 0, f3Max = 0, tiltMax = 0, trSmMax = 0, overMax = 0;
    bool   ok = false, nan = false;
};

// ---- WAV 解析：从内存里的 WAV 取出 float 单声道 48k ----
static bool wavToMono48(const std::vector<unsigned char>& raw, std::vector<float>& mono, float& srcRate) {
    if (raw.size() < 44) return false;
    size_t off = 12;
    int ch = 2, bits = 16, fmt = 3; long sr = 48000;
    const unsigned char* data = nullptr; size_t dataBytes = 0;
    while (off + 8 <= raw.size()) {
        uint32_t id = (uint32_t)raw[off] | ((uint32_t)raw[off+1]<<8) |
                      ((uint32_t)raw[off+2]<<16) | ((uint32_t)raw[off+3]<<24);
        uint32_t csz = (uint32_t)raw[off+4] | ((uint32_t)raw[off+5]<<8) |
                       ((uint32_t)raw[off+6]<<16) | ((uint32_t)raw[off+7]<<24);
        const unsigned char* p = raw.data() + off + 8;
        if (off + 8 + (size_t)csz > raw.size()) {
            if (id == 0x61746164) { data = p; dataBytes = raw.size() - (off + 8); }  // data 截断也算
            break;
        }
        if (id == 0x20746D66 && csz >= 16) {
            fmt = p[0] | (p[1]<<8);
            ch  = p[2] | (p[3]<<8);
            sr  = (long)p[4] | ((long)p[5]<<8) | ((long)p[6]<<16) | ((long)p[7]<<24);
            bits = p[14] | (p[15]<<8);
        } else if (id == 0x61746164) {
            data = p; dataBytes = csz;
        }
        off += 8 + (size_t)csz + (csz & 1);
    }
    if (!data || ch < 1 || bits < 8) return false;
    int bps = bits / 8;
    size_t frames = dataBytes / (size_t)(ch * bps);
    if (!frames) return false;
    mono.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const unsigned char* q = data + i * ch * bps;
        float x;
        if (fmt == 3 && bps == 4)      x = *(const float*)q;
        else if (fmt == 3 && bps == 8) x = (float)*(const double*)q;
        else if (bps == 2)             x = *(const int16_t*)q / 32768.0f;
        else if (bps == 3)             x = (float)((int32_t)(q[0] | (q[1]<<8) | ((int8_t)q[2]<<16))) / 8388608.0f;
        else                           x = (float)(*(const int32_t*)q) / 2147483648.0f;
        mono[i] = x;
    }
    srcRate = (float)sr;
    return true;
}

static void resampleTo48(std::vector<float>& x, float fsIn) {
    if (fabsf(fsIn - 48000.f) < 1.f) return;
    size_t nOut = (size_t)((double)x.size() * 48000.0 / fsIn);
    std::vector<float> y(nOut);
    double step = (double)fsIn / 48000.0;
    for (size_t i = 0; i < nOut; ++i) {
        double p = i * step; size_t i0 = (size_t)p;
        if (i0 >= x.size()) i0 = x.size() - 1;
        size_t i1 = std::min(i0 + 1, x.size() - 1);
        double fr = p - (double)i0;
        y[i] = (float)(x[i0] + (x[i1] - x[i0]) * fr);
    }
    x.swap(y);
}

static Metrics scanOne(const std::string& path, const std::string& vgm) {
    Metrics m; m.name = path;
    // cmd.exe 的 /c 需要额外一层引号包裹整条命令，否则 "prog" 的头一个引号会被吃掉，
    // 命令名被截成路径首段（实测报 'tools' 不是内部或外部命令，输出 0 字节）。
    auto bs = [](std::string s) { for (auto& c : s) if (c == '/') c = '\\'; return s; };
    std::string cmd = "\"\"" + bs(vgm) + "\" -i -w -p \"" + bs(path) + "\" 2>NUL\"";
    FILE* fp = POPEN(cmd.c_str(), "rb");
    if (!fp) return m;
    std::vector<unsigned char> raw;
    raw.reserve(1 << 21);
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        raw.insert(raw.end(), buf, buf + n);
        if (raw.size() > (size_t)512 << 20) break;      // 512 MB 上限，防失控
    }
    PCLOSE(fp);
    // vgmstream 的 -p 会把 PCM 写 stdout，但 -w 仍会在源文件旁落下 <infile>.wav。
    // 12 022 条全跑一遍就是 13 GB 垃圾（实测）。读完全部内容后立刻删掉。
    std::remove((path + ".wav").c_str());
    std::vector<float> mono; float fs = 48000.f;
    if (!wavToMono48(raw, mono, fs)) return m;
    resampleTo48(mono, fs);
    if (mono.size() < 512) return m;

    m.dur = mono.size() / 48000.0;
    double acc = 0;
    for (float v : mono) { m.peakIn = std::max(m.peakIn, (double)fabsf(v)); acc += (double)v*v; }
    m.rmsIn = 10.0 * log10(acc / mono.size() + 1e-20);

    // 立体声（单声道复制），完整链路
    std::vector<float> io(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) { io[2*i] = mono[i]; io[2*i+1] = mono[i]; }

    DspParams p; p.aOn = true; p.bOn = true; p.envAmt = 0.55f; p.stepAmt = 0.65f;
    p.ceilDb = -3.f; p.lookMs = 1.5f; p.bypass = false;
    DspCore d; d.Prepare(48000.f); d.SetParams(p);

    const size_t chunk = 480;
    size_t duckBlocks = 0, blocks = 0;
    for (size_t i = 0; i + chunk <= mono.size(); i += chunk) {
        d.Process(io.data() + i * 2, (int)chunk);
        ++blocks;
        double dk = d.LastDuck();
        if (dk > m.duckMax) m.duckMax = dk;
        if (dk > 0.05) ++duckBlocks;
        m.f3Max   = std::max(m.f3Max,   (double)d.LastF3());
        m.tiltMax = std::max(m.tiltMax, (double)d.LastTilt());
        m.trSmMax = std::max(m.trSmMax, (double)d.LastTrSm());
        m.overMax = std::max(m.overMax, (double)d.LastOverMs());
        for (size_t k = 0; k < chunk; ++k) {
            float a = io[2*(i+k)], b = io[2*(i+k)+1];
            if (!std::isfinite(a) || !std::isfinite(b)) { m.nan = true; continue; }
            m.peakOut = std::max(m.peakOut, (double)std::max(fabsf(a), fabsf(b)));
        }
    }
    m.duckMs = blocks ? 10.0 * (double)duckBlocks : 0.0;
    m.clip = d.ClippedSamples();
    m.ok = true;
    return m;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: fullscan <list.txt> <out.csv> [threads] [vgmstream]\n"); return 1; }
    const char* listPath = argv[1];
    const char* csvPath  = argv[2];
    int threads = argc > 3 ? atoi(argv[3]) : 8;
    std::string vgm = argc > 4 ? argv[4] : "tools/vgm/vgmstream-cli.exe";

    std::vector<std::string> files;
    {
        FILE* f = fopen(listPath, "rb");
        if (!f) { printf("cannot open %s\n", listPath); return 1; }
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t L = strlen(line);
            while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
            if (L) files.push_back(line);
        }
        fclose(f);
    }
    printf("files: %zu, threads: %d\n", files.size(), threads);

    std::vector<Metrics> results(files.size());
    std::atomic<size_t> next{0};
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= files.size()) break;
                results[i] = scanOne(files[i], vgm);
                static std::atomic<size_t> done{0};
                size_t k = ++done;
                if (k % 500 == 0) printf("  %zu / %zu\n", k, files.size());
            }
        });
    }
    for (auto& th : pool) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double wall = std::chrono::duration<double>(t1 - t0).count();

    FILE* out = fopen(csvPath, "w");
    fprintf(out, "name,ok,dur,peakIn,peakOut,rmsIn,clip,nan,duckMax,duckMs,f3Max,tiltMax,trSmMax,overMax\n");
    double totalDur = 0, sumRms = 0; long bad = 0, clipFiles = 0, nanFiles = 0;
    for (const auto& m : results) {
        if (!m.ok) { ++bad; continue; }
        totalDur += m.dur; sumRms += m.rmsIn;
        if (m.clip) ++clipFiles;
        if (m.nan) ++nanFiles;
        fprintf(out, "%s,1,%.3f,%.4f,%.4f,%.1f,%ld,%d,%.3f,%.0f,%.3f,%.1f,%.3f,%.0f\n",
                m.name.c_str(), m.dur, m.peakIn, m.peakOut, m.rmsIn,
                m.clip, m.nan ? 1 : 0, m.duckMax, m.duckMs, m.f3Max, m.tiltMax, m.trSmMax, m.overMax);
    }
    fclose(out);

    printf("\n=== 汇总 ===\n");
    printf("成功 %zu / %zu   解码失败 %ld\n", files.size() - (size_t)bad, files.size(), bad);
    printf("音频总时长 %.2f h (%.1f s)\n", totalDur / 3600.0, totalDur);
    printf("墙钟耗时 %.1f s   -> %.0fx 实时（%d 线程）\n", wall, totalDur / wall, threads);
    printf("削顶/超限文件 %ld   非有限样本文件 %ld\n", clipFiles, nanFiles);
    return 0;
}
