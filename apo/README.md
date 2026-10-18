# SoundLock APO（引擎内版本，v2.0）

把同一套 DSP 塞进 **Windows 音频引擎**（`audiodg.exe`）里跑，不再需要虚拟声卡。

| | v1（用户态，`dist/`） | v2（APO，`dist-apo/`） |
|---|---|---|
| 延迟 | 64 ms | **理论 1.5 ms**（只有限幅前视，无额外缓冲） |
| 依赖 | VB-Cable 虚拟声卡 | 无（但需要管理员安装 + 驱动签名） |
| 影响范围 | 只处理走虚拟声卡的那路音频 | **该端点上的所有声音** |
| 反作弊风险 | 无 | 安装未签名驱动可能被反作弊关注 |

两个版本互不覆盖：v1 的文件在 `dist/`，v2 的在 `dist-apo/`。

---

## 1. 目录

```
apo/
  src/apo_defs.h        APO 接口定义（不依赖 WDK）
  src/SoundLockApo.cpp  APO 实现：IAudioProcessingObject / ...RT / ...Configuration
  src/apo_ui.cpp        控制面板（写 HKLM\SOFTWARE\SoundLock）
  SoundLockApo.inf      驱动包 INF（Class=AudioProcessingObject）
dist-apo/
  SoundLockApo.dll      187 KB
  SoundLockApoCtl.exe   226 KB
  SoundLockApo.inf
  install_apo.bat / uninstall_apo.bat
```

## 2. 已验证（无需安装即可测）

`tests/apo_test.exe` 直接 `LoadLibrary` + `DllGetClassObject` + `CreateInstance` 拉起 DLL，
构造连接描述符走完整链路：

```
[T1] registration: SoundLock APO flags=0x17 v2.0   PASS   （INPLACE + 格式必须匹配）
[T2] reported latency: 1.50 ms
[T3] LockForProcess  float32/48k/2ch               PASS
[T4] limiter: peak 0.7079  ceiling 0.7079          PASS   （0 dBFS 输入被压到 -3 dBFS，零过冲）
[T5] non-float input -> passthrough: 0.500         PASS   （16bit/单声道自动旁路，不崩）
```

## 3. 安装（需要管理员）

1. 右键 `install_apo.bat` → **以管理员身份运行**
2. 重启

脚本做三件事：把 DLL 复制到 `System32` → 注册 COM 类 `{A5C6D1E0-...}` →
把 CLSID 写进当前默认播放端点的 `Enum\...\Fx\0`（`FxStreamEffectClsid` / `FxModeEffectClsid` / `FxEndpointEffectClsid`）。

卸载：右键 `uninstall_apo.bat` → 以管理员身份运行 → 重启。

## 4. 必须知道的限制

1. **签名**：Windows x64 要求驱动包带有效签名。本仓库**不含证书与 `.cat`**，
   所以 `pnputil /add-driver` 会被拒。两条路：
   - 用 WDK 的 `inf2cat` + 你自己的代码签名证书生成 `SoundLockApo.cat`，再 `bcdedit /set testsigning on` 后安装；
   - 或走微软 attestation 签名（正式发行）。
   仅靠注册表挂载（脚本的第 4 步）在部分系统上仍会被音频引擎拒绝加载未签名 APO。
2. **影响全局**：APO 挂在端点上，音乐、语音、系统提示音都会被处理。
   只想处理游戏的话请继续用 v1。
3. **参数生效时机**：APO 在 `LockForProcess`（音频流启动）时读一次配置，
   所以「保存参数」后需要**切换一次播放设备或重新开始播放**才生效。
4. 参数存在 `HKLM\SOFTWARE\SoundLock`，写注册表需要管理员，控制面板会自动提权。

## 5. 与 v1 共用同一套 DSP

`apo/src/SoundLockApo.cpp` 直接 `#include "../../src/dsp.h"`，
四段分频、瞬态门控、前视限幅的代码与 v1 完全一致，只是宿主从 WASAPI 线程换成了音频引擎。
格式不匹配（非 float32 / 非双声道）时**原样旁路**，不会破坏声音。

## 6. 构建

```bat
apo\build_apo.bat        :: 输出到 dist-apo\
g++ -std=c++20 -O2 -o tests\apo_test.exe tests\apo_test.cpp -lole32 -luuid
tests\apo_test.exe
```
