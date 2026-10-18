// SoundLockApo.cpp - in-engine (APO) version of SoundLock.
// Runs inside audiodg.exe: no extra buffering, only the limiter look-ahead.
#include "apo_defs.h"
#include "../../src/dsp.h"
#include <cstring>
#include <cstdio>
#include <new>

using namespace sl;

static const wchar_t* kRegKey = L"SOFTWARE\\SoundLock";

// minimal view of IAudioMediaType (only GetAudioFormat is called)
struct IAudioMediaType : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAudioFormat(WAVEFORMATEX** ppFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* pbCompressed) = 0;
};

static void LoadParams(DspParams& p) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    auto rd = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD v = def, sz = sizeof(v), ty = REG_DWORD;
        if (RegQueryValueExW(k, name, nullptr, &ty, (LPBYTE)&v, &sz) != ERROR_SUCCESS) return def;
        return v;
    };
    p.aOn      = rd(L"AOn", 1) != 0;
    p.bOn      = rd(L"BOn", 1) != 0;
    p.envAmt   = (float)rd(L"EnvAmt", 55) / 100.0f;
    p.envThrDb = -60.0f + (float)rd(L"EnvThr", 18);
    p.stepAmt  = (float)rd(L"StepAmt", 65) / 100.0f;
    p.stepSens = (float)rd(L"StepSens", 70) / 100.0f;
    p.ceilDb   = -24.0f + (float)rd(L"Ceil", 210) / 10.0f;
    p.lookMs   = 1.5f;
    RegCloseKey(k);
}

class CSoundLockApo;

// IAudioProcessingObjectConfiguration gets its own vtable so the RT object keeps
// a single IUnknown chain (required for a correct COM object layout).
class CConfig : public IAudioProcessingObjectConfiguration {
public:
    explicit CConfig(CSoundLockApo* owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32, APO_CONNECTION_DESCRIPTOR**,
                                             UINT32, APO_CONNECTION_DESCRIPTOR**) override;
    HRESULT STDMETHODCALLTYPE UnlockForProcess() override;
private:
    CSoundLockApo* owner_;
};

class CSoundLockApo : public IAudioProcessingObjectRT {
public:
    CSoundLockApo() : cfg_(this) {}

