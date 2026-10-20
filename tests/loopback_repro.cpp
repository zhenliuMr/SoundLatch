// loopback_repro.cpp — 复现「勾选回环捕获后闪退」：直接走引擎路径，不经过 UI
#include <cstdio>
#include <thread>
#include <chrono>
#include "../src/engine.h"

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int secs = argc > 1 ? atoi(argv[1]) : 4;

    std::wstring ren = sl::AudioEngine::DefaultRenderId();
    printf("render id: %ls\n", ren.c_str());

    sl::DspCore dsp;
    dsp.Prepare(48000.0f);
    sl::DspParams p;
    p.aOn = true; p.bOn = true; p.ceilDb = -3.f;
    dsp.SetParams(p);

    sl::AudioEngine eng;
    std::wstring err;
    bool ok = eng.Start(L"", ren, /*loopback*/ true, &dsp, err);
    printf("Start(loopback) = %d  err=%ls  lat=%.1f ms\n", ok ? 1 : 0, err.c_str(), eng.LatencyMs());
    if (!ok) return 2;

    for (int i = 0; i < secs * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        printf("  t=%.1fs lat=%.1f ring=%.1f under=%d pkts=%llu frames=%llu\n",
               (i + 1) / 10.0, eng.LatencyMs(), eng.RingMs(), eng.Underruns(),
               eng.CapPkts(), eng.CapFrames());
        fflush(stdout);
    }
    eng.Stop();
    printf("stopped cleanly\n");
    CoUninitialize();
    return 0;
}
