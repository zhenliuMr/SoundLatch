// apo_attach.cpp - attach/detach the SoundLock APO to audio endpoints the supported way:
// device properties (PKEY_FX_*), which survive the AudioEndpointBuilder rebuild.
//   apo_attach.exe attach   [nameFilter]
//   apo_attach.exe detach
#include <windows.h>
#include <setupapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "setupapi.lib")

// MinGW headers miss this one; it is exported by setupapi.dll since Windows 8.
extern "C" BOOL WINAPI SetupDiSetDevicePropertyW(HDEVINFO DeviceInfoSet,
                                                 PSP_DEVINFO_DATA DeviceInfoData,
                                                 const PROPERTYKEY* PropertyKey,
                                                 const PROPVARIANT* PropertyBuffer,
                                                 DWORD Flags);

// {c166523c-fe0c-4a94-a586-f1a80cfbbf3e} = DEVCLASS_AUDIO_ENDPOINT
static const GUID kAudioEndpointClass =
    {0xc166523c, 0xfe0c, 0x4a94, {0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e}};

// {d04e05a6-594b-4fb6-a80d-01af5eed7d1d}
static const GUID kFxBase =
    {0xd04e05a6, 0x594b, 0x4fb6, {0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d}};

static PROPERTYKEY PkeyFx(int pid) {
    PROPERTYKEY k;
    k.fmtid = kFxBase;
    k.pid = pid;
    return k;
}

static std::wstring FriendlyName(HDEVINFO h, SP_DEVINFO_DATA* d) {
    wchar_t buf[512] = {0};
    DWORD sz = sizeof(buf), type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(h, d, SPDRP_FRIENDLYNAME, &type, (PBYTE)buf, sz, &sz))
        return buf;
    if (SetupDiGetDeviceRegistryPropertyW(h, d, SPDRP_DEVICEDESC, &type, (PBYTE)buf, sz, &sz))
        return buf;
    return L"(unknown)";
}

static bool SetClsid(HDEVINFO h, SP_DEVINFO_DATA* d, int pid, const wchar_t* value) {
    PROPERTYKEY key = PkeyFx(pid);
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)value;
    BOOL ok = SetupDiSetDevicePropertyW(h, d, &key, &pv, 0);
    if (!ok) printf("    set(pid=%d) failed, err=%lu\n", pid, GetLastError());
    return ok != FALSE;
}

int wmain(int argc, wchar_t** argv) {
    bool attach = !(argc > 1 && !_wcsicmp(argv[1], L"detach"));
    const wchar_t* filter = argc > 2 ? argv[2] : L"";
    const wchar_t* clsid = L"{A5C6D1E0-3B7F-4C21-9E44-9E1D2C0A7781}";

    HDEVINFO h = SetupDiGetClassDevsW(&kAudioEndpointClass, nullptr, nullptr, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) { printf("SetupDiGetClassDevs failed %lu\n", GetLastError()); return 1; }

    int count = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA d{};
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(h, i, &d)) break;
        std::wstring name = FriendlyName(h, &d);
        if (filter[0] && name.find(filter) == std::wstring::npos) continue;
        if (attach && name.find(L"CABLE") != std::wstring::npos) continue;
        if (attach && name.find(L"Broadcast") != std::wstring::npos) continue;

        // 1 = endpoint effect, 2 = stream effect, 3 = mode effect
        bool ok = SetClsid(h, &d, 1, attach ? clsid : L"")
               && SetClsid(h, &d, 2, attach ? clsid : L"")
               && SetClsid(h, &d, 3, attach ? clsid : L"");
        wprintf(L"%-40s %s\n", name.c_str(), ok ? (attach ? L"ATTACHED" : L"DETACHED") : L"FAILED");
        if (ok) count++;
    }
    SetupDiDestroyDeviceInfoList(h);
    printf("endpoints handled: %d\n", count);
    return count ? 0 : 2;
}
