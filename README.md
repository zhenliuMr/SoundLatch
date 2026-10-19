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

[![download](https://img.shields.io/badge/下载-SoundLatch.exe-0078D4?style=for-the-badge&logo=windows)](https://github.com/zhenliuMr/SoundLatch/releases/tag/v1.2.0)
[![vbcable](https://img.shields.io/badge/依赖-VB--Cable-BA7517?style=for-the-badge&logo=virtualbox)](https://vb-audio.com/Cable/)

</div>

---

Windows 实时音频处理，两个开关：**降低环境音 + 强化脚步声**、**锁定最大输出**。

- 单文件 `SoundLatch.exe`（412 KiB），无安装、无运行库依赖
- 启动后**静默进托盘**（右下角），不弹任何窗口；点托盘图标才展开面板——所有开关、滑杆、设备选择都在这一页，没有第二个窗口
- 开机自启动勾选后也是静默启动，直接开始处理音频
- 界面跟随系统浅色/深色主题，托盘图标自适应明暗
- 32 位浮点内部处理，48 kHz；音频线程 CPU **0.34 %**（单核），进程内存 **17.7 MB**
- 算法延迟 **0.67 ms**（一个控制块的前视），端到端延迟取决于声卡缓冲，见 §2

## 目录

<details>
<summary><b>Table of Contents</b></summary>

- [1. 快速开始](#1-快速开始)
  - [1.1 路由（必须做一次）](#11-路由必须做一次)
  - [1.2 两个开关](#12-两个开关)
  - [1.3 调参建议（PUBG）](#13-调参建议pubg)
- [2. 实测数据](#2-实测数据本机2026-10-19)
- [3. 原理](#3-原理)
- [4. 目录结构](#4-目录结构)
- [5. 重新编译](#5-重新编译)
- [6. 常见问题](#6-常见问题)
- [7. 声明](#7-声明)

</details>

---

## 1. 快速开始

### 1.1 路由（必须做一次）

```
PUBG  →  CABLE Input   （游戏内音频输出设备选它）
              │
        [ VB-Cable 虚拟声卡 ]
              │
        CABLE Output   （本程序采集）
              │
        SoundLatch.exe
              │
        你的耳机 / 音箱   （本程序输出）
```

1. 装 VB-Cable：官方下载 <https://vb-audio.com/Cable/>（约 1 MB，免费），右键 `install_vbcable.bat` → 以管理员身份运行 → 重启
2. 游戏里把「音频输出设备」设为 `CABLE Input`
3. 打开 `SoundLatch.exe`：采集选 `CABLE Output`，输出选你的耳机
4. 点「启动」

> 没有 VB-Cable 时可勾选「回环捕获」做无驱动测试：此时采集的是**当前默认输出**，
>   
> 请把「输出」选成**另一块设备**，否则会自己录自己。

### 1.2 两个开关

| 开关              | 作用                                         | 可调项                                                       |
| --------------- | ------------------------------------------ | --------------------------------------------------------- |
| ① 降低环境音 + 强化脚步声 | 4 段动态处理：20–150 / 150–600 / 600–3k / 3k–16k | 环境抑制 0–100%、脚步增强 0…+12 dB（判据阈值已实测固化，不开放） |
| ② 锁定最大输出        | 前视砖墙限幅：输出峰值**永不超过**所设上限，不超不动                     | 最大输出 0–100%（100% = 0 dBFS 不限制，1% = −36 dBFS，0% = 静音）；可直接在电平表上拖动 |

- 面板底部：采集/输出设备选择（每次打开面板自动重新枚举，保留当前选择）、回环捕获、**开机自启动**、📌 固定到屏幕、↻ 恢复默认
- 📌 固定：面板常驻最上层、失焦不收起，方便窗口化游戏时看电平/调参（再次点击或点托盘图标取消）
- 开机自启动写在 HKCU 的 `Software\Microsoft\Windows\CurrentVersion\Run` 下，状态以注册表为准（移动 exe 后会被自动更新路径）
- 参数改动**立即生效**，自动存到 `%APPDATA%\SoundLatch\config.ini`
- 底部电平表：渐变柱形显示系统实时输出电平，标尺 −36…0 dBFS（右端就是数字满刻度），直角槽；
  标尺左端与「最大输出」滑杆的 1% 对齐，拖到哪游标就落在哪；低于 −36 dBFS 的信号画 1px 短条
  游标就是一条 1px 分割线，只按开关换颜色：②开=琥珀色，②关=灰色
- 「最大输出」是**绝对上限**（输出峰值的天花板），不是"衰减多少 dB"：
  柱形的显示上限就是游标，超过的部分只画虚影——限幅开为红色（正在压），关为灰色（只是超了上限）
- 刻度：**100% = 0 dBFS（不限制），1% = −36 dBFS，0% = 静音**，每 1% ≈ 0.36 dB；
  滑杆与柱形同标尺：拖到最左 = 游标贴左、拖到最右 = 游标贴右；
  也可以**直接在柱形上按住左右拖动**来设上限（柱形与滑杆同标尺，最左一小段 = 静音）

### 1.3 调参建议（PUBG）

| 场景       | 建议                                         |
| -------- | ------------------------------------------ |
| 野外听脚步    | 环境抑制 60–75%，脚步 +8 dB |
| 城区/房区    | 环境抑制 40–55%（枪声多，别压太狠）          |
| 载具/飞机噪音大 | 环境抑制 85%，脚步 +10 dB             |
| 嫌枪声炸耳    | 开关②天花板 −6 ~ −9 dBFS                        |

---

## 2. 实测数据（本机，2026-10-19）

| 指标     | 数值           | 说明                                             |
| ------ | ------------ | ---------------------------------------------- |
| 算法延迟   | **0.67 ms**   | 1 个控制块（32 样本）的前视，脉冲法实测     |
| 链路延迟   | **63–79 ms** | 采 22 + 缓冲 25 + 放 22（本机端点各 22 ms，硬件下限） |
| 端点缓冲   | 22 ms / 端    | 探测结果：所有端点固定 1056 帧（`tools/lat_probe.exe`）      |
| 欠载     | ~2–3 次/秒     | 见[常见问题](#6-常见问题)，源于虚拟声卡内部采样率 96 kHz       |
| DSP CPU | 0.34 %（单核）  | 20 s 音频处理耗时 69 ms；含 UI 的进程 CPU 2–3 %          |
| 内存     | 17.7 MB      | 工作集                                            |

> 本机两个端点的缓冲区被驱动固定为 22 ms（直连 Realtek 也一样），所以下限就是 44 ms。
>   
> **普通消费级主板/耳机的共享模式缓冲一般是 10 ms**，同样代码在那类机器上约 **25–30 ms**。
>   
> 想再低只有两条路：独占模式（本机驱动不支持 float 独占），或换用原生支持低延迟的声卡。

DSP 离线验收（`tests/dsp_test.exe`，12 项全 PASS）：

```
[T1] 天花板 -6dB -> 总峰值 0.5012 (ceil 0.5012) PASS          零过冲
[T2] 分频平坦度 @1k: 0.00 dB PASS                            4 段求和
[T3] 低频独大时 60Hz -19.3 dB / 1kHz -0.6 dB PASS环境压、脚步带不动
[T3b] 谱倾斜判据 5 个用例 PASS                20dB/16dB/12dB 独大→压；等幅/中频更大→不压
[T4] 瞬态增强: +6.4 dB PASS                                  脚步类瞬态被抬起
[T6] 1kHz 脚步带在爆炸期间变化 -5.1 dB / 爆炸后 -0.1 dB PASS  限幅不误伤脚步带
     总峰值 0.5012 = 天花板 PASS
[T5] 20 s 音频处理耗时 69 ms -> 0.34% of one core
```

限幅溢出专项审计（`tests/overshoot_test.exe`）：6 组恶劣场景全部 **+0.00 dB**，
含块内相位 0–31 全扫描、回调块 480/512/1024/2048、块级方波、静音档、A/B 全开。

分频器平坦度：20 Hz–20 kHz 最大起伏 −0.173 dB @21.4 Hz。

参数跨线程一致性：800 万次参数快照，**0 撕裂**。

---

## 3. 原理

```
输入 → 4 段 Linkwitz-Riley 分频(150/600/3000Hz，全通相位补偿)
     → 判据①谱倾斜：低频相对中高频独大 → 压 band0/band1（环境隆隆）
     → 判据②瞬态频段分布：中频比低频更陡 → 抬 band2（脚步主体）
     → 整条信号一个增益的砖墙限幅：超过"最大输出"的部分压到这条线
     → 求和 → 输出      1 个控制块 ≈0.67 ms 前视
```

两个判据都与**绝对电平无关**，阈值由实测固化在 `src/dsp.h` 的 `namespace judge`，
面板上不暴露滑杆。依据是对素材库 189 条三维分层样本的实测：

| 判据   | 低频环境 | 脚步带250–2k | 枪声带 2k–5k | 分离度 |
| ------ | ------- | ----------- | ----------- | ----- |
| 谱倾斜 tilt | **+16.6 dB** | **+2.1 dB** | −13.2 dB | 14 dB  |
| 瞬态陡度 P90 | +5.9 dB  | +4.9 dB  | +5.7 dB   | 0.2 dB（分不开）|

谱倾斜能分开，瞬态陡度不能 —— 所以脚步判据必须用**频段分布**（中频比低频更陡），
而不是"多响才算瞬态"。

频段依据来自对游戏内 13 590 条 Wwise 音频流的实测（`docs/01`）：
环境音~47 % 能量在 20–125 Hz，脚步瞬态 125 Hz 以下为 0、主体 250 Hz–2 kHz。

---

## 4. 目录结构

```
dist/           SoundLatch.exe  SigGen.exe(测试信号) install_vbcable.bat
src/            dsp.h  engine.cpp/.h  main.cpp
tests/          dsp_test.cpp overshoot_test.cpp      离线验收 + 限幅溢出审计
                audit_*.cpp                多agent 审计探针（DSP / 引擎 / 并发）
tools/          pick_samples.js coverage.js fix_probe.cpp   素材分层与判据标定
                siggen.cpp lat_probe.cpp devfmt.cpp carve.py features.py
docs/           01 频段分析 / 02 技术设计 / 03 任务分解
                assets/logo.png           README 用的图标（从 res/app.ico 提取）
build.bat       一键编译（MinGW-w64，已装在 D:\toolchain\mingw64）
```

## 5. 重新编译

```bat
build.bat                       :: 需要 D:\toolchain\mingw64\bin\g++.exe
g++ -std=c++20 -O2 -o tests\dsp_test.exe tests\dsp_test.cpp && tests\dsp_test.exe
```

## 6. 常见问题

| 现象      | 处理                                                                                                                               |
| ------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 点了启动没声音 | 采集/输出选反了；采集必须是 `CABLE Output`（录音设备），输出是耳机                                                                                        |
| 游戏里没声音  | 游戏输出设成了 `CABLE Input`，但本程序没启动                                                                                                    |
| 有回声/啸叫  | 勾了「回环捕获」又把输出选成了正在采集的那块设备                                                                                                         |
| 爆音、卡顿   | 提高「环形缓冲」不可调时，先关掉其它占用音频的设备；蓝牙耳机不要用                                                                                                |
| 延迟显示很高  | 换有线耳机/独立声卡；蓝牙链路自带 100–200 ms                                                                                                     |
| 偶发轻微咔哒声 | 打开 `dist/vbcable/VBCABLE_ControlPanel.exe`，把 **Internal Sample Rate 改成 48000** 并重启系统；本机出厂是 96000，与 48 kHz 端点不匹配，会逼着引擎做 ±4 % 的重采样 |
| 脚步提升不明显 | 提高「脚步增强」；若环境音没压住，再加「环境抑制」                                                                                                       |

## 7. 声明

只处理本机已输出的音频，不注入游戏进程、不读写游戏文件、不修改任何游戏内存。
  
PUBG 音频资源仅在本机做离线分析（提取 → 解码 → 统计），用于确定分频参数。
