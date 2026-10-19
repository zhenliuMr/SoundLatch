#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <windows.h>
#include "dsp.h"

namespace sl {

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

class Ring {
public:
    void init(size_t capacityFrames) {
        cap_ = capacityFrames;
        buf_.assign(cap_ * 2, 0.f);
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
    }
    size_t available() const {
        size_t w = w_.load(std::memory_order_acquire);
        size_t r = r_.load(std::memory_order_acquire);
        return w - r;
    }
    size_t push(const float* src, size_t frames) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        size_t space = cap_ - (w - r);
        size_t n = frames < space ? frames : space;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = ((w + i) % cap_) * 2;
            buf_[idx] = src[i * 2];
            buf_[idx + 1] = src[i * 2 + 1];
        }
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    void pushSilence(size_t frames) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        size_t space = cap_ - (w - r);
        size_t n = frames < space ? frames : space;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = ((w + i) % cap_) * 2;
            buf_[idx] = buf_[idx + 1] = 0.f;
        }
        w_.store(w + n, std::memory_order_release);
    }
    size_t pop(float* dst, size_t frames) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t n = frames < avail ? frames : avail;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = ((r + i) % cap_) * 2;
            dst[i * 2] = buf_[idx];
            dst[i * 2 + 1] = buf_[idx + 1];
        }
        r_.store(r + n, std::memory_order_release);
        return n;
    }
    void skip(size_t frames) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t n = frames < avail ? frames : avail;
        r_.store(r + n, std::memory_order_release);
    }
    void reset() { w_.store(0); r_.store(0); }

private:
    size_t cap_ = 0;
    std::vector<float> buf_;
    std::atomic<size_t> w_{0}, r_{0};
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    bool Start(const std::wstring& captureId, const std::wstring& renderId,
               bool loopbackCapture, DspCore* dsp, std::wstring& err);
    void Stop();
    bool Running() const { return running_; }

    float LatencyMs() const { return latencyMs_.load(); }
    int   Underruns() const { return underruns_.load(); }
    int   Overruns() const { return overruns_.load(); }
    float RingMs() const { return ringMs_.load(); }
    float CapBufMs() const { return capBufMs_; }
    float RenBufMs() const { return renBufMs_; }
    int SkipA() const { return skipA_.load(); }
    int SkipB() const { return skipB_.load(); }
    size_t RingMax() const { return ringMax_.load(); }
    size_t PreFrames() const { return preframes_; }
    float DbgLevel() const { return dbgLevel_.load(); }
    float DbgRatio() const { return dbgRatio_.load(); }
    unsigned long long CapPkts() const { return capPkts_.load(); }
    unsigned long long CapFrames() const { return capFrames_.load(); }

    static std::vector<DeviceInfo> EnumRender();
    static std::vector<DeviceInfo> EnumCapture();
    static std::wstring DefaultRenderId();
    static std::wstring DefaultCaptureId();

private:
    static DWORD WINAPI CaptureThread(LPVOID p);
    static DWORD WINAPI RenderThread(LPVOID p);
    void CaptureLoop();
    void RenderLoop();

    std::wstring capId_, renId_;
    bool loopback_ = false;
    DspCore* dsp_ = nullptr;

    HANDLE hCapThread_ = nullptr, hRenThread_ = nullptr;
    HANDLE hCapStop_ = nullptr, hRenStop_ = nullptr;
    std::atomic<bool> running_{false};

    Ring ring_;

    std::atomic<float> latencyMs_{0.f};
    std::atomic<float> ringMs_{0.f};
    std::atomic<float> dbgLevel_{0.f}, dbgRatio_{1.f};
    std::atomic<int> underruns_{0}, overruns_{0}, skipA_{0}, skipB_{0};
    std::atomic<unsigned long long> capPkts_{0}, capFrames_{0}, capSilent_{0};
    std::atomic<size_t> ringMax_{0};

    float capBufMs_ = 0.f, renBufMs_ = 0.f;
    UINT32 capBufFrames_ = 0, renBufFrames_ = 0;
    size_t preframes_ = 0;

    // adaptive sample-rate converter (two clock domains: capture vs render)
    std::vector<float> inBuf_;
    size_t inLen_ = 0;
    double cur_ = 0.0;
    double ratio_ = 1.0;
    // 欠载淡出/淡入斜坡的位置。必须是成员而非回调内局部量：WASAPI 回调周期
    // 可能只有 2~3 ms（128~192 样本），短于 5 ms 斜坡长度；
    // 每回调重置会让淡出永远爬不完，恢复瞬间产生硬跳变（实测 0.79阶跃）。
    float duck_ = 1.0f;
    double integ_ = 0.0;
    double target_ = 0.0;
};

} // namespace sl