    // ---------------------------------------------------------- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAudioProcessingObjectRT))
            *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        else if (IsEqualIID(riid, IID_IAudioProcessingObject))
            *ppv = static_cast<IAudioProcessingObject*>(this);
        else if (IsEqualIID(riid, IID_IAudioProcessingObjectConfiguration))
            *ppv = static_cast<IAudioProcessingObjectConfiguration*>(&cfg_);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    // ------------------------------------------- IAudioProcessingObject
    HRESULT STDMETHODCALLTYPE Initialize(APOInitBaseStruct*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME* pTime) override {
        if (!pTime) return E_POINTER;
        *pTime = (HNSTIME)15000;   // 1.5 ms look-ahead, in 100-ns units
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES** ppProps) override {
        if (!ppProps) return E_POINTER;
        auto* p = (APO_REG_PROPERTIES*)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
        if (!p) return E_OUTOFMEMORY;
        memset(p, 0, sizeof(*p));
        p->clsid = CLSID_SoundLockApo;
        p->Flags = (APO_FLAG)(APO_FLAG_INPLACE |
                              APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                              APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
                              APO_FLAG_SAMPLESPERFRAME_MUST_MATCH);
        wcsncpy(p->szFriendlyName, L"SoundLock APO", 255);
        wcsncpy(p->szCopyrightInfo, L"SoundLock", 255);
        p->u32MajorVersion = 2;
        p->u32MinorVersion = 0;
        p->u32MinInputConnections = 1;
        p->u32MaxInputConnections = 1;
        p->u32MinOutputConnections = 1;
        p->u32MaxOutputConnections = 1;
        p->u32MaxInstances = 1;
        p->u32NumAPOInterfaces = 0;
        *ppProps = p;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Uninitialize() override { return S_OK; }

    // ---------------------------------------- driven by CConfig
    HRESULT DoLock(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** ppIn) {
        if (nIn < 1 || !ppIn || !ppIn[0]) return E_INVALIDARG;
        WAVEFORMATEX* wf = nullptr;
        if (ppIn[0]->pFormat) ppIn[0]->pFormat->GetAudioFormat(&wf);

        ok_ = false;
        if (wf) {
            bool isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                           (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->wBitsPerSample == 32);
            if (isFloat && wf->nChannels == 2 && wf->nSamplesPerSec >= 8000) {
                dsp_.Prepare((float)wf->nSamplesPerSec);
                LoadParams(params_);
                dsp_.SetParams(params_);
                ok_ = true;
                fs_ = wf->nSamplesPerSec;
            }
            // NOTE: the format pointer is owned by the media type - never free it
        }
        maxFrames_ = ppIn[0]->u32MaxFrameCount;
        return S_OK;
    }
    HRESULT DoUnlock() {
        ok_ = false;
        dsp_.Reset();
        return S_OK;
    }

    // ---------------------------------------- IAudioProcessingObjectRT
    void STDMETHODCALLTYPE APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** ppIn,
                                      UINT32 nOut, APO_CONNECTION_PROPERTY** ppOut) override {
        if (nIn < 1 || nOut < 1 || !ppIn || !ppOut) return;
        APO_CONNECTION_PROPERTY* in = ppIn[0];
        APO_CONNECTION_PROPERTY* out = ppOut[0];
        if (!in || !out) return;
        if (out->BufferFlags == BUFFER_SILENT) return;

        float* src = (float*)in->pBuffer;
        float* dst = (float*)out->pBuffer;
        UINT32 frames = in->u32ValidFrameCount < out->u32ValidFrameCount
                            ? in->u32ValidFrameCount
                            : out->u32ValidFrameCount;
        if (!frames) return;

        const bool silentIn = (in->BufferFlags == BUFFER_SILENT);
        if (src != dst) {
            if (silentIn) memset(dst, 0, (size_t)frames * 8);
            else          memcpy(dst, src, (size_t)frames * 8);
        }
        if (!ok_ || silentIn || !dst) return;
        dsp_.Process(dst, (int)frames);
    }

    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 n) override { return n; }
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 n) override { return n; }

private:
    LONG ref_ = 1;
    bool ok_ = false;
    UINT32 maxFrames_ = 0;
    UINT32 fs_ = 48000;
    DspCore dsp_;
    DspParams params_{};
    CConfig cfg_;
};

HRESULT STDMETHODCALLTYPE CConfig::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        AddRef();
        return S_OK;
    }
    return owner_->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE CConfig::AddRef() { return owner_->AddRef(); }
ULONG STDMETHODCALLTYPE CConfig::Release() { return owner_->Release(); }
HRESULT STDMETHODCALLTYPE CConfig::LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** ppIn,
                                                  UINT32, APO_CONNECTION_DESCRIPTOR**) {
    return owner_->DoLock(nIn, ppIn);
}
HRESULT STDMETHODCALLTYPE CConfig::UnlockForProcess() { return owner_->DoUnlock(); }

// ------------------------------------------------------------------ factory
class CSoundLockApoFactory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* apo = new (std::nothrow) CSoundLockApo();
        if (!apo) return E_OUTOFMEMORY;
        HRESULT hr = apo->QueryInterface(riid, ppv);
        apo->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

private:
    LONG ref_ = 1;
};

// ------------------------------------------------------------------ exports
extern "C" {

__declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

__declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!IsEqualCLSID(rclsid, CLSID_SoundLockApo)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) CSoundLockApoFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

__declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
__declspec(dllexport) HRESULT __stdcall DllRegisterServer() { return S_OK; }
__declspec(dllexport) HRESULT __stdcall DllUnregisterServer() { return S_OK; }

} // extern "C"
