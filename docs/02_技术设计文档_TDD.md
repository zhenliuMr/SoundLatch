# 02 · 技术设计文档（TDD）· SoundLatch

> 项目：Windows 实时游戏音频处理器（PUBG 场景）
> 版本：v1.0 草案　日期：2026-10-19
> 关联：`01_PUBG音频频段分析.md`（频段依据）、`03_任务分解与验收.md`（排期）

---

## 1. 目标与非目标

### 1.1 目标（Must）

| # | 需求 | 量化指标 |
|---|---|---|
| G1 | 开关 A：降低环境音 + 提升脚步声，阈值可调 | 环境段衰减 0 ~ -18 dB；脚步增强 0 ~ +12 dB；两个阈值独立滑杆 |
| G2 | 开关 B：输出上限锁定（限幅器），阈值可调 | 天花板 -24 ~ 0 dBFS；超过即压，不超不动；无削波、无泵感 |
| G3 | 延迟 | **Mode A ≤ 20 ms**（默认 12–16 ms）；**Mode B ≤ 4 ms** |
| G4 | 资源占用 | 音频线程 CPU ≤ 1.5 %（单核，i5-8400 基准）；进程常驻内存 ≤ 30 MB |
| G5 | 稳定 | 8 小时不爆音/不累积漂移/不掉帧；音频线程零分配、零锁、零系统调用 |
| G6 | 兼容 | Win10 2004+ / Win11；48 kHz 为主，44.1/96 kHz 自动重采样；不改游戏文件 |

### 1.2 非目标（Won't）

- 不做"雷达/透视/外挂"类功能，只处理**本机已输出的音频**。
- 不做云端、不做账号、不做遥测上传。
- 不修改 PUBG 游戏文件（Pak 不写、不注入进程），避免 BattlEye 风险。

---

## 2. 关键约束与结论：延迟从哪来

游戏音频要在"出游戏 → 进我们 → 出我们 → 进声卡"之间走两遍缓冲。**用户态方案每跨一次缓冲就多一个 period**。业界的实测基线：

| 通路 | 实测延迟 | 来源 |
|---|---|---|
| WASAPI 共享模式 | **> 20 ms 地板**，与缓冲区无关 | FlexASIO issue / 独立测试 |
| WASAPI 独占模式 | 4–10 ms（受硬件 period 限制） | 同上 |
| IAudioClient3 低延迟共享 | 理论 1.3 ms（需驱动支持） | 微软 Low Latency Audio |
| VB-Cable 内部缓冲 | 默认 2048 样本 ≈ 21.3 ms；调 512 样本 ≈ 5 ms | VB-Audio 官方论坛 |
| APO（音频引擎内处理） | 仅算法延迟（lookahead），**0 缓冲** | 微软 APO 架构 |

### 2.1 延迟预算

**Mode A｜虚拟声卡（默认，通用）**

| 环节 | 典型 | 优化后 |
|---|---|---|
| 游戏 → CABLE Input | 10 ms（共享 period） | 10 ms |
| CABLE 内部缓冲 | 21.3 ms（默认） | **5.3 ms**（设 512 样本） |
| 我们捕获缓冲 | 10 ms（480 帧） | **2.67 ms**（240 帧 / IAudioClient3） |
| DSP（IIR，零延迟） | 0 | 0 |
| 限幅器 lookahead | 2 ms | 1 ms |
| 我们渲染缓冲 | 10 ms | **2.67 ms** |
| **合计** | **53 ms** ❌ | **≈ 21 ms** |

> 结论：Mode A 的**物理下限约 10–15 ms**，实际可做到 **12–20 ms**。再往下必须进入音频引擎内部。

**Mode B｜APO / 引擎内插件（低延迟模式）**

| 环节 | 延迟 |
|---|---|
| 引擎 period | 0（本来就要走） |
| DSP | 0 |
| 限幅器 lookahead | 1–2 ms |
| **合计** | **1–2 ms** ✅ |

### 2.2 架构选型对比

