// dsp.h - SoundLock DSP core (interleaved stereo float32)
// Realtime-safe: no allocation / locking / exceptions inside Process().
//
// Structure:
//   input -> 4-band Linkwitz-Riley 4th order split (phase-compensated with allpass)
//         -> per-band dynamic gain (downward expander for ambience,
//            transient-gated upward expander for footsteps)
//         -> sum -> lookahead brickwall limiter -> out
//
// Gains are computed at control rate (32 samples) and linearly ramped,
// so log/pow cost is amortised and CPU stays around 1%.
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstring>
#include <atomic>

namespace sl {

inline float dbToLin(float db) { return powf(10.0f, db * 0.05f); }
inline float linToDb(float v)  { return 20.0f * log10f(v + 1e-9f); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    void setLP(float fs, float f, float q) {
        float w0 = 6.28318530718f * f / fs, cw = cosf(w0), sw = sinf(w0);
        float alpha = sw / (2.0f * q), a0 = 1.0f + alpha;
        b0 = (1.0f - cw) * 0.5f / a0;
        b1 = (1.0f - cw) / a0;
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void setHP(float fs, float f, float q) {
        float w0 = 6.28318530718f * f / fs, cw = cosf(w0), sw = sinf(w0);
        float alpha = sw / (2.0f * q), a0 = 1.0f + alpha;
        b0 = (1.0f + cw) * 0.5f / a0;
        b1 = -(1.0f + cw) / a0;
        b2 = b0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    // 2nd order allpass: same magnitude/phase as an LR4 LP+HP pair
    void setAP(float fs, float f, float q) {
        float w0 = 6.28318530718f * f / fs, cw = cosf(w0), sw = sinf(w0);
        float alpha = sw / (2.0f * q), a0 = 1.0f + alpha;
        b0 = (1.0f - alpha) / a0;
        b1 = -2.0f * cw / a0;
        b2 = 1.0f;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    inline float proc(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct LR4 {
    Biquad s1, s2;
    void setLP(float fs, float f) { s1.setLP(fs, f, 0.70710678f); s2.setLP(fs, f, 0.70710678f); }
    void setHP(float fs, float f) { s1.setHP(fs, f, 0.70710678f); s2.setHP(fs, f, 0.70710678f); }
    void reset() { s1.reset(); s2.reset(); }
    inline float lp(float x) { return s2.proc(s1.proc(x)); }
    inline float hp(float x) { return s2.proc(s1.proc(x)); }
};

struct Env {
    float a = 0, r = 0, e = 0;
    void set(float fs, float attackMs, float releaseMs) {
        a = expf(-1.0f / (fs * attackMs  * 0.001f));
        r = expf(-1.0f / (fs * releaseMs * 0.001f));
    }
    void reset() { e = 0.f; }
    inline float proc(float x) {
        float v = fabsf(x);
        float c = (v > e) ? a : r;
        e = v + c * (e - v);
        return e;
    }
};

struct DspParams {
    bool  aOn      = true;
    bool  bOn      = true;
    float envAmt   = 0.55f;     // 0..1
    float envThrDb = -42.f;     // -60..-18
    float stepAmt  = 0.65f;     // 0..1
    float stepSens = 0.70f;     // 0..1
    float ceilDb   = -3.0f;     // -24..0
    float lookMs   = 1.5f;      // 0.5..5
    bool  bypass   = false;
};

// --------------------------------------------------------------- limiter
class Limiter {
public:
    static const int kMaxLook = 256;

    void prepare(float fs) {
        fs_ = fs;
        int look = (int)(fs * 0.0015f);
        look = (int)clampf((float)look, 8.0f, (float)kMaxLook);
        rebuild(look);
    }
    void configure(float ceilDb, float lookMs) {
        ceil_ = dbToLin(ceilDb);
        int n = (int)(fs_ * lookMs * 0.001f);
        n = (int)clampf((float)n, 8.0f, (float)kMaxLook);
        if (n != look_) rebuild(n);
    }
    void reset() {
        std::fill(audioL_.begin(), audioL_.end(), 0.f);
        std::fill(audioR_.begin(), audioR_.end(), 0.f);
        w_ = 0; qh_ = 0; qt_ = 0; pos_ = 0;
        gain_ = 1.0f;
        relCoef_ = 1.0f - expf(-1.0f / (fs_ * 0.100f));
    }

    void process(float* io, int frames) {
        for (int i = 0; i < frames; ++i) {
            float l = io[2 * i], r = io[2 * i + 1];
            if (!std::isfinite(l)) l = 0.f;
            if (!std::isfinite(r)) r = 0.f;
            float peak = std::max(fabsf(l), fabsf(r));

            float dl = audioL_[w_], dr = audioR_[w_];
            audioL_[w_] = l; audioR_[w_] = r;

            while (qh_ != qt_) {
                int b = (qt_ - 1 + cap_) % cap_;
                if (peaks_[b] <= peak) qt_ = b; else break;
            }
            peaks_[qt_] = peak; pidx_[qt_] = pos_;
            qt_ = (qt_ + 1) % cap_;
            int oldest = pos_ - look_ + 1;
            while (qh_ != qt_ && pidx_[qh_] < oldest) qh_ = (qh_ + 1) % cap_;
            float wmax = (qh_ != qt_) ? peaks_[qh_] : peak;

            float target = (wmax > ceil_) ? (ceil_ / wmax) : 1.0f;
            if (target < gain_) gain_ = target;
            else                gain_ += (target - gain_) * relCoef_;

            io[2 * i]     = clampf(dl * gain_, -ceil_, ceil_);
            io[2 * i + 1] = clampf(dr * gain_, -ceil_, ceil_);

            w_ = (w_ + 1) % cap_;
            pos_++;
        }
    }

    int LookFrames() const { return look_; }

private:
    void rebuild(int look) {
        look_ = look; cap_ = look_ + 2;
        audioL_.assign(cap_, 0.f);
        audioR_.assign(cap_, 0.f);
        peaks_.assign(cap_, 0.f);
        pidx_.assign(cap_, 0);
        reset();
    }

    float fs_ = 48000.f;
    int   look_ = 72, cap_ = 74;
    float ceil_ = 0.707f, gain_ = 1.f, relCoef_ = 0.01f;
    int   w_ = 0, qh_ = 0, qt_ = 0;
    long  pos_ = 0;
    std::vector<float> audioL_, audioR_, peaks_;
    std::vector<int>   pidx_;
};

// --------------------------------------------------------------- core
class DspCore {
public:
    static constexpr int kCB = 32;   // control block

    void Prepare(float fs) {
        fs_ = fs;
        for (int ch = 0; ch < 2; ++ch) {
            lp150_[ch].setLP(fs, 150.f);  hp150_[ch].setHP(fs, 150.f);
            lp600_[ch].setLP(fs, 600.f);  hp600_[ch].setHP(fs, 600.f);
            lp3k_[ch].setLP(fs, 3000.f);  hp3k_[ch].setHP(fs, 3000.f);
            ap600_[ch].setAP(fs, 600.f, 0.70710678f);
            ap3k_[ch].setAP(fs, 3000.f, 0.70710678f);
            ap3kb_[ch].setAP(fs, 3000.f, 0.70710678f);
        }
        for (int b = 0; b < 4; ++b) { envB_[b].set(fs, 5.f, 120.f); gPrev_[b] = 1.f; gCur_[b] = 1.f; }
        envFast_.set(fs, 3.f, 3.f);
        envSlow_.set(fs, 150.f, 150.f);
        limiter_.prepare(fs);
        Reset();
    }

    void Reset() {
        for (int ch = 0; ch < 2; ++ch) {
            lp150_[ch].reset(); hp150_[ch].reset();
            lp600_[ch].reset(); hp600_[ch].reset();
            lp3k_[ch].reset();  hp3k_[ch].reset();
            ap600_[ch].reset(); ap3k_[ch].reset(); ap3kb_[ch].reset();
        }
        for (int b = 0; b < 4; ++b) { envB_[b].reset(); gPrev_[b] = gCur_[b] = 1.f; }
        envFast_.reset(); envSlow_.reset();
        limiter_.reset();
        trSm_ = 0.f;
        peakOut_ = 0.f; rmsAcc_ = 0.f; rmsN_ = 0;
        meterPeak_.store(0.f);
    }

    void SetParams(const DspParams& p) {
        params_ = p;
        limiter_.configure(p.ceilDb, p.lookMs);
    }
    const DspParams& Params() const { return params_; }

    float TakePeak() { float v = peakOut_; peakOut_ = 0.f; return v; }
    float TakeRms()  { float v = rmsN_ ? sqrtf(rmsAcc_ / (float)rmsN_) : 0.f; rmsAcc_ = 0.f; rmsN_ = 0; return v; }

    float MeterPeak() {
        float v = meterPeak_.load(std::memory_order_relaxed);
        meterPeak_.store(0.f, std::memory_order_relaxed);
        return v;
    }

    void Process(float* io, int frames) {
        const DspParams p = params_;
        if (p.bypass) {
            if (p.bOn) limiter_.process(io, frames);
            track(io, frames);
            return;
        }

        float boostDb[4] = {0.f, 0.f, 0.f, 0.f};
        const float wgt[4] = {0.f, 0.25f, 1.0f, 0.5f};
        for (int b = 1; b < 4; ++b) boostDb[b] = p.stepAmt * 12.0f * wgt[b];
        const float sensDb  = 6.0f - 6.0f * p.stepSens;
        const float cutBase = -18.0f * p.envAmt;
        const float cutMax  = -12.0f * p.envAmt;

        int base = 0;
        while (base < frames) {
            int n = std::min(kCB, frames - base);
            float bl[4][kCB], br[4][kCB];

            for (int i = 0; i < n; ++i) {
                float l = io[2 * (base + i)], r = io[2 * (base + i) + 1];
                if (!std::isfinite(l)) l = 0.f;
                if (!std::isfinite(r)) r = 0.f;

                float h1l = hp150_[0].hp(l), h1r = hp150_[1].hp(r);
                float a0l = lp150_[0].lp(l), a0r = lp150_[1].lp(r);
                float h2l = hp600_[0].hp(h1l), h2r = hp600_[1].hp(h1r);
                float b1l = lp600_[0].lp(h1l), b1r = lp600_[1].lp(h1r);

                // phase compensation so the 4 bands sum flat
                a0l = ap3k_[0].proc(ap600_[0].proc(a0l));
                a0r = ap3k_[1].proc(ap600_[1].proc(a0r));
                b1l = ap3kb_[0].proc(b1l);
                b1r = ap3kb_[1].proc(b1r);

                bl[0][i] = a0l; br[0][i] = a0r;
                bl[1][i] = b1l; br[1][i] = b1r;
                bl[2][i] = lp3k_[0].lp(h2l); br[2][i] = lp3k_[1].lp(h2r);
                bl[3][i] = hp3k_[0].hp(h2l); br[3][i] = hp3k_[1].hp(h2r);

                float m = 0.5f * (l + r);
                envFast_.proc(m);
                envSlow_.proc(m);
                for (int b = 0; b < 4; ++b) envB_[b].proc(0.5f * (bl[b][i] + br[b][i]));
            }

            // ---- control-rate gain update
            float tdb = linToDb(envFast_.e) - linToDb(envSlow_.e);
            trSm_ = clampf((tdb - sensDb) / 12.0f, 0.f, 1.f);

            for (int b = 0; b < 4; ++b) {
                float edb = linToDb(envB_[b].e);
                float gdb = 0.f;
                if (p.aOn) {
                    float cut = (edb < p.envThrDb) ? (edb - p.envThrDb) * 0.5f : 0.f;
                    if (b == 0) {
                        gdb = clampf(cutBase + clampf(cut, cutMax, 0.f), -30.f, 0.f);
                    } else {
                        gdb = clampf(cut, cutMax * 0.6f, 0.f);
                        float guard = clampf((-6.0f - edb) / 12.0f + 1.f, 0.f, 1.f);
                        gdb += boostDb[b] * trSm_ * guard;
                    }
                }
                gPrev_[b] = gCur_[b];
                gCur_[b] = dbToLin(gdb);
            }

            // ---- apply with per-sample ramp
            float inv = 1.0f / (float)n;
            for (int i = 0; i < n; ++i) {
                float oL = 0.f, oR = 0.f;
                float t = (float)(i + 1) * inv;
                for (int b = 0; b < 4; ++b) {
                    float g = gPrev_[b] + (gCur_[b] - gPrev_[b]) * t;
                    oL += bl[b][i] * g;
                    oR += br[b][i] * g;
                }
                io[2 * (base + i)]     = oL;
                io[2 * (base + i) + 1] = oR;
            }
            base += n;
        }

        if (p.bOn) limiter_.process(io, frames);
        track(io, frames);
    }

private:
    void track(const float* io, int frames) {
        float blk = 0.f;
        for (int i = 0; i < frames; ++i) {
            float a = fabsf(io[2 * i]), b = fabsf(io[2 * i + 1]);
            float pk = std::max(a, b);
            if (pk > blk) blk = pk;
            rmsAcc_ += io[2 * i] * io[2 * i] + io[2 * i + 1] * io[2 * i + 1];
        }
        if (blk > peakOut_) peakOut_ = blk;
        if (blk > meterPeak_.load(std::memory_order_relaxed))
            meterPeak_.store(blk, std::memory_order_relaxed);
        rmsN_ += frames * 2;
    }

    float fs_ = 48000.f;
    float trSm_ = 0.f;
    float gPrev_[4] = {1, 1, 1, 1}, gCur_[4] = {1, 1, 1, 1};
    float peakOut_ = 0.f, rmsAcc_ = 0.f;
    int   rmsN_ = 0;

    LR4 lp150_[2], hp150_[2], lp600_[2], hp600_[2], lp3k_[2], hp3k_[2];
    Biquad ap600_[2], ap3k_[2], ap3kb_[2];
    Env envB_[4];
    Env envFast_, envSlow_;
    Limiter limiter_;
    DspParams params_{};
    std::atomic<float> meterPeak_{0.f};
};

} // namespace sl
