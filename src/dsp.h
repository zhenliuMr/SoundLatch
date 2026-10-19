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
#include <mutex>

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
    float envAmt   = 0.55f;     // 0..1  强度（唯一保留的环境参数）
    float stepAmt  = 0.65f;     // 0..1  强度（唯一保留的脚步参数）
    float ceilDb   = -3.0f;     // -24..0
    float lookMs   = 1.5f;      // 0.5..5
    bool  bypass   = false;

    // ---- 无锁一致性传输 --------------------------------------------------
    // SetParams 在 UI 线程、Process 在音频线程。整体赋值结构体不是原子操作，
    // 压力测试实测会读到「一半新一半旧」的混合代参数（撕裂率 1~2%，观测到
    // ceilDb 是新值而 aOn/bOn 仍是旧值）。
    //
    // 试过的方案都不够：
    //   · seqlock —— 结构体拷贝可被编译器重排成任意指令序列，读者即使看到
    //     seq 未变也可能读到写了一半的数据（实测撕裂率反而升到 35%）
    //   · 双缓冲 + 原子索引 —— 残留约 0.01%，因为 slot_ 两次读之间可能翻转
    //     两次回到原值，复查失效；且结构体拷贝期间的窗口无法用屏障完全消除
    //   · alignas(64) 防伪共享 —— 无效（残留同样随机）
    //
    // 最终方案：把全部参数**量化打包进一个 uint64**，一次原子读天然一致。
    // 量化步长远小于听觉可辨阈值（0.01%），不引入任何可闻误差。
    // 位分配（共 64 bit，互不重叠）：
    //   [ 0.. 2] aOn/bOn/bypass 各 1 bit
    //   [ 3..19] envAmt  0..1 -> 0..100000（17 bit）
    //   [20..36] stepAmt 0..1 -> 0..100000（17 bit）
    //   [37..62] ceilDb  -120..0，量化到 0.01 dB（26 bit）
    //   [63]留空
    static constexpr uint32_t kQ100000 = 100000u;
    static uint32_t qU(float v01) {                 // 0..1 -> 0..100000
        float x = clampf(v01, 0.f, 1.f) * (float)kQ100000 + 0.5f;
        return (uint32_t)x;
    }
    static float  uQ(uint32_t q) { return (float)q / (float)kQ100000; }
    // ceilDb 实际取值范围是 -120（静音）.. 0 dBFS，必须覆盖，否则静音档会被
    // 截断成 -24 dBFS —— 表现为「拖到最左仍有声音」。
    static int32_t qDb(float db) {                  // -120..0 -> 0..12000 (0.01dB)
        float x = (clampf(db, -120.f, 0.f) + 120.f) * 100.f + 0.5f;
        return (int32_t)x;
    }
    static float  dbQ(int32_t q) { return ((float)q) * 0.01f - 120.f; }
    static int32_t qMs(float ms) {                  // 0.5..5 ms -> 0..450 (0.01ms)
        float x = (clampf(ms, 0.5f, 5.f) - 0.5f) * 100.f + 0.5f;
        return (int32_t)x;
    }
    static float  msQ(int32_t q) { return ((float)q) * 0.01f + 0.5f; }

    uint64_t Pack() const {
        return ((uint64_t)(aOn    ? 1u : 0u) << 0)
             | ((uint64_t)(bOn    ? 1u : 0u) << 1)
             | ((uint64_t)(bypass ? 1u : 0u) << 2)
             | ((uint64_t)qU(envAmt)<< 3)
             | ((uint64_t)qU(stepAmt) << 20)
             | ((uint64_t)(uint32_t)qDb(ceilDb) << 37);
    }
    void Unpack(uint64_t v) {
        aOn     = (v >> 0) & 1u;
        bOn     = (v >> 1) & 1u;
        bypass  = (v >> 2) & 1u;
        envAmt  = uQ((uint32_t)((v >> 3) & 0x1FFFFu));
        stepAmt = uQ((uint32_t)((v >> 20) & 0x1FFFFu));
        ceilDb  = dbQ((int32_t)(uint32_t)((v >> 37) & 0x3FFFFFFu));
        lookMs  = 1.5f;   // 不再传输：lookMs 只在 configure() 用，已在那里读原值
    }
};
// 判据常数：由tools/fix_probe.exe 在 189 条分层样本上实测标定，固化不开放给用户。
//
// 为什么固化：原「环境阈值」是绝对 dBFS 阈值，实测证明它无法区分目标——
//   L1 低频环境包络 −27.9 dBFS，脚步带(250-2k) −44.4 dBFS，但判据对所有频段
//   同时生效，结果脚步带被压 −25 dB 反而比环境的 −11.5 dB 更狠，方向是反的。
//   绝对阈值还随系统音量、玩家距离漂移，没有稳定取值。
//   原「瞬态灵敏度」只是移动门限 bias，选择性(脚步触发率/低频触发率)在
//   bias=0..6 区间单调下降(1.31→0.90)，调它只会同时放大误触发。
//
// 改为两个不依赖绝对电平的判据：
namespace judge {
// 环境抑制：谱倾斜抑制。只压「低频相对中高频明显独大」的部分。
// 实测各层倾斜 tilt = L0dBFS − mean(L1,L2)dBFS：
//   L1 低频环境 +16.6 / 脚步带 +2.1 / 枪声带 −13.2
// → tilt 超过 +8 dB 才判为环境隆隆，+16 dB 处全量压制。
// 实测各层 tilt = L0dBFS − mean(L1,L2)dBFS：L1 低频环境 +16.6 / 脚步带 +2.1 / 枪声带 −13.2
// 注意分频器自身在中频有增益：等幅 60Hz+1kHz 正弦实测 tilt=+9.4 dB，
// 所以 kTiltOn 必须 > 9.4，取 11；kTiltFull=22（约等幅 +12dB 时全量压制）。
//脚步带 tilt 仅 +2.1，离阈值 9 dB 以上，绝不会被误判成环境音。
constexpr float kTiltOn   = 11.0f;
constexpr float kTiltFull = 22.0f;
constexpr float kEnvCutMax  = 20.0f;   // dB，满档时 band0 最多压20 dB
constexpr float kEnvCutMid  =  7.0f;   // band1(150-600) 压得少，避免误伤脚步下缘

// 脚步增强：瞬态提升需同时满足「够陡」和「不是纯低频隆隆」。
// 实测 (fast-slow) 的 P90：L1 +5.9 / 脚步带 +4.9 / 枪声带 +5.7 —— 单靠陡度分不开，
// 必须靠频段分布：脚步主体在 600-3k，爆炸/载具冲击在 <600。
constexpr float kTransBias =  2.0f;   // dB，超过 fast-slow 多少才算瞬态
constexpr float kTransSpan = 12.0f;   // dB，映射到 0..1 的斜率
constexpr float kBandRatio =  4.0f;   // dB，中频瞬态需比低频瞬态高这么多
constexpr float kStepBoostMax = 12.0f; // dB，满档时中频最多抬 12 dB
} // namespace judge