| 方案 | 延迟 | 免驱 | 反作弊风险 | 实现成本 | 结论 |
|---|---|---|---|---|---|
| **A. 虚拟声卡 + 用户态 DSP** | 12–20 ms | 需装 VB-Cable（已签名） | 低（标准音频设备） | 低 | ✅ **v1 主线** |
| **B. 自研 APO** | 1–3 ms | 需 INF + 微软签名 | 中（用户态驱动仍可能被质疑） | 高（EV 证书/WHQL/Win11 24H2 新规） | ✅ **v2 极速模式** |
| C. 进程环回捕获 + 静音原流 | 10–20 ms | 免驱 | 低 | 中 | ❌ 会话音量会同时作用于环回流，需 1% 音量回补，双流时钟漂移难控 |
| D. 直接改系统"响度均衡/音效增强" | 0 | 免驱 | 无 | 极低 | ❌ 只能抬响度，无法分频；体验更差 |
| E. 注入游戏进程（Hook Wwise） | 0 | 免驱 | **极高（封号）** | 高 | ❌ 禁止 |

**决策**：一套 DSP 内核，两种宿主适配器（Host Adapter）。
- `HostA_WasapiVirtualCable`：捕获 CABLE Output → 处理 → 渲染到耳机。
- `HostB_Apo`：导出 `IAudioProcessingObject` 实现，装到目标端点。
核心 `DspCore` 与宿主解耦（纯 `float*` 进 `float*` 出，无 Windows 依赖，可单测）。

---

## 3. 系统架构

```
┌──────────┐   WASAPI 共享    ┌──────────────┐
│  PUBG    │ ───────────────► │ CABLE Input  │  (虚拟声卡，驱动层)
└──────────┘                  └──────┬───────┘
                                     │ CABLE Output
                                     ▼
                        ┌────────────────────────────┐
                        │  SoundLatch Audio Engine    │
                        │  ┌──────────────────────┐  │
                        │  │ Capture Thread       │  │  MMCSS "Pro Audio"
                        │  │ WASAPI event-driven  │  │  AVRT_PRIORITY_CRITICAL
                        │  └────────┬─────────────┘  │
                        │     lock-free SPSC ring    │
                        │  ┌────────▼─────────────┐  │
                        │  │ DSP Thread           │  │
                        │  │ DspCore (4-band)     │  │
                        │  └────────┬─────────────┘  │
                        │           │ ring           │
                        │  ┌────────▼─────────────┐  │
                        │  │ Render Thread        │  │
                        │  │ WASAPI → 耳机        │  │
                        │  └──────────────────────┘  │
                        └───────────┬────────────────┘
                                    │ 共享内存（参数/电平）
                        ┌───────────▼────────────────┐
                        │  UI 进程 (Win32 + D2D)     │
                        │  开关A / 阈值 / 开关B / 表 │
                        └────────────────────────────┘
```

- 音频侧**单进程三线程**即可（捕获/渲染可合并为同一回调，见 §5.1）；UI 与音频线程通过**无锁参数快照**通信。
- 参数传递：`std::atomic<ParamsSnapshot>` 双缓冲，UI 写、音频线程每块读一次，无锁无阻塞。

---

## 4. DSP 内核设计

### 4.1 处理链

```
in → DC Block → 分频(LR4, 150/600/3000) → B1/B2/B3/B4
      ├─ B1: 静态搁架衰减 + 向下扩展(稳态环境抑制)
      ├─ B2/B3/B4: 瞬态检测门控的向上增益 + 稳态向下扩展
      → 求和 → 输出限幅器(开关B) → 软削波保护 → out
```

### 4.2 分频

- **Linkwitz-Riley 4 阶**（两级 Butterworth 级联），相位对齐、求和平坦；**IIR 因果，零延迟**。
- 禁止使用线性相位 FIR（会引入 (N-1)/2 采样延迟，违背 G3）。
- 系数在 48 kHz 下预计算，切换采样率时重算；用 TDF-II 结构保证数值稳定。

### 4.3 包络与瞬态检测

```
Efast: |x| 一阶低通，τa = 3 ms
Eslow: |x| 一阶低通，τa = 150 ms
T_dB  = 20log10(Efast) - 20log10(Eslow)      // 瞬态度
g_tr  = clamp((T_dB - TrSens) / 12, 0, 1)    // TrSens = 瞬态灵敏度阈值滑杆
```

- 脚步特征：`T_dB` 在起音 10 ms 内冲到 **15–30 dB**，随后 100 ms 内衰减；环境音 `T_dB < 6 dB`。
- `g_tr` 经 5 ms attack / 120 ms release 平滑，避免"抽气"。

