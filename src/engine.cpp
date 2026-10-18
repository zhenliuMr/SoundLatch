#include "engine.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <cstdio>
#include <xmmintrin.h>

namespace sl {

static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static const GUID kSubFloat = {0x00000003, 0x0000, 0x0010,
                               {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

static bool IsFloatFmt(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)wf;
        return IsEqualGUID(e->SubFormat, kSubFloat) != 0;
    }
    return false;
}

// any endpoint format -> float32 stereo (first two channels, mono duplicated)
static void ConvertToFloatStereo(const BYTE* src, UINT32 frames, const WAVEFORMATEX* wf,
                                 bool isFloat, int ch, float* dst) {
    int bytes = wf->wBitsPerSample / 8;
    int align = wf->nBlockAlign;
    int nch = ch < 2 ? 1 : 2;
    for (UINT32 i = 0; i < frames; ++i) {
        const BYTE* p = src + (size_t)i * align;
        float v0 = 0.f, v1 = 0.f;
        for (int c = 0; c < nch; ++c) {
            const BYTE* q = p + c * bytes;
            float v;
            if (isFloat) v = *(const float*)q;
            else if (bytes == 2) v = *(const int16_t*)q / 32768.0f;
            else if (bytes == 3) {
                int32_t s = (int32_t)(q[0] | (q[1] << 8) | ((int8_t)q[2] << 16));
                v = s / 8388608.0f;
            } else v = *(const int32_t*)q / 2147483648.0f;
            if (c == 0) v0 = v; else v1 = v;
        }
        if (nch == 1) v1 = v0;
        dst[2 * i] = v0;
        dst[2 * i + 1] = v1;
    }
}

// IEEE float stereo 48k in WAVEFORMATEXTENSIBLE form
static void FillFloatStereo(WAVEFORMATEXTENSIBLE& wf, int rate = 48000) {
    memset(&wf, 0, sizeof(wf));
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = (DWORD)rate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = (WORD)(wf.Format.nChannels * wf.Format.wBitsPerSample / 8);
    wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = 3;  // FL | FR
    wf.SubFormat = kSubFloat;
}

static HRESULT InitRenderClient(IAudioClient* client, const WAVEFORMATEX* wf, UINT32* bufFrames) {
    WAVEFORMATEX* closest = nullptr;
    HRESULT sup = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, wf, &closest);
    if (closest) CoTaskMemFree(closest);

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (sup != S_OK) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    IAudioClient3* c3 = nullptr;
    if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), (void**)&c3)) && c3) {
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        if (SUCCEEDED(c3->GetSharedModeEnginePeriod(wf, &def, &fund, &mn, &mx)) && fund > 0) {
            if (SUCCEEDED(c3->InitializeSharedAudioStream(flags, fund, wf, nullptr))) {
                c3->GetBufferSize(bufFrames);
                c3->Release();
                return S_OK;
            }
        }
        c3->Release();
    }
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, wf, nullptr);
    if (SUCCEEDED(hr)) client->GetBufferSize(bufFrames);
    return hr;
}

// ------------------------------------------------------------------ enum
static std::vector<DeviceInfo> EnumByFlow(EDataFlow flow) {
    std::vector<DeviceInfo> out;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pEnum))) || !pEnum) return out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT32 n = 0;
        col->GetCount(&n);
        for (UINT32 i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev) continue;
            LPWSTR id = nullptr;
            if (FAILED(dev->GetId(&id)) || !id) { dev->Release(); continue; }
            std::wstring name = L"(未命名设备)";
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            DeviceInfo di;
            di.id = id;
            di.name = name;
            out.push_back(di);
            CoTaskMemFree(id);
            dev->Release();
        }
        col->Release();
    }
    pEnum->Release();
    return out;
}

std::vector<DeviceInfo> AudioEngine::EnumRender()  { return EnumByFlow(eRender); }
std::vector<DeviceInfo> AudioEngine::EnumCapture() { return EnumByFlow(eCapture); }

static std::wstring DefaultId(EDataFlow flow) {
    std::wstring res;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&pEnum))) || !pEnum) return res;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(flow, eConsole, &dev)) && dev) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) { res = id; CoTaskMemFree(id); }
        dev->Release();
    }
    pEnum->Release();
    return res;
}

std::wstring AudioEngine::DefaultRenderId()  { return DefaultId(eRender); }
std::wstring AudioEngine::DefaultCaptureId() { return DefaultId(eCapture); }

