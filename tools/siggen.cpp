// siggen.cpp - render a test signal to a chosen output device (for latency/level testing)
// usage: SigGen.exe [deviceNameSubstring] [seconds] [mode]
//   mode: tone | burst | noise
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

static const PROPERTYKEY kName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

int wmain(int argc, wchar_t** argv) {
    const wchar_t* want = argc > 1 ? argv[1] : L"CABLE Input";
    double secs = argc > 2 ? _wtof(argv[2]) : 10.0;
    std::string mode = argc > 3 ? "burst" : "burst";
    if (argc > 3) {
        char buf[32];
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, buf, 32, nullptr, nullptr);
        mode = buf;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);

    IMMDevice* dev = nullptr;
    UINT32 n = 0; col->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v; PropVariantInit(&v);
        ps->GetValue(kName, &v);
        std::wstring name = (v.vt == VT_LPWSTR && v.pwszVal) ? v.pwszVal : L"";
        PropVariantClear(&v); ps->Release();
        wprintf(L"[%u] %s\n", i, name.c_str());
        if (!dev && name.find(want) != std::wstring::npos) { dev = d; } else d->Release();
    }
    if (!dev) { wprintf(L"device not found: %s\n", want); return 1; }

    IAudioClient* cl = nullptr;
    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl);
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = 48000 * 8;
    cl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                   100000, 0, &wf, nullptr);
    UINT32 bufFrames = 0; cl->GetBufferSize(&bufFrames);
    IAudioRenderClient* rc = nullptr;
    cl->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    cl->Start();

    long long total = (long long)(secs * 48000);
    long long done = 0;
    double ph = 0;
    unsigned rng = 12345;
    while (done < total) {
        UINT32 pad = 0; cl->GetCurrentPadding(&pad);
        UINT32 frames = bufFrames - pad;
        if (!frames) { Sleep(2); continue; }
        BYTE* p = nullptr; rc->GetBuffer(frames, &p);
        float* f = (float*)p;
        for (UINT32 i = 0; i < frames; ++i, ++done) {
            double t = (double)done / 48000.0;
            float s = 0;
            if (mode == "tone") s = 0.1f * sinf((float)(2 * 3.14159265 * 1000.0 * t));
            else if (mode == "noise") { rng = rng * 1103515245u + 12345u; s = 0.05f * ((float)((rng >> 9) & 0x7fff) / 16384.0f - 1.0f); }
            else {  // 120 ms 1 kHz burst every 600 ms  (footstep-ish cadence)
                double ph2 = fmod(t, 0.6);
                if (ph2 < 0.12) s = 0.12f * sinf((float)(2 * 3.14159265 * 900.0 * t)) * (float)(1.0 - ph2 / 0.12);
            }
            f[2 * i] = s; f[2 * i + 1] = s;
        }
        rc->ReleaseBuffer(frames, 0);
    }
    cl->Stop();
    rc->Release(); cl->Release(); dev->Release(); col->Release(); en->Release();
    CoUninitialize();
    return 0;
}