### 4.4 增益映射（用户滑杆 → 实际参数）

| 滑杆 | 范围 | 映射 |
|---|---|---|
| 环境抑制 `EnvAmt` | 0–100 % | `B1_gain = -18 dB × (EnvAmt/100)`（搁架 150 Hz）<br>`B1_floor = -12 dB × (EnvAmt/100)`（稳态向下扩展最大衰减） |
| 环境阈值 `EnvThr` | -60 ~ -20 dBFS | B1 向下扩展阈值；低于阈值按 2:1 向下扩展 |
| 脚步增强 `StepAmt` | 0–100 % | `B2 += 3 dB·k`，`B3 += 12 dB·k`，`B4 += 6 dB·k`（k = StepAmt/100） |
| 脚步灵敏度 `StepSens` | 0–100 % | 映射 `TrSens = 6 dB → 0 dB`（越灵敏越容易触发） |
| 限幅天花板 `Ceil` | -24 ~ 0 dBFS | 限幅器输出上限 |
| 限幅启动 `Look` | 0.5–5 ms | lookahead，默认 1.5 ms |

> 边界保护：B3 增益上限 +12 dB 时，若频段电平已 > -6 dBFS，则自动按 `1/(1+excess)` 收缩增益（防"提脚步把枪声顶爆"）。

### 4.5 输出限幅器（开关 B）

```
延时线 D = lookahead × fs
peak = max(|x| 前视窗口)              // 滑动最大值，单调队列 O(1)
target = min(1, Ceil / peak)
gain   = min(gain, target)            // 瞬时压，无过冲
gain  += (target - gain) × releaseCoef // release 80 ms 平滑
out    = delayed × gain
```

- 硬保证：`|out| ≤ Ceil`，**零过冲**（前视 + 单调队列）。
- 软膝 3 dB，避免阈值附近"抽吸"。
- 可选 2× 过采样峰值检测，防 inter-sample peak（默认关闭，省 CPU）。
- 攻击时间由 lookahead 决定，**不引入额外缓冲**（在已存在的延时线内完成）。

### 4.6 CPU / 内存预算

| 项 | 估算 |
|---|---|
| Biquad 数 | 4 段 × 2（LR4 两级）× 2 声道 = 16 个双二阶 |
| 每样本运算 | ≈ 16 × 5 mul/add ≈ 80 FLOP + 包络 ≈ 20 |
| 48 kHz 立体声 | ≈ 9.6 MFLOPS ≈ **0.3–1.2 % 单核**（无 SIMD）；AVX2 可再降 2–4× |
| 缓冲内存 | 环形缓冲 2 × 4096 帧 × 2ch × 4B ≈ 64 KB |
| 进程常驻 | 引擎 ~8 MB + UI/ImGui/D3D11 ~12 MB → **目标 ≤ 30 MB** |

---

## 5. 实时线程实现规范（硬性）

1. **MMCSS**：`AvSetMmThreadCharacteristics(L"Pro Audio")` + `AvSetMmThreadPriority(AVRT_PRIORITY_CRITICAL)`。
2. **零分配**：所有缓冲、对象在初始化阶段一次性分配；处理路径禁止 `new/vector 扩容/lock/unordered_map`。
3. **零锁**：SPSC 无锁环形缓冲（`std::atomic<size_t>` 读写指针，acquire/release）。
4. **零系统调用**：不写日志、不读文件、不查注册表、不调 `GetTickCount`（用 QPC 仅在异常检测时）。
5. **无页错误**：`VirtualLock` 锁住音频缓冲。
6. **事件驱动**：`IAudioClient::SetEventHandle` + `WaitForSingleObject`，不用 sleep 轮询。
7. **WASAPI 配置**：`ShareMode=Shared`、`AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`；优先 `IAudioClient3::InitializeSharedAudioStream` 请求 240 帧 period，失败回退 480 帧。
8. **渲染端可请求独占**：提供"极限延迟"选项（独占 + 48 kHz/24 bit），失败自动回退共享。
9. **时钟漂移**：捕获与渲染来自不同设备（CABLE vs 耳机），必须做 **drift 补偿**：以环形缓冲水位为目标（50 %），每 1024 帧最多调整 ±1 帧（≈ ±0.1 %，听感不可察）；水位 >85 % 或 <15 % 触发丢/补一帧，避免累积溢出。
10. **异常保护**：`__try/__except`（MSVC SEH）包住回调；设备被拔/切换时 200 ms 内重建流并静音淡入。