// --------------------------------------------------------------- limiter
class Limiter {
public:
    static const int kMaxLook = 256;

    void prepare(float fs) {
        fs_ = fs;
        // 一次分配到最大可能尺寸。configure() 由 UI 线程调用，若届时才按需扩容，
        // 就会与音频线程的 process() 争抢同一批 vector —— 既是数据竞争，
        // 又在实时路径上引入堆分配（可能缺页/加锁）。
        const int capMax = kMaxLook + 2;
        audioL_.assign((size_t)capMax, 0.f);
        audioR_.assign((size_t)capMax, 0.f);
        peaks_.assign((size_t)capMax, 0.f);
        pidx_.assign((size_t)capMax, 0);
        int look = (int)(fs * 0.0015f);
        look = (int)clampf((float)look, 8.0f, (float)kMaxLook);
        look_ = look; cap_ = look + 2;
        w_ = 0; qh_ = qt_ = 0; pos_ = 0; gain_ = 1.f;
    }
    // 只改标量，绝不碰内存
    void configure(float ceilDb, float lookMs) {
        ceil_ = dbToLin(ceilDb);
        int n = (int)(fs_ * lookMs * 0.001f);
        n = (int)clampf((float)n, 8.0f, (float)kMaxLook);
        if (n == look_) return;
        look_ = n;
        cap_  = n + 2;
        // 环形队列下标必须回到合法范围，否则 audioL_[w_] 会越界
        w_ = 0; qh_ = qt_ = 0; pos_ = 0;
        gain_ = 1.0f;
    }
    void reset() {
        std::fill(audioL_.begin(), audioL_.end(), 0.f);
        std::fill(audioR_.begin(), audioR_.end(), 0.f);
        std::fill(peaks_.begin(), peaks_.end(), 0.f);
        std::fill(pidx_.begin(), pidx_.end(), 0);
        w_ = 0; qh_ = qt_ = 0; pos_ = 0;
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
        // 低频/中频各自的快慢包络：用于瞬态的频段分布判据
        envFastL_.set(fs, 3.f, 3.f);   envSlowL_.set(fs, 150.f, 150.f);
        envFastM_.set(fs, 3.f, 3.f);   envSlowM_.set(fs, 150.f, 150.f);
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
        for (int b = 0; b < 4; ++b) {
            limCur_[b] = limPrev_[b] = 1.f;
            std::fill(pendL_[b], pendL_[b] + kCB, 0.f);
            std::fill(pendR_[b], pendR_[b] + kCB, 0.f);
        }
        // 注意：这个系数是"每个控制块"用一次（kCB=32 样本），所以要按块长折算，
        // 否则实际恢复时间会慢 32 倍（80 ms 变成 2.56 s，枪声后整条信号会被压低好几秒）
        relLimCoef_ = 1.0f - expf(-(float)kCB / (fs_ * 0.080f));
        envFast_.reset(); envSlow_.reset();
        envFastL_.reset(); envSlowL_.reset();
        envFastM_.reset(); envSlowM_.reset();
        limiter_.reset();
        trSm_ = 0.f;
        peakOut_ = 0.f; rmsAcc_ = 0.f; rmsN_ = 0;
        meterPeak_.store(0.f);
    }