// ------------------------------------------------------------------ engine
AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(const std::wstring& captureId, const std::wstring& renderId,
                        bool loopbackCapture, DspCore* dsp, std::wstring& err) {
    Stop();
    if (!dsp) { err = L"内部错误：DSP 未初始化"; return false; }
    if (renderId.empty()) { err = L"未选择输出设备"; return false; }
    if (captureId.empty() && !loopbackCapture) { err = L"未选择采集设备"; return false; }

    dsp_ = dsp;
    capId_ = captureId;
    renId_ = renderId;
    loopback_ = loopbackCapture;
    ring_.init(48000 * 2);
    underruns_.store(0);
    overruns_.store(0);

    running_ = true;
    hCapStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hRenStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    hRenThread_ = CreateThread(nullptr, 0, RenderThread, this, 0, nullptr);
    hCapThread_ = CreateThread(nullptr, 0, CaptureThread, this, 0, nullptr);
    if (!hRenThread_ || !hCapThread_) {
        err = L"创建音频线程失败";
        Stop();
        return false;
    }
    Sleep(300);
    if (!running_) { err = L"音频设备打开失败（设备可能被独占或格式不支持）"; return false; }
    return true;
}

void AudioEngine::Stop() {
    if (!running_ && !hCapThread_ && !hRenThread_) return;
    running_ = false;
    if (hCapStop_) SetEvent(hCapStop_);
    if (hRenStop_) SetEvent(hRenStop_);
    if (hCapThread_) { WaitForSingleObject(hCapThread_, 2000); CloseHandle(hCapThread_); hCapThread_ = nullptr; }
    if (hRenThread_) { WaitForSingleObject(hRenThread_, 2000); CloseHandle(hRenThread_); hRenThread_ = nullptr; }
    if (hCapStop_) { CloseHandle(hCapStop_); hCapStop_ = nullptr; }
    if (hRenStop_) { CloseHandle(hRenStop_); hRenStop_ = nullptr; }
    capBufMs_ = renBufMs_ = 0.f;
    latencyMs_.store(0.f);
    ringMs_.store(0.f);
}

DWORD WINAPI AudioEngine::CaptureThread(LPVOID p) {
    ((AudioEngine*)p)->CaptureLoop();
    return 0;
}
DWORD WINAPI AudioEngine::RenderThread(LPVOID p) {
    ((AudioEngine*)p)->RenderLoop();
    return 0;
}

void AudioEngine::CaptureLoop() {
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ + DAZ
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    HANDLE evt = nullptr;
    WAVEFORMATEX* mix = nullptr;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&pEnum)))) break;
        std::wstring id = loopback_ ? renId_ : capId_;
        if (FAILED(pEnum->GetDevice(id.c_str(), &dev)) || !dev) break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) break;

        // Capture in the endpoint's OWN mix format: forcing 48k on a 44.1k endpoint
        // makes some (virtual) drivers deliver frames at the wrong rate.
        if (FAILED(client->GetMixFormat(&mix)) || !mix) break;
        bool isFloat = IsFloatFmt(mix);
        int ch = mix->nChannels;
        int srcRate = (int)mix->nSamplesPerSec;

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        UINT32 bufFrames = 0;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, mix, nullptr))) break;
        client->GetBufferSize(&bufFrames);

        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) break;
        if (FAILED(client->SetEventHandle(evt))) break;

        capBufMs_ = bufFrames * 1000.0f / (float)srcRate;
        capBufFrames_ = (UINT32)(capBufMs_ * 48.0f + 0.5f);
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) break;
        if (FAILED(client->Start())) break;

        double t = 0.0, ratio = (double)srcRate / 48000.0;   // emit 1/ratio samples per input
        float lastL = 0.f, lastR = 0.f;
        std::vector<float> tmp, out48;
        out48.resize(16384);

        while (running_) {
            DWORD w = WaitForSingleObject(evt, 500);
            if (w != WAIT_OBJECT_0) continue;
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* data = nullptr;
                UINT32 n = 0;
                DWORD st = 0;
                if (FAILED(cap->GetBuffer(&data, &n, &st, nullptr, nullptr))) break;
                if (n) {
                    capPkts_.fetch_add(1);
                    capFrames_.fetch_add(n);
                    if (st & AUDCLNT_BUFFERFLAGS_SILENT) {
                        capSilent_.fetch_add(1);
                        ring_.pushSilence((size_t)((double)n / ratio));
                    } else {
                        tmp.resize((size_t)n * 2);
                        ConvertToFloatStereo(data, n, mix, isFloat, ch, tmp.data());
                        if (srcRate == 48000) {
                            ring_.push(tmp.data(), n);
                        } else {
                            size_t o = 0;
                            for (UINT32 i = 0; i < n; ++i) {
                                float l = tmp[2 * i], r = tmp[2 * i + 1];
                                while (t < 1.0) {
                                    if ((o + 1) * 2 + 2 > out48.size()) out48.resize(out48.size() * 2);
                                    out48[o * 2]     = lastL + (l - lastL) * (float)t;
                                    out48[o * 2 + 1] = lastR + (r - lastR) * (float)t;
                                    ++o;
                                    t += ratio;
                                }
                                t -= 1.0;
                                lastL = l; lastR = r;
                            }
                            if (o) ring_.push(out48.data(), o);
                        }
                    }
                }
                cap->ReleaseBuffer(n);
            }
        }
    } while (false);

    if (client) client->Stop();
    if (cap) cap->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (pEnum) pEnum->Release();
    if (evt) CloseHandle(evt);
    if (mix) CoTaskMemFree(mix);
    running_ = false;
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}

