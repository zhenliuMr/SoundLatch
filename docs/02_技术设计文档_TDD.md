# 02 · 技术设计文档（TDD）· SoundLatch

> **⚠ 本文档中的 Mode B（自研 APO 驱动）方案已于 2026-10-21 移除。**
> 实测结论：Win11 第三方 APO 必须走微软驱动包扩展 INF + 官方签名，无法自签挂载；
> 直接写 MMDevices 下的 `Fx\0` 会被 AudioEndpointBuilder 重建时删除（验证两次）。
> 当前仅保留 Mode A（WASAPI 回环 + VB-Cable），相关代码与安装包已删除。
> 本文档其余内容（频段分析结论、DSP 设计、限幅器原理）仍然有效。

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
                        │  UI（同进程，Win32 + GDI）  │
                        │  开关A / 阈值 / 开关B / 表 │
                        └────────────────────────────┘
```

- 音频侧**单进程三线程**即可（捕获/渲染可合并为同一回调，见 §5.1）；UI 与音频线程通过**无锁参数快照**通信。
- 参数传递：`std::atomic<ParamsSnapshot>` 双缓冲，UI 写、音频线程每块读一次，无锁无阻塞。

---

## 4. DSP 内核设计

### 4.1 处理链（与 `src/dsp.h` 一致）

```
in → 4 段 Linkwitz-Riley LR4 分频(150/600/3000 Hz，全通相位补偿)
     ├─ band0 (<150)   ：-20 dB × envAmt × envGate(谱倾斜) + 枪声 duck
     ├─ band1 (150-600)：-7 dB × envAmt × envGate + 脚步增强 3.6 dB
     ├─ band2 (0.6-3k) ：脚步增强 12 dB（不动环境抑制，也不被 duck 碰）
     └─ band3 (>3k)    ：脚步增强 5.4 dB + 枪声 duck
     → 求和 → 1 个控制块(32 样本 ≈0.67 ms)前视砖墙限幅 → 硬钳位兜底 → out
```

三组判据的强度都是 0..1 的连续量，乘在各自的 dB 增益上，再按控制块线性斜坡。

### 4.2 分频

- **Linkwitz-Riley 4 阶**（两级 Butterworth 级联），相位对齐、求和平坦（实测 20 Hz–20 kHz 最大起伏 −0.173 dB）。
- 全部 IIR，**零额外延迟**；不使用线性相位 FIR。
- 相位补偿：band0 串 `ap600 + ap3k`，band1 串 `ap3kb`，四段相加才平。

### 4.3 判据（全部与绝对电平无关）

| 判据 | 定义 | 门限 | 作用 |
|---|---|---|---|
| ① 谱倾斜 | `tilt = L0 − mean(L1,L2)`（dB） | 11 → 22 dB 满档 | 低频独大 → 压 band0/band1（环境隆隆、载具） |
| ② 瞬态频段分布 | `trSm = clamp((tM−2)/12) × clamp((tM−tL−4)/6)` | 中频瞬态比低频陡 4 dB | 抬 band1/2/3（脚步主体） |
| ③ 枪声事件 | `f3 = E(>5k)/E(全带)` × `tAll = fast−slow` | 0.20→0.36 / 6→14 dB | 只压 band0/band3（轰头与炸耳） |

- 包络：fast 3 ms、slow 150 ms；band 级 5 ms / 120 ms。
- 判据 ③ 额外要求：本块输入峰值 ≥ 0.98 × 上限（**峰值门控**），且 `trSm > 0.25` 时不做（与判据 ② 互斥）。
- 判据 ③ 的快攻慢放：4 ms 压、180 ms 放，深度 4 dB。
- 常数固化在 `src/dsp.h` 的 `namespace judge` / `namespace duck`，面板不暴露。

> 为什么 ③ 不能"识别枪声"：全量 11 043 条素材实测，52 % 的文件都含明亮宽带瞬态（枪声/破片/玻璃/弹壳），
> 频段占比分不开。所以 ③ 的定位是「信号确实顶到天花板时替限幅器分担」，靠峰值门控兜住误触发。

### 4.4 增益映射（面板滑杆 → 实际参数）

| 滑杆 | 范围 | 映射 |
|---|---|---|
| 环境抑制 `envAmt` | 0–100 % | band0 `−20 dB × k`、band1 `−7 dB × k`（k = 百分比/100），再乘判据 ① 的 gate |
| 脚步增强 `stepAmt` | 0–100 % | band1 `+3.6 dB × k`、band2 `+12 dB × k`、band3 `+5.4 dB × k`，再乘 `trSm` 与电平 guard |
| 最大输出 `ceilDb` | 1 % = −36 dBFS … 100 % = 0 dBFS，0 % = 静音 | 限幅器绝对上限；同时是判据 ③ 峰值门控的参考 |

> guard = `clamp((−6 − bandLevel_dBFS)/12 + 1, 0, 1)`：该频段已经很响时自动收缩增强，防止把枪声顶爆。

### 4.5 输出限幅器（开关 B）

```
延时线 D = lookahead × fs（默认 1.5 ms ≈ 72 样本）
peak   = max(|x| 前视窗口)                 // 单调队列 O(1)
target = min(1, Ceil / peak)
gain   = min(gain, target)                 // 瞬时压，无过冲
gain  += (target − gain) × releaseCoef     // release 35 ms（v1.3.0 由 80 ms 缩短）
out    = delayed × gain → clamp(±Ceil)     // 硬钳位兜底
```

- 硬保证 `|out| ≤ Ceil`：`tests/overshoot_test.exe` 6 组恶劣场景溢出 +0.00 dB（唯一 +0.01 dB 出现在 ceil=静音）。
- 增益按**整条信号一个值**：不分频段，所以枪声期间整条混音同比例下降 —— 这正是 v1.3.0 引入判据 ③ 的原因。
- 攻击由前视完成，不额外加缓冲。

### 4.6 实测开销

| 项 | 实测 |
|---|---|
| 每样本 | 8 个 LR4 双二阶 ×2 声道 + 3 个全通 + 7 个包络 + 1 个 5 kHz 探测双二阶 |
| CPU | **0.35 % 单核**（`tests/dsp_test.exe` T5：20 s 音频 70 ms） |
| 进程常驻 | **17.7 MB** |
| 全量离线 | 11 043 条 / 17.23 h，241× 实时（14 线程，瓶颈在解码器进程启动） |

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
10. **异常保护**：MinGW 无 SEH，改为不抛异常 + `std::isfinite` 逐样本兜底；设备被拔/切换时 200 ms 内重建流并淡入。

---


## 6. UI（实际形态：托盘面板，不是主窗口）

```
托盘图标 ──左键──► 浮动面板（右下角向左上展开，无标题栏，客户区可拖动）
          └右键──► 菜单：控制面板 / 退出
