// fmt48.cpp - set a *capture* endpoint's default format to 48k/32f via IPolicyConfig
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <cstdio>
#include <string>

static const PROPERTYKEY kName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// undocumented but stable: IPolicyConfig (Win7 .. Win11)
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static const CLSID kPolicyClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID kIPolicyConfig = {0xf8679f50, 0x850a, 0x3689, {0xa5, 0xd8, 0xde, 0x6c, 0x0f, 0x94, 0xd4, 0x5c}};

int main(int argc, char** argv) {
    const wchar_t* want = argc > 1 ? L"CABLE Output" : L"";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));

    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(kPolicyClient, nullptr, CLSCTX_ALL, kIPolicyConfig, (void**)&pc);
    if (FAILED(hr) || !pc) { printf("policy client failed 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
    UINT32 n = 0; col->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v; PropVariantInit(&v); ps->GetValue(kName, &v);
        std::wstring nm = (v.vt == VT_LPWSTR && v.pwszVal) ? v.pwszVal : L"";
        PropVariantClear(&v); ps->Release();
        if (nm.find(want) == std::wstring::npos) { d->Release(); continue; }

        LPWSTR id = nullptr;
        d->GetId(&id);
        WAVEFORMATEXTENSIBLE wf{};
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

        HRESULT h1 = pc->SetDeviceFormat(id, &wf.Format, &wf.Format);
        printf("%ls: SetDeviceFormat=0x%08lX\n", nm.c_str(), (unsigned long)h1);
        CoTaskMemFree(id);
        d->Release();
    }
    pc->Release();
    col->Release(); en->Release();
    CoUninitialize();
    return 0;
}
