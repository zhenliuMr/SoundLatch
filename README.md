<div align="center">

<img src="docs/assets/logo.png" alt="SoundLatch" width="128" height="128">

# SoundLatch

**PUBG 音频后处理器** — 降低环境音 / 强化脚步声 + 输出限幅

[![release](https://img.shields.io/badge/release-v1.2.0-D4537E?style=flat-square)](https://github.com/zhenliuMr/SoundLatch/releases/tag/v1.2.0)
[![platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![language](https://img.shields.io/badge/language-C%2B%2B20%7C%20Win32-00599C?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![size](https://img.shields.io/badge/exe-412%20KiB-5F5F5F?style=flat-square)](https://github.com/zhenliuMr/SoundLatch/releases/tag/v1.2.0)

[![cpu](https://img.shields.io/badge/DSP%20CPU-0.34%25-3B6D11?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![latency](https://img.shields.io/badge/算法延迟-0.67%20ms-3B6D11?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)
[![tests](https://img.shields.io/badge/离线验收-12%20项%20PASS-639922?style=flat-square)](https://github.com/zhenliuMr/SoundLatch)

[![download](https://img.shields.io/badge/下载-SoundLatch--1.2.0--x64.exe-0078D4?style=for-the-badge&logo=windows)](https://github.com/zhenliuMr/SoundLatch/releases/download/v1.2.0/SoundLatch-1.2.0-x64.exe)
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

面板底部：采集/输出设备、回环捕获、开机自启动、固定到屏幕、恢复默认。

「最大输出」是**绝对上限**，不是衰减量。电平柱的游标就是阈值，可直接在柱上拖动设置。

### 1.2 调参建议

| 场景 | 建议 |
|---|---|
| 野外听脚步 | 环境抑制 60–75%，脚步 +8 dB |
| 城区/房区 | 环境抑制 40–55%（枪声多，别压太狠） |
| 载具/飞机 | 环境抑制 85%，脚步 +10 dB |
| 嫌枪声炸耳 | 天花板 −6 ~ −9 dBFS |

---

## 2. 实测数据

| 指标 | 数值 |
|---|---|
| 算法延迟 | **0.67 ms**（1 个控制块前视，脉冲法实测） |
| 链路延迟 | **63–79 ms**（本机端点缓冲各 22 ms，驱动固定） |
| DSP CPU | 0.34 %（单核） |
| 进程内存 | 17.7 MB |

链路延迟取决于声卡驱动缓冲。普通消费级主板/耳机通常 10 ms，本机约 25–30 ms。
本机驱动固定 22 ms（直连 Realtek 亦然），故下限 44 ms。

**验收**（`tests/dsp_test.exe` 12 项、`tests/overshoot_test.exe` 6 组场景）：

```
[T1] 限幅天花板 -6dB -> 总峰值 0.5012 (ceil 0.5012)      零过冲
[T2] 分频平坦度 @1k: 0.00 dB
[T3] 低频独大时 60Hz -19.3 dB / 1kHz -0.6 dB           环境压、脚步带不动
[T3b] 谱倾斜判据 5 用例                    独大→压；等幅/中频更大→不压
[T4] 瞬态增强 +6.4 dB
[T6] 爆炸期间 1kHz 脚步带 -5.1 dB / 事后 -0.1 dB       限幅不误伤脚步带
[T5] 20 s 音频处理耗时 69 ms -> 0.34% of one core

限幅溢出   6 组恶劣场景全部 +0.00 dB（含块内相位 0–31 全扫描）
分频平坦度 20 Hz–20 kHz 最大起伏 −0.173 dB @21.4 Hz
参数并发   800 万次快照，0 撕裂
```

---

## 3. 原理

```
输入 → 4 段 Linkwitz-Riley 分频(150/600/3000Hz，全通相位补偿)
     → 判据①谱倾斜：低频相对中高频独大 → 压 band0/band1（环境隆隆）
     → 判据②瞬态频段分布：中频比低频更陡 → 抬 band2（脚步主体）
     → 整条信号一个增益的砖墙限幅
     → 求和 → 输出      1 个控制块 ≈0.67 ms 前视
```

两个判据都与**绝对电平无关**，阈值由实测固化在 `src/dsp.h` 的 `namespace judge`，面板不暴露滑杆。

依据是189 条三维分层样本的实测（`tools/pick_samples.js` 分层，覆盖素材库全部62 个非空频谱形态格）：

| 判据 | 低频环境 | 脚步带 250–2k | 枪声带 2k–5k | 分离度 |
|---|---|---|---|---|
| 谱倾斜 tilt | **+16.6 dB** | **+2.1 dB** | −13.2 dB | **14 dB** |
| 瞬态陡度 P90 | +5.9 dB | +4.9 dB | +5.7 dB | 0.2 dB（分不开） |

谱倾斜能分开，瞬态陡度不能 —— 所以脚步判据必须用**频段分布**，而非"多响才算瞬态"。

频段依据来自 13 590 条 Wwise 音频流的实测（`docs/01`）：环境音 47 % 能量在 20–125 Hz，脚步 125 Hz 以下为 0、主体 250 Hz–2 kHz。

---

## 4. 目录结构

```
src/       dsp.h  engine.cpp/.h  main.cpp
tests/     dsp_test.cpp overshoot_test.cpp        离线验收 + 限幅溢出审计
           audit_*.cpp                            DSP / 引擎 / 并发审计探针
tools/     pick_samples.js coverage.js fix_probe.cpp    素材分层与判据标定
           siggen.cpp lat_probe.cpp carve.py features.py
docs/      01 频段分析 · 02 技术设计 · 03 任务分解
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

## 7. 声明

只处理本机已输出的音频，不注入游戏进程、不读写游戏文件、不修改游戏内存。
PUBG 音频资源仅在本地离线分析，用于确定分频参数。