```

面板内容（自上而下）：

| 区 | 控件 |
|---|---|
| 开关 1 | ☑ 降低环境音 + 强化脚步声 |
| | 环境抑制 0–100 % ｜ 脚步增强 0–100 %（0…+12 dB） |
| 开关 2 | ☑ 锁定最大输出音量 |
| | 最大输出 1 % = −36 dBFS … 100 % = 0 dBFS（0 % = 静音） |
| 电平 | 输入电平柱 + 上限游标（可直接在柱上拖阈值） |
| 设备 | 采集（下拉，每次展开重新枚举）｜ 输出（下拉） |
| 底部 | 回环捕获 ｜ 开机自启动 ｜ 固定到屏幕 ｜ 恢复默认 |

- 全部控件原生 Win32 + GDI 自绘；深浅色跟随系统（uxtheme 私有 API + DWM），无第三方 UI 框架。
- 改动立即生效，收起面板/退出时落盘 `%APPDATA%\SoundLatch\config.ini`（`key=value`，不是 JSON）。
- 两个「？」由自绘气泡解释，不受系统 tooltip 超时影响。
- 已知：首次启动只驻留托盘、不弹面板（v1.3.0 未改），见 `docs/04` 第 1 节。

---

## 7. 技术栈与产物（实际）

| 层 | 选型 | 说明 |
|---|---|---|
| 语言 | C++20（MinGW-w64 g++ 16.2，`-O2 -municode -mwindows`） | 静态链接 libgcc/libstdc++，无运行库依赖 |
| 音频 | WASAPI 共享模式 + MMCSS/AVRT | 独占模式实测被驱动拒绝，见 §11 |
| UI | 原生 Win32 + GDI 自绘 | 无 ImGui / Direct2D / WebView |
| 配置 | `%APPDATA%\SoundLatch\config.ini` | 纯文本 key=value |
| 构建 | `build.bat`（windres + g++） | 一次产出单文件 exe |
| 产物 | `dist/SoundLatch.exe` | 413 KiB，常驻 17.7 MB |

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

## 9. 实际接口（与代码一致）

```cpp
// dsp.h —— 无 Windows 依赖，可离线单测
struct DspParams {
    bool  aOn = true, bOn = true, bypass = false;
    float envAmt = 0.55f, stepAmt = 0.65f;   // 0..1
    float ceilDb = -3.0f, lookMs = 1.5f;
};

class DspCore {
public:
    void Prepare(float fs);                  // 48 kHz
    void SetParams(const DspParams& p);      // UI 线程调用，uint64 原子打包，无撕裂
    void Process(float* interleaved, int frames);  // 音频线程；无分配/锁/异常
    // 观测用：LastTilt / LastTrSm / LastDuck / LastF3 / LastTAll / ClippedSamples
    //           MeterPeak / MeterInPeak / TakePeak / TakeRms
};
```

```cpp
// engine.h —— WASAPI 双线程
class AudioEngine {
    bool Start(captureId, renderId, loopbackCapture, DspCore*, std::wstring& err);
    void Stop();
    float LatencyMs() const;   // 采集缓冲 + 环形水位 + 渲染缓冲 + 前视
};
```

---

## 10. 验收口径（实际执行的）

| 项 | 判据 | 执行 |
|---|---|---|
| 限幅零过冲 | 6 组恶劣场景（含块内相位 0–31 全扫描）溢出 ≤ +0.01 dB | `tests/overshoot_test.exe` |
| 分频平坦度 | 20 Hz–20 kHz 起伏 ≤ 0.5 dB（实测最大 −0.173 dB @21.4 Hz） | `tests/dsp_test.exe` T2 |
| 环境抑制选择性 | 低频独大被压 ≥ 12 dB、脚步带 \|Δ\| < 3 dB；等幅/中频更大不压 | T3 / T3b |
| 脚步增强 | 瞬态段 ≥ +6 dB，稳态段 ≈ 0 | T4 |
| CPU | 20 s 音频处理 ≤ 1.5 % 单核（实测 0.35 %） | T5 |
| 全量稳定性 | 11 043 条素材：削顶 0、非有限样本 0 | `tools/fullscan.exe` |

延迟口径：**算法延迟 0.67 ms**（1 个控制块前视，脉冲法）；**链路延迟 63–79 ms**，由端点缓冲决定（本机 22 ms/端）。

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