---

## 6. UI 设计（最小可用）

单窗口 ~420×320，D2D 自绘，无外部 UI 框架：

```
┌─────────────────────────────────────────┐
│  SoundLatch                    [—][□][×] │
├─────────────────────────────────────────┤
│  ① 脚步增强            [ ●────── ] ON   │
│     环境抑制   ─────●──────   60 %      │
│     环境阈值   ──●────────  -42 dBFS    │
│     脚步增益   ───────●───   +8 dB      │
│     瞬态灵敏度 ────●──────   70 %       │
│                                         │
│  ② 输出限幅            [ ●────── ] ON   │
│     天花板     ──────────●  -3.0 dBFS   │
│                                         │
│  [实时电平] ▁▃▅▇▅▃▁  峰值 -6.2 dBFS     │
│  设备: CABLE Output → 耳机 (Realtek)    │
│  状态: 运行中 · 18 ms · CPU 0.9 %       │
└─────────────────────────────────────────┘
```

- 参数改动**立即生效**（无"应用"按钮），写入 `%APPDATA%\SoundLatch\config.json`。
- 首次运行向导：检测 VB-Cable → 引导安装 → 自动把 PUBG 输出设为 CABLE Input → 选择耳机为渲染设备。
- 提供 **A/B 对比**（勾选「旁通」临时旁路处理），供耳朵验收。热键功能已按需求移除。

---

## 7. 技术栈与产物

| 层 | 选型 | 理由 |
|---|---|---|
| 语言 | C++20（MSVC 2022，/O2 /fp:fast） | 实时确定性、无 GC、单 exe |
| 音频 | WASAPI（Core Audio）+ MMCSS/AVRT | 系统原生最低延迟路径 |
| UI | Win32 + Direct2D + Dear ImGui (D3D11) | 体积极小（~300 KB），无 WebView 开销 |
| 配置 | JSON（nlohmann/json, header-only） | 简单、可手改 |
| 构建 | CMake 3.25+ | 一键出 Release |
| 打包 | 单文件 exe + 可选静默安装脚本（含 VB-Cable 检测/引导） | 用户零配置 |
| 测试 | GoogleTest（DSP 数值）+ 自研 Loopback 延迟测量工具 | 可回归 |

**产物**：`SoundLatch.exe`（≤ 3 MB）、`config.json`、`INSTALL.md`。

---

## 8. 风险登记

| ID | 风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | Mode A 延迟 20 ms 仍被用户判为"有延迟" | 高 | 交付 Mode B（APO）路线图；Mode A 默认即优化配置（512 样本 CABLE + 240 帧） |
| R2 | BattlEye 对虚拟声卡/音频钩子敏感 | 高 | 不注入游戏进程、不 Hook、不装自签驱动；只做标准 WASAPI 客户端 |
| R3 | 用户游戏用"独占模式"输出 | 中 | 独占模式下虚拟声卡仍可用（CABLE 是独立设备）；仅 APO 模式失效，UI 提示 |
| R4 | 提脚步同时放大枪声 → 刺耳/掩盖 | 高 | B3 电平自适应收缩 + 限幅器兜底（§4.4） |
| R5 | 蓝牙耳机额外 100–200 ms | 中 | 启动时检测传输类型，UI 告警"蓝牙链路不适合竞技" |
| R6 | 采样率/声道不匹配导致变调、杂音 | 中 | 强制 AUTOCONVERTPCM + 统一 48 kHz 内部处理 |
| R7 | 长时间运行水位漂移 → 爆音 | 中 | §5.9 drift 补偿 + 水位监控 |
| R8 | Win11 24H2 APO 新规（必须 Class=AudioProcessingObject + 签名） | 中 | Mode B 延后，先做 A；B 走微软 attestation 签名 |
| R9 | 游戏更新改变混音（新赛季音效重做） | 低 | 内置实时频谱 + "一键标定"，参数不写死 |

---

## 9. 参考实现骨架