void AudioEngine::RenderLoop() {
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ + DAZ
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL);

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* ren = nullptr;
    HANDLE evt = nullptr;
    WAVEFORMATEXTENSIBLE wf;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&pEnum)))) break;
        if (FAILED(pEnum->GetDevice(renId_.c_str(), &dev)) || !dev) break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) break;

        FillFloatStereo(wf);
        UINT32 bufFrames = 0;
        if (FAILED(InitRenderClient(client, &wf.Format, &bufFrames))) break;

        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) break;
        if (FAILED(client->SetEventHandle(evt))) break;

        renBufFrames_ = bufFrames;
        renBufMs_ = bufFrames * 1000.0f / 48000.0f;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&ren))) break;

        inBuf_.assign((size_t)16384 * 2, 0.f);
        inLen_ = 0;
        cur_ = 0.0;
        ratio_ = 1.0;
        integ_ = 0.0;

        // prefill: one capture period + 25 % so the two thread phases cannot collide
        {
            UINT32 target = capBufFrames_ ? capBufFrames_ : bufFrames;
            target += target / 4 + 64;
            preframes_ = target;
            target_ = (double)target;
            for (int i = 0; i < 200 && running_; ++i) {
                if (ring_.available() >= target) break;
                Sleep(5);
            }
        }
        if (FAILED(client->Start())) break;

        while (running_) {
            DWORD w = WaitForSingleObject(evt, 500);
            if (w != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) break;
            UINT32 frames = bufFrames - pad;
            if (!frames) continue;

            BYTE* p = nullptr;
            if (FAILED(ren->GetBuffer(frames, &p))) break;
            float* f = (float*)p;

            // ---- async sample-rate conversion (input domain -> 48k output)
            // compaction keeps only the fractional cursor, so phase stays continuous
            {
                size_t n = (size_t)cur_;
                if (n > 0) {
                    memmove(inBuf_.data(), inBuf_.data() + n * 2, (inLen_ - n) * 2 * sizeof(float));
                    inLen_ -= n;
                    cur_ -= (double)n;
                }
            }
            size_t capFrames = inBuf_.size() / 2;
            // fill the staging buffer aggressively; it is the reservoir the ASRC
            // servos on, not the thin ring between the two threads
            size_t want = capFrames - 2 - inLen_;
            size_t got = ring_.pop(inBuf_.data() + inLen_ * 2, want);
            inLen_ += got;

            double level = (double)ring_.available() + (double)inLen_;
            double err = (target_ > 0.0) ? (level - target_) / target_ : 0.0;
            if (fabs(err) < 0.35) integ_ = clampd(integ_ + err * 0.02, -3.0, 3.0);
            double wantRatio = 1.0 + clampd(err * 0.45 + integ_ * 0.03, -0.10, 0.10);
            ratio_ += (wantRatio - ratio_) * 0.10;
            ratio_ = clampd(ratio_, 0.85, 1.15);

            bool starve = false;
            for (UINT32 k = 0; k < frames; ++k) {
                size_t i0 = (size_t)cur_;
                double fr = cur_ - (double)i0;
                if (i0 + 1 >= inLen_) {
                    f[k * 2]     = inLen_ ? inBuf_[(inLen_ - 1) * 2] : 0.f;
                    f[k * 2 + 1] = inLen_ ? inBuf_[(inLen_ - 1) * 2 + 1] : 0.f;
                    starve = true;
                    continue;
                }
                float aL = inBuf_[i0 * 2], aR = inBuf_[i0 * 2 + 1];
                float bL = inBuf_[(i0 + 1) * 2], bR = inBuf_[(i0 + 1) * 2 + 1];
                f[k * 2]     = (float)(aL + (bL - aL) * fr);
                f[k * 2 + 1] = (float)(aR + (bR - aR) * fr);
                cur_ += ratio_;
            }
            if (starve) {
                underruns_.fetch_add(1);
                ratio_ = clampd(ratio_ * 0.997, 0.85, 1.15);   // emergency: pull back, avoid repeat
            }

            dsp_->Process(f, (int)frames);
            ren->ReleaseBuffer(frames, 0);

            float ringMs = (float)(((double)ring_.available() + (double)inLen_ - cur_) * 1000.0 / 48000.0);
            ringMs_.store(ringMs);
            dbgLevel_.store((float)((double)inLen_ - cur_));
            dbgRatio_.store((float)ratio_);
            float look = dsp_->Params().bOn ? dsp_->Params().lookMs : 0.f;
            latencyMs_.store(capBufMs_ + ringMs + renBufMs_ + look);
        }
    } while (false);

    if (client) client->Stop();
    if (ren) ren->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (pEnum) pEnum->Release();
    if (evt) CloseHandle(evt);
    running_ = false;
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}

} // namespace sl