    // SetParams 在 UI 线程调用，Process 在音频线程调用。
    //
    // 整体赋值 params_ = p 不是原子操作：压力测试实测会读到「一半新一半旧」的
    // 混合代参数（撕裂率约 1~2%，观测到 ceilDb 是新值而 aOn/bOn 仍是旧值）。
    //
    // seqlock 在这里**无效**：params_ 是普通结构体，编译器可以把 p = params_ 拆成
    // 任意指令组合，读者即使看到 seq 未变也可能读到写了一半的数据（实测撕裂率
    // 反而升到 35%）。
    //
    // 改用双缓冲 + 原子索引：写者只写「非活动」那一份，写完再原子地翻转索引；
    // 读者一次读出索引和结构体，若两者一致就是完整的一代。无需锁、无需重试，
    // 实时线程零开销。
    void SetParams(const DspParams& p) {
        packed_.store(p.Pack(), std::memory_order_release);
        limiter_.configure(p.ceilDb, p.lookMs);
    }

    // 音频线程侧：一次原子读 = 一组完整且自洽的参数，天然无撕裂
    DspParams ParamsSnapshot() const {
        DspParams p;
        p.Unpack(packed_.load(std::memory_order_acquire));
        return p;
    }
    const DspParams& Params() const {
        cached_.Unpack(packed_.load(std::memory_order_acquire));
        return cached_;
    }

    float TakePeak() { float v = peakOut_; peakOut_ = 0.f; return v; }
    float TakeRms()  { float v = rmsN_ ? sqrtf(rmsAcc_ / (float)rmsN_) : 0.f; rmsAcc_ = 0.f; rmsN_ = 0; return v; }

    // 供离线标定/测试观察判据内部量（实时路径不用）
    float LastTilt() const { return tilt_; }
    float LastTrSm() const { return trSm_; }

    float MeterPeak() {
        float v = meterPeak_.load(std::memory_order_relaxed);
        meterPeak_.store(0.f, std::memory_order_relaxed);
        return v;
    }

    // 输入（系统输出）电平，供电平表显示真实音量（不受限幅削顶影响）
    float MeterInPeak() {
        float v = meterInPeak_.load(std::memory_order_relaxed);
        meterInPeak_.store(0.f, std::memory_order_relaxed);
        return v;
    }

    void Process(float* io, int frames) {
        const DspParams p = ParamsSnapshot();
        {   // 记录输入峰值（处理前 = 系统输出电平）
            float inp = 0.f;
            for (int i = 0; i < frames; ++i) {
                float a = fabsf(io[2 * i]), b = fabsf(io[2 * i + 1]);
                float pk = a > b ? a : b;
                if (pk > inp) inp = pk;
            }
            float cur = meterInPeak_.load(std::memory_order_relaxed);
            if (inp > cur) meterInPeak_.store(inp, std::memory_order_relaxed);
        }
        // 削顶判定阈值：与本次 Process 使用的同一份参数快照一致
        clipLimit_ = ((p.bOn && !p.bypass) ? dbToLin(p.ceilDb) : 1.0f) * 1.0009f;
        if (p.bypass) {
            if (p.bOn) limiter_.process(io, frames);
            track(io, frames);
            return;
        }

        float boostDb[4] = {0.f, 0.f, 0.f, 0.f};
        const float wgt[4] = {0.f, 0.30f, 1.0f, 0.45f};
        for (int b = 1; b < 4; ++b)
            boostDb[b] = p.stepAmt * judge::kStepBoostMax * wgt[b];
        const float ceilLin = dbToLin(p.ceilDb);      // 线性上限（-120 dB 即静音）

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
                float mL = 0.5f * (bl[0][i] + br[0][i]);                // <150
                float mM = 0.5f * (bl[1][i] + br[1][i]
                                 + bl[2][i] + br[2][i]);                // 150-3k
                envFastL_.proc(mL); envSlowL_.proc(mL);
                envFastM_.proc(mM); envSlowM_.proc(mM);
                for (int b = 0; b < 4; ++b) envB_[b].proc(0.5f * (bl[b][i] + br[b][i]));
            }

