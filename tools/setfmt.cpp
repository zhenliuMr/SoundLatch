// setfmt.cpp - force a capture endpoint's device format to 48k float32 stereo
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <cstdio>
#include <string>

static const PROPERTYKEY kName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// PKEY_AudioEngine_DeviceFormat
static const PROPERTYKEY kFmt = {
    {0xf19f064d, 0x82c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

int main(int argc, char** argv) {
    const wchar_t* want = argc > 1 ? L"CABLE Output" : L"";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
    UINT32 n = 0; col->GetCount(&n);
    int changed = 0;
    for (UINT32 i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v; PropVariantInit(&v); ps->GetValue(kName, &v);
        std::wstring nm = (v.vt == VT_LPWSTR && v.pwszVal) ? v.pwszVal : L"";
        PropVariantClear(&v);
        if (nm.find(want) != std::wstring::npos) {
            IPropertyStore* wr = nullptr;
            HRESULT hr = d->OpenPropertyStore(STGM_READWRITE, &wr);
            if (FAILED(hr) || !wr) { printf("open rw failed 0x%08lX\n", (unsigned long)hr); ps->Release(); d->Release(); continue; }
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
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_BLOB;
            pv.blob.cbSize = sizeof(wf);
            pv.blob.pBlobData = (BYTE*)&wf;
            hr = wr->SetValue(kFmt, pv);
            if (SUCCEEDED(hr)) hr = wr->Commit();
            printf("%ls -> set format hr=0x%08lX\n", nm.c_str(), (unsigned long)hr);
            changed++;
            wr->Release();
        }
        ps->Release(); d->Release();
    }
    col->Release(); en->Release();
    CoUninitialize();
    printf("changed=%d\n", changed);
    return 0;
}
