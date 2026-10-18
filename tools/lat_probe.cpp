// lat_probe.cpp - fresh client per attempt, report achievable buffer sizes
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <string>

static const PROPERTYKEY kName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static void mkfmt(WAVEFORMATEXTENSIBLE& wf) {
    memset(&wf, 0, sizeof(wf));
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = 48000;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = 48000 * 8;
    wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = 3;
    static const GUID kFloat = {0x3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    wf.SubFormat = kFloat;
}

static void tryInit(IMMDevice* d, int mode, int durUs, int per, const char* label) {
    IAudioClient* cl = nullptr;
    if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl)) || !cl) {
        printf("   %-24s activate failed\n", label);
        return;
    }
    WAVEFORMATEXTENSIBLE wf; mkfmt(wf);
    HRESULT hr;
    if (mode == 2) {
        IAudioClient3* c3 = nullptr;
        hr = cl->QueryInterface(__uuidof(IAudioClient3), (void**)&c3);
        if (FAILED(hr) || !c3) { printf("   %-24s no IAudioClient3\n", label); cl->Release(); return; }
        hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, &wf.Format, nullptr);
        if (SUCCEEDED(hr)) {
            UINT32 bf = 0; c3->GetBufferSize(&bf);
            printf("   %-24s buf=%u frames  %.2f ms\n", label, bf, bf * 1000.0 / 48000.0);
            c3->Release(); cl->Release(); return;
        }
        c3->Release();
    } else {
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = cl->Initialize(mode == 0 ? AUDCLNT_SHAREMODE_SHARED : AUDCLNT_SHAREMODE_EXCLUSIVE,
                            flags, durUs, mode == 0 ? 0 : durUs, &wf.Format, nullptr);
        if (SUCCEEDED(hr)) {
            UINT32 bf = 0; cl->GetBufferSize(&bf);
            printf("   %-24s buf=%u frames  %.2f ms\n", label, bf, bf * 1000.0 / 48000.0);
            cl->Release(); return;
        }
    }
    printf("   %-24s FAIL 0x%08lX\n", label, (unsigned long)hr);
    cl->Release();
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    for (int flow = 0; flow < 2; ++flow) {
        IMMDeviceCollection* col = nullptr;
        en->EnumAudioEndpoints(flow ? eCapture : eRender, DEVICE_STATE_ACTIVE, &col);
        UINT32 n = 0; col->GetCount(&n);
        for (UINT32 i = 0; i < n; ++i) {
            IMMDevice* d = nullptr; col->Item(i, &d);
            IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
            PROPVARIANT v; PropVariantInit(&v); ps->GetValue(kName, &v);
            std::wstring nm = (v.vt == VT_LPWSTR && v.pwszVal) ? v.pwszVal : L"";
            PropVariantClear(&v); ps->Release();
            printf("=== %s %u: %ls\n", flow ? "CAPT" : "PLAY", i, nm.c_str());
            tryInit(d, 0, 100000, 0, "shared 10ms");
            tryInit(d, 0, 20000, 0, "shared 2ms");
            tryInit(d, 1, 20000, 0, "excl 2ms");
            tryInit(d, 1, 10000, 0, "excl 1ms");
            tryInit(d, 2, 0, 480, "ac3 period 480");
            tryInit(d, 2, 0, 240, "ac3 period 240");
            tryInit(d, 2, 0, 144, "ac3 period 144");
            d->Release();
        }
        col->Release();
    }
    en->Release(); CoUninitialize();
    return 0;
}