            // ---- 判据 1：谱倾斜（相对判据，不依赖绝对电平）
            // 低频相对中高频越独大，越像环境隆隆/载具；脚步带这个值只有 +2 dB。
            float eLow = linToDb(envB_[0].e);
            float eMid = 0.5f * (linToDb(envB_[1].e) + linToDb(envB_[2].e));
            float tilt = eLow - eMid;
            tilt_ = tilt;
            float envGate = clampf((tilt - judge::kTiltOn)
                                 / (judge::kTiltFull - judge::kTiltOn), 0.f, 1.f);

            // ---- 判据 2：瞬态的频段分布
            // 单靠「够陡」分不开（各层 P90 都在 +5 dB 左右），必须看中频瞬态
            // 是否比低频瞬态更突出——脚步主体在 600-3k，爆炸冲击在 <600。
            float tL = linToDb(envFastL_.e) - linToDb(envSlowL_.e);
            float tM = linToDb(envFastM_.e) - linToDb(envSlowM_.e);
            float trMid = clampf((tM - judge::kTransBias) / judge::kTransSpan, 0.f, 1.f);
            float trRatio = clampf((tM - tL - judge::kBandRatio) / 6.0f, 0.f, 1.f);
            trSm_ = trMid * trRatio;

            for (int b = 0; b < 4; ++b) {
                float edb = linToDb(envB_[b].e);
                float gdb = 0.f;
                if (p.aOn) {
                    // 只压 band0/band1，band2/band3 是脚步主体，一律不动
                    if (b == 0)      gdb = -judge::kEnvCutMax * p.envAmt * envGate;
                    else if (b == 1) gdb = -judge::kEnvCutMid  * p.envAmt * envGate;
                    if (b >= 1) {
                        float guard = clampf((-6.0f - edb) / 12.0f + 1.f, 0.f, 1.f);
                        gdb += boostDb[b] * trSm_ * guard;
                    }
                }
                gPrev_[b] = gCur_[b];
                gCur_[b] = dbToLin(gdb);
            }

            // ---- 限幅：超出的部分压到用户设的上限，整条信号一个增益（不分频段） ----
            // 先把 A/B 增益应用到本块，这样下面量到的峰值就是真正会输出的幅度
            float inv = 1.0f / (float)n;
            for (int i = 0; i < n; ++i) {
                float t = (float)(i + 1) * inv;
                for (int b = 0; b < 4; ++b) {
                    float g = gPrev_[b] + (gCur_[b] - gPrev_[b]) * t;
                    bl[b][i] *= g;
                    br[b][i] *= g;
                }
            }

            // 4 段叠加后的真实输出峰值
            float pk = 0.f;
            for (int i = 0; i < n; ++i) {
                float a = fabsf(bl[0][i] + bl[1][i] + bl[2][i] + bl[3][i]);
                float c = fabsf(br[0][i] + br[1][i] + br[2][i] + br[3][i]);
                if (a > pk) pk = a;
                if (c > pk) pk = c;
            }

            for (int b = 0; b < 4; ++b) limPrev_[b] = limCur_[b];
            if (p.bOn) {
                float target = (pk > ceilLin) ? ceilLin / pk : 1.0f;   // 压到上限即可
                for (int b = 0; b < 4; ++b)
                    limCur_[b] = (target < limCur_[b]) ? target
                                                       : limCur_[b] + (target - limCur_[b]) * relLimCoef_;
            } else {
                for (int b = 0; b < 4; ++b) limCur_[b] = 1.f;
            }

