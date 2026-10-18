// apo_test.cpp - loads the built APO, runs a block through it, checks the limiter.
#include "../apo/src/apo_defs.h"
#include <cstdio>
#include <cmath>
#include <vector>

typedef HRESULT(__stdcall* PFN_GetClassObject)(REFCLSID, REFIID, void**);

// fake IAudioMediaType (only GetAudioFormat is ever called)
struct FakeMediaType : public IUnknown {
    WAVEFORMATEXTENSIBLE wf{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    virtual HRESULT STDMETHODCALLTYPE GetAudioFormat(WAVEFORMATEX** pp) { *pp = &wf.Format; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* b) { *b = FALSE; return S_OK; }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("start\n");
    HMODULE h = LoadLibraryW(L"dist-apo\\SoundLockApo.dll");
    if (!h) { printf("[FAIL] LoadLibrary: %lu\n", GetLastError()); return 1; }
    printf("loaded\n");

    auto getClass = (PFN_GetClassObject)GetProcAddress(h, "DllGetClassObject");
    if (!getClass) { printf("[FAIL] no DllGetClassObject\n"); return 1; }

    IClassFactory* fac = nullptr;
    HRESULT hr = getClass(CLSID_SoundLockApo, IID_IClassFactory, (void**)&fac);
    if (FAILED(hr) || !fac) { printf("[FAIL] DllGetClassObject 0x%08lX\n", (unsigned long)hr); return 1; }

    IAudioProcessingObjectRT* rt = nullptr;
    hr = fac->CreateInstance(nullptr, IID_IAudioProcessingObjectRT, (void**)&rt);
    if (FAILED(hr) || !rt) { printf("[FAIL] CreateInstance 0x%08lX\n", (unsigned long)hr); return 1; }

    APO_REG_PROPERTIES* props = nullptr;
    hr = rt->GetRegistrationProperties(&props);
    if (SUCCEEDED(hr) && props) {
        printf("[T1] registration: %ls flags=0x%X v%u.%u  PASS\n",
               props->szFriendlyName, (unsigned)props->Flags, props->u32MajorVersion, props->u32MinorVersion);
        CoTaskMemFree(props);
    } else {
        printf("[T1] GetRegistrationProperties FAIL\n");
    }

    HNSTIME lat = 0;
    rt->GetLatency(&lat);
    printf("[T2] reported latency: %.2f ms\n", lat / 10000.0);

    // lock
    APO_CONNECTION_DESCRIPTOR inDesc{}, outDesc{};
    FakeMediaType mt;
    memset(&mt.wf, 0, sizeof(mt.wf));
    mt.wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mt.wf.Format.nChannels = 2;
    mt.wf.Format.nSamplesPerSec = 48000;
    mt.wf.Format.wBitsPerSample = 32;
    mt.wf.Format.nBlockAlign = 8;
    mt.wf.Format.nAvgBytesPerSec = 48000 * 8;
    mt.wf.Format.cbSize = 22;
    mt.wf.Samples.wValidBitsPerSample = 32;
    mt.wf.dwChannelMask = 3;

    const int N = 4800;
    std::vector<float> buf((size_t)N * 2, 0.f);
    inDesc.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inDesc.pBuffer = (UINT_PTR)buf.data();
    inDesc.u32MaxFrameCount = N;
    inDesc.pFormat = (IAudioMediaType*)&mt;
    outDesc = inDesc;

    APO_CONNECTION_DESCRIPTOR* pin[1] = {&inDesc};
    APO_CONNECTION_DESCRIPTOR* pout[1] = {&outDesc};

    printf("A: query config\n");
    IAudioProcessingObjectConfiguration* cfg = nullptr;
    hr = rt->QueryInterface(IID_IAudioProcessingObjectConfiguration, (void**)&cfg);
    if (FAILED(hr) || !cfg) { printf("[FAIL] QueryInterface(config) 0x%08lX\n", (unsigned long)hr); return 1; }

    printf("B: lock\n");
    hr = cfg->LockForProcess(1, pin, 1, pout);
    if (FAILED(hr)) { printf("[FAIL] LockForProcess 0x%08lX\n", (unsigned long)hr); return 1; }
    printf("[T3] LockForProcess  float32/48k/2ch  PASS\n");

    // 0 dBFS sine -> default ceiling -3 dBFS (0.7079)
    for (int i = 0; i < N; ++i) {
        float s = sinf(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }
    APO_CONNECTION_PROPERTY inP{};
    inP.pBuffer = (UINT_PTR)buf.data();
    inP.u32ValidFrameCount = N;
    inP.BufferFlags = BUFFER_VALID;
    APO_CONNECTION_PROPERTY outP = inP;
    APO_CONNECTION_PROPERTY* ip[1] = {&inP};
    APO_CONNECTION_PROPERTY* op[1] = {&outP};

    rt->APOProcess(1, ip, 1, op);

    float peak = 0.f;
    for (int i = 0; i < N; ++i) peak = std::max(peak, fabsf(buf[2 * i]));
    float ceil = powf(10.0f, -3.0f / 20.0f);
    printf("[T4] limiter: peak %.4f  ceiling %.4f  %s\n", peak, ceil,
           peak <= ceil + 0.002f ? "PASS" : "FAIL");

    // 16-bit / mono formats must bypass without crashing
    mt.wf.Format.wBitsPerSample = 16;
    cfg->UnlockForProcess();
    cfg->LockForProcess(1, pin, 1, pout);
    memset(buf.data(), 0, buf.size() * sizeof(float));
    for (int i = 0; i < N; ++i) buf[2 * i] = buf[2 * i + 1] = 0.5f;
    rt->APOProcess(1, ip, 1, op);
    printf("[T5] non-float input -> passthrough: %.3f  %s\n", buf[0],
           fabsf(buf[0] - 0.5f) < 1e-6f ? "PASS" : "FAIL");

    cfg->UnlockForProcess();
    cfg->Release();
    rt->Release();
    fac->Release();
    FreeLibrary(h);
    return 0;
}