```cpp
// dsp_core.h —— 无 Windows 依赖，可单测
struct Params { float envAmt, envThrDb, stepAmt, stepSens, ceilDb, lookMs; bool aOn, bOn; };

class DspCore {
public:
    void Prepare(double fs, int maxFrames);
    // 实时安全：无分配、无锁、无异常
    void Process(const float* in[2], float* out[2], int frames) noexcept;
    void SetParams(const Params& p) noexcept;   // 每块调用，内部做平滑
private:
    Biquad  lp[4][2], hp[4][2];     // LR4 crossover
    EnvF    fast[2], slow[2], band[4][2];
    Limiter lim;
    ParamSmoother sm;
};
```

```cpp
// 瞬态门控增益（B3 示例）
float tr = 20.f*log10f(fast + 1e-9f) - 20.f*log10f(slow + 1e-9f);
float k  = clampf((tr - trSens) / 12.f, 0.f, 1.f);       // 0..1
float g  = db2lin(stepGainDb[1] * k * levelGuard);        // levelGuard 防顶爆
b3 = b3 * g;
```

---

## 10. 验收口径（详见 03）

- 延迟：Loopback 脉冲法测 `in→out`，Mode A ≤ 20 ms、Mode B ≤ 4 ms（50 次均值，抖动 < 3 ms）。
- CPU：PerfMon 采样 10 min，音频线程均值 ≤ 1.5 %。
- 内存：Private Bytes ≤ 30 MB（运行 1 h 后）。
- 听感：盲测 10 人，≥ 8 人认为"脚步更清晰且枪声不刺耳"。

---

## 11. 实现记录（v0.9，2026-10-19）

### 11.1 已落地

| 项 | 状态 | 说明 |
|---|---|---|
| DSP 内核 | ✅ | 4 段 LR4 + 全通相位补偿 + 瞬态门控 + 前视限幅，控制率 32 样点 |
| WASAPI 采集/渲染 | ✅ | 双线程 + APPA 事件驱动 + MMCSS Pro Audio + FTZ/DAZ |
| 环形缓冲 / 漂移 | ✅ | 预填充一个采集周期 + 单帧渐进修正，实测 0 欠载/0 溢出 |
| UI / 配置 / 热键 | ✅ | Win32 自绘，参数即时生效，配置持久化 |
| 离线验收 | ✅ | `tests/dsp_test.exe` 5 项全过 |

### 11.2 与设计的偏差（重要）

| 设计假设 | 实测 | 处理 |
|---|---|---|
| 共享模式缓冲 10 ms/端 | **本机所有端点均为 22 ms（1056 帧）**，`IAudioClient3` 低延迟周期不支持，独占模式 float 也被拒 | 保留共享模式；延迟 = 22+22+环+前视 ≈ 53.5 ms。换机器（普通 Realtek/USB DAC）同代码约 25–30 ms |
| 10 ms 缓冲即可无欠载 | 两线程相位会碰撞，实测 3 次/秒欠载 | 增加"预填充一个采集周期"环节，代价 8 ms，欠载归零 |
| 用户态可做到 20 ms | 硬件/驱动周期决定下限，软件无法突破 | 维持原判断：要 <5 ms 必须走 APO（Mode B） |
| — | `WAVEFORMATEX`（tag=3）虽返回 S_OK，引擎仍按 22 ms 分配 | 改用 `WAVEFORMATEXTENSIBLE` + IEEE_FLOAT 子格式，避免 SRC |

### 11.3 性能实测（本机）

| 项 | 结果 |
|---|---|
| DSP 离线 | 20 s 音频 76 ms → **0.38 % 单核** |
| 进程 CPU | 2.3 %（含 UI 定时器与电平表刷新） |
| 内存 | 17.7 MB |
| 延迟 | 53.5 ms（采 22 + 环 8 + 放 22 + 前视 1.5） |
| 欠载/溢出 | 0 / 0 |

### 11.4 后续可做的低延迟优化

1. 换用支持 `IAudioClient3` 的端点（Intel SST / 部分 USB DAC）→ 单端可降到 2.7–5 ms。
2. 增加"独占输出"选项：需按设备混音格式做 int16/int24 转换，且会独占耳机。
3. Mode B（APO）仍是唯一能做到 1–3 ms 的方案，前置条件见 §2.2。