            // ---- 输出上一块：用本块算出的增益，相当于 1 个控制块（≈0.67 ms）的前视 ----
            //
            // 关键：块内插值从 limPrev_（旧增益，偏大）起步、逐步降到 limCur_（新增益）。
            // 但施加对象是**上一块**的样本，而上一块的峰值是按 limPrev_ 压过的，
            // 于是块首那一两个样本仍带着偏大的增益 → 实测超出上限 0.006~0.074 dB。
            // 砖墙限幅的定义就是"永不超过"，所以最后加一道硬钳位兜底。
            // （Limiter 类里有同样的单调队列 + 硬钳位，两条路径语义一致）
            for (int i = 0; i < n; ++i) {
                float t = (float)(i + 1) * inv;
                float oL = 0.f, oR = 0.f;
                for (int b = 0; b < 4; ++b) {
                    float g = limPrev_[b] + (limCur_[b] - limPrev_[b]) * t;
                    oL += pendL_[b][i] * g;
                    oR += pendR_[b][i] * g;
                }
                io[2 * (base + i)]     = p.bOn ? clampf(oL, -ceilLin, ceilLin) : oL;
                io[2 * (base + i) + 1] = p.bOn ? clampf(oR, -ceilLin, ceilLin) : oR;
            }
            for (int b = 0; b < 4; ++b) {
                memcpy(pendL_[b], bl[b], sizeof(float) * (size_t)n);
                memcpy(pendR_[b], br[b], sizeof(float) * (size_t)n);
            }
            base += n;
        }

        track(io, frames);
    }

private:
    public:
    // 累计削顶/非有限样本数（排查爆音用；正常运行应为 0）
    long ClippedSamples() const { return clipCount_.load(std::memory_order_relaxed); }
    void ResetClipCount() { clipCount_.store(0, std::memory_order_relaxed); }

private:
    void track(const float* io, int frames) {
        float blk = 0.f;
        // 失真/爆音直接证据。注意：不能用固定阈值 0.999 —— 「最大输出」设成 100%
        // 时合法输出本来就在 1.0 附近，会大量误报。必须按用户设定的上限 ceilDb 判定：
        // 超过上限才算失真（限幅失守），非有限值一律算。
        // clipLimit_ 由 Process 在块循环外算好一次（同一份 p），避免这里再读 params_。
        const float lim = clipLimit_;
        long clip = 0;
        for (int i = 0; i < frames; ++i) {
            float a = io[2 * i], b = io[2 * i + 1];
            if (!std::isfinite(a) || !std::isfinite(b)) { ++clip; continue; }
            float aa = fabsf(a), ab = fabsf(b);
            if (aa > lim) ++clip;
            if (ab > lim) ++clip;
            float pk = std::max(aa, ab);
            if (pk > blk) blk = pk;
            rmsAcc_ += a * a + b * b;
        }
        if (clip) clipCount_.fetch_add(clip, std::memory_order_relaxed);
        if (blk > peakOut_) peakOut_ = blk;
        if (blk > meterPeak_.load(std::memory_order_relaxed))
            meterPeak_.store(blk, std::memory_order_relaxed);
        rmsN_ += frames * 2;
    }

    float fs_ = 48000.f;
    float trSm_ = 0.f;
    float tilt_ = 0.f;
    float gPrev_[4] = {1, 1, 1, 1}, gCur_[4] = {1, 1, 1, 1};
    float limCur_[4] = {1, 1, 1, 1}, limPrev_[4] = {1, 1, 1, 1};   // 分频段限幅增益
    float pendL_[4][kCB]{}, pendR_[4][kCB]{};                     // 1 个控制块的前视缓存
    float relLimCoef_ = 0.0002f;
    float peakOut_ = 0.f, rmsAcc_ = 0.f;
    std::atomic<long> clipCount_{0};
    int   rmsN_ = 0;

    LR4 lp150_[2], hp150_[2], lp600_[2], hp600_[2], lp3k_[2], hp3k_[2];
    Biquad ap600_[2], ap3k_[2], ap3kb_[2];
    Env envB_[4];
    Env envFast_, envSlow_;
    Env envFastL_, envSlowL_;   // <150Hz 的快/慢包络
    Env envFastM_, envSlowM_;   // 150-3kHz 的快/慢包络
    Limiter limiter_;
    std::atomic<uint64_t> packed_{0};   // 参数打包传输（见 DspParams::Pack/Unpack）
    mutable DspParams cached_{};        // 仅供 Params() 返回引用用
    float clipLimit_ = 1.0009f;
    std::atomic<float> meterPeak_{0.f};
    std::atomic<float> meterInPeak_{0.f};
};

} // namespace sl
