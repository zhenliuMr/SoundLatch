// devfmt.cpp - dump mix format / periods of all endpoints
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <string>

static const PROPERTYKEY kName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static void dump(EDataFlow flow, const wchar_t* tag) {
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    UINT32 n = 0; col->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v; PropVariantInit(&v); ps->GetValue(kName, &v);
        std::wstring name = (v.vt == VT_LPWSTR && v.pwszVal) ? v.pwszVal : L"";
        PropVariantClear(&v); ps->Release();

        IAudioClient* cl = nullptr;
        d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl);
        WAVEFORMATEX* mix = nullptr;
        cl->GetMixFormat(&mix);
        REFERENCE_TIME def = 0, mn = 0;
        cl->GetDevicePeriod(&def, &mn);

        UINT32 fund = 0;
        IAudioClient3* c3 = nullptr;
        if (SUCCEEDED(cl->QueryInterface(__uuidof(IAudioClient3), (void**)&c3)) && c3) {
            UINT32 a = 0, b = 0, c = 0, dd = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &a, &b, &c, &dd))) fund = c;
            c3->Release();
        }
        wprintf(L"%s | %-45s | %u ch %u Hz %u bit tag=%u | period def=%.1fms min=%.1fms | ac3 min=%u frames (%.1fms)\n",
                tag, name.c_str(), mix->nChannels, mix->nSamplesPerSec, mix->wBitsPerSample,
                mix->wFormatTag, def / 10000.0, mn / 10000.0, fund, fund * 1000.0 / mix->nSamplesPerSec);
        CoTaskMemFree(mix);
        cl->Release(); d->Release();
    }
    col->Release(); en->Release();
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    dump(eRender, L"PLAY");
    dump(eCapture, L"CAPT");
    CoUninitialize();
    return 0;
}
