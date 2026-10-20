<div align="center">

<img src="docs/assets/logo.png" alt="SoundLatch" width="128" height="128">

# SoundLatch

**PUBG 音频后处理器** — 降低环境音 / 强化脚步声 + 输出限幅

[![release](https://img.shields.io/badge/release-v1.3.0-D4537E?style=flat-square)](https://github.com/zhenliuMr/SoundLatch/releases/tag/v1.3.0)
[![platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![language](https://img.shields.io/badge/language-C%2B%2B20%7C%20Win32-00599C?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![size](https://img.shields.io/badge/exe-413%20KiB-5F5F5F?style=flat-square)](https://github.com/zhenliuMr/SoundLatch/releases/tag/v1.3.0)

[![cpu](https://img.shields.io/badge/DSP%20CPU-0.36%25-3B6D11?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![latency](https://img.shields.io/badge/算法延迟-0.67%20ms-3B6D11?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![tests](https://img.shields.io/badge/离线验收-12%20项%20PASS-639922?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)

[![download](https://img.shields.io/badge/下载-SoundLatch--1.3.0--x64.exe-0078D4?style=for-the-badge&logo=windows)](https://github.com/zhenliuMr/SoundLatch/releases/download/v1.3.0/SoundLatch-1.3.0-x64.exe)
[![vbcable](https://img.shields.io/badge/依赖-VB--Cable-BA7517?style=for-the-badge&logo=virtualbox)](https://vb-audio.com/Cable/)

</div>

---

单文件绿色版：解压即用，无安装、无运行库依赖。启动后驻留托盘，点图标展开面板。

- 32 位浮点处理，48 kHz
- 算法延迟 0.67 ms；端到端延迟取决于声卡缓冲
- 界面跟随系统浅/深色主题
- 配置存于 `%APPDATA%\SoundLatch\config.ini`

<details>
<summary><b>Contents</b></summary>

- [1. 快速开始](#1-快速开始)
- [2. 实测数据](#2-实测数据)
- [3. 原理](#3-原理)
- [4. 目录结构](#4-目录结构)
- [5. 编译](#5-编译)
- [6. 常见问题](#6-常见问题)
- [7. 声明](#7-声明)

</details>

---

## 1. 快速开始

```
游戏 → CABLE Input → [VB-Cable] → CABLE Output → SoundLatch → 耳机
```

1. 安装 VB-Cable：<https://vb-audio.com/Cable/> —— 以管理员身份运行，重启
2. 游戏「音频输出设备」设为 `CABLE Input`
3. 运行 SoundLatch：采集选 `CABLE Output`，输出选耳机

无VB-Cable 时可勾选「回环捕获」做无驱动测试，此时请把输出选成另一块设备。

### 1.1 两个开关

| 开关 | 作用 | 参数 |
|---|---|---|
| 降低环境音 + 强化脚步声 | 4 段动态处理（150 / 600 / 3 kHz 分频） | 环境抑制 0–100%、脚步增强 0…+12 dB |
| 锁定最大输出 | 前视砖墙限幅，输出峰值永不超过所设上限 | 最大输出 1% = −36 dBFS … 100% = 0 dBFS，0% = 静音 |
| 枪声事件 duck（自动） | 枪声命中时只压 <150 Hz 与 >3 kHz 各 6 dB，中频不动，180 ms 放开 | 无参数 |

面板底部：采集/输出设备、回环捕获、开机自启动、固定到屏幕、恢复默认。

「最大输出」是**绝对上限**，不是衰减量。电平柱的游标就是阈值，可直接在柱上拖动设置。
上限越低，枪声尾音被限幅压住的时间越长 —— 整段尾音都在压，混在里面的脚步也跟着变小声。
**别把它当"音量旋钮"往下拉**：−3 ~ −6 dBFS 已经足够不炸耳。

### 1.2 调参建议

| 场景 | 建议 |
|---|---|
| 野外听脚步 | 环境抑制 60–75%，脚步 +8 dB |
| 城区/房区 | 环境抑制 40–55%（枪声多，别压太狠） |
| 载具/飞机 | 环境抑制 85%，脚步 +10 dB |
| 嫌枪声炸耳 | 天花板 −3 ~ −6 dBFS（先靠自动 duck，不够再降） |

---

## 2. 实测数据

| 指标 | 数值 |
|---|---|
| 算法延迟 | **0.67 ms**（1 个控制块前视，脉冲法实测） |
| 链路延迟 | **63–79 ms**（本机端点缓冲各 22 ms，驱动固定） |
| DSP CPU | 0.36 %（单核） |
| 进程内存 | 17.7 MB |

链路延迟取决于声卡驱动缓冲。普通消费级主板/耳机通常 10 ms，本机约 25–30 ms。
本机驱动固定 22 ms（直连 Realtek 亦然），故下限 44 ms。

**验收**（`tests/dsp_test.exe` 12 项、`tests/overshoot_test.exe` 6 组场景）：

```
[T1] 限幅天花板 -6dB -> 总峰值 0.5012 (ceil 0.5012)      零过冲
[T2] 分频平坦度 @1k: 0.00 dB
[T3] 低频独大时 60Hz -19.3 dB / 1kHz -0.6 dB           环境压、脚步带不动
[T3b] 谱倾斜判据 5 用例                    独大→压；等幅/中频更大→不压
[T4] 瞬态增强 +6.1 dB
[T6] 爆炸期间 1kHz 脚步带 -4.9 dB / 事后 -0.0 dB       限幅不误伤脚步带
[T5] 20 s 音频处理耗时 71 ms -> 0.36% of one core

限幅溢出   6 组恶劣场景全部 +0.00 dB（含块内相位 0–31 全扫描）
分频平坦度 20 Hz–20 kHz 最大起伏 −0.173 dB @21.4 Hz
参数并发   800 万次快照，0 撕裂
```

**全量回归**（`tools/fullscan.exe`，素材库全部 11 043 条）：

```
规模   11 043 个 wem / 2.9 GB / 解码后 17.23 h
覆盖   pak 内唯一 Wwise 头 12 023 条：965 条 AES 加密 + 14 条解码器不支持，均已剔除
耗时   257 s（14 线程）= 241× 实时；DSP 单独 290× 实时（单核 0.35%）
结果   11 043/11 043 全部解码成功；削顶 0，非有限样本 0
```

解码走 `vgmstream-cli -i -w -p` 直连管道，不落盘；瓶颈在解码进程启动而不是 DSP。

---

## 3. 原理

```
输入 → 4 段 Linkwitz-Riley 分频(150/600/3000Hz，全通相位补偿)
     → 判据①谱倾斜：低频相对中高频独大 → 压 band0/band1（环境隆隆）
     → 判据②瞬态频段分布：中频比低频更陡 → 抬 band2（脚步主体）
     → 判据③枪声事件：>5 kHz 占比高 + 全带陡起 → 只压 band0/band3（炸耳/轰头）
     → 整条信号一个增益的砖墙限幅
     → 求和 → 输出      1 个控制块 ≈0.67 ms 前视
```

三个判据都与**绝对电平无关**，阈值由实测固化在 `src/dsp.h` 的 `namespace judge` / `duck`，面板不暴露滑杆。

判据③是 v1.3.0 新增的「枪声事件 duck」：枪声是宽带（>5 kHz 占比 0.35–0.47）且陡起（+20 dB 级），
命中后只在 4 ms 内把 **<150 Hz 与 >3 kHz** 压 4 dB、松手 180 ms 放开，**150–3 kHz 一点不动**。
附加峰值门控：本块输入没顶到用户设的天花板时，一点都不动 —— duck 的定位是"替限幅器分担"，
不是"识别枪声"（全量 11 043 条实测，频段占比分不开枪声与破片/玻璃/弹壳，射击游戏的音效 52 % 都是明亮宽带瞬态）。
这样炸耳与轰头的部分被削掉，脚步主体、方位线索、以及整条混音的电平都不会被拖着走 ——
旧的全带限幅在低上限下会把整段枪声+尾音一压到底（实测 3.2 s 尾音全程处于压缩态，脚步跟着掉 3–14 dB）。

| 判据 | 低频环境 | 脚步带 250–2k | 枪声带 2k–5k | 分离度 |
|---|---|---|---|---|
| 谱倾斜 tilt | **+16.6 dB** | **+2.1 dB** | −13.2 dB | **14 dB** |
| 瞬态陡度 P90 | +5.9 dB | +4.9 dB | +5.7 dB | 0.2 dB（分不开） |
| >5k 能量占比 | 0.03 | 0.04 | **0.35–0.47** | 10 倍 |

前两行来自 189 条三维分层样本（`tools/pick_samples.js`），第三行来自真实 Wwise 素材（`tools/probe_duck.cpp`）。
谱倾斜能分开环境，瞬态陡度分不开任何两类，枪声只能靠**高频占比 + 陡起**这一组相对判据。

频段依据来自 11 043 条 Wwise 音频流的实测（从 pak 的 12 023 个唯一头里剔除 965 条 AES 加密、14 条解码器不支持）（`docs/01`）：环境音 47 % 能量在 20–125 Hz，脚步 125 Hz 以下为 0、主体 250 Hz–2 kHz。

---

## 4. 目录结构

```
src/       dsp.h  engine.cpp/.h  main.cpp
tests/     dsp_test.cpp overshoot_test.cpp        离线验收 12 项 + 限幅溢出审计
           audit_*.cpp  duck_test.cpp             DSP / 引擎 / 并发审计探针
           engine_glitch_test.cpp starve_test.cpp  引擎异常场景
tools/     carve.py carve_check.py features.py       素材抽取与抽取质量复核
           pick_samples.js coverage.js               素材分层与覆盖统计
           analyze_sounds.py                         按频谱把素材分 6 类（docs/01 那张表）
           fix_probe.cpp mix_probe.cpp               判据标定与混音 SNR 实验
           fullscan.cpp scan_report.py               全量扫描（解码→DSP→CSV）与汇总
           probe_duck.cpp                            枪声判据选择性
           probe_limiter.cpp probe_bandsel.cpp       限幅掩蔽 / 频带选择性实验
           probe_ui.ps1                              启动行为与窗口枚举
           siggen.cpp lat_probe.cpp                  测试信号与延迟测量
docs/      01 频段分析 · 02 技术设计 · 03 任务分解 · 04 问题排查与全量测试
res/       app.rc app.ico app.manifest
build.bat  一键编译（需MinGW-w64，PATH 中可见 g++.exe）
```

## 5. 编译

```bat
build.bat
tests\dsp_test.exe
```

## 6. 常见问题

| 现象 | 原因 |
|---|---|
| 没声音 | 采集必须是 `CABLE Output`，输出是耳机；游戏输出设备必须是 `CABLE Input` |
| 回声/啸叫 | 勾了「回环捕获」但输出选成了正在采集的那块设备 |
| 偶发咔哒 | VB-Cable 的 Internal Sample Rate 设为 48000 后重启；96000 会逼引擎做 ±4 % 重采样 |
| 延迟高 | 蓝牙链路自带 100–200 ms，换有线 |
| 脚步不明显 | 提高「脚步增强」；环境压不住再加「环境抑制」 |
| 枪声一响整个场景变小/变闷 | 「最大输出」设太低了。上限是绝对天花板，枪声尾音高于它就会被压下去；**建议 −3 ~ −6 dBFS（滑杆 60–80 %）**，不要低于 −12 dBFS |
| 枪声听着不炸了但变单薄 | 正常。v1.3.0 的枪声事件 duck 只削 <150 Hz 与 >3 kHz（轰头与炸耳），中频不动，所以枪声会比以前"薄"一点 |

## 7. 声明

只处理本机已输出的音频，不注入游戏进程、不读写游戏文件、不修改游戏内存。
PUBG 音频资源仅在本地离线分析，用于确定分频参数。
