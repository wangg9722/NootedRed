# 14.8.9 上解码守卫导致 VCN fence 不 retire（已定案）

## 现象（修复前，work1 / macOS 14.8.9 23J631，Cezanne 1002:1638）

- `16:32:23` 起内核日志出现 `channel 12 VCN0Dec is hung! (lastReadTimestamp=0x00000007)`，
  `HangFlags: FenceNotRetired 1, FullAsicReset 1`，**严格每 7.0 秒一次**，共 14 次挂死 → 28 次 `FullAsicReset`。
- 91 秒后桌面冻死：WindowServer 120 秒无 checkin，`watchdogd` 触发 panic
  （`Kernel-2026-11-21-163529.panic`：`userspace watchdog timeout: no successful checkins from WindowServer`），
  伴随 `Kernel_2026-11-21-1633xx_*.gpuRestart`。
- 触发者：`uTools Helper` 拉起 `com.apple.coremedia.videodecoder` XPC（VTDecoderXPCService），12 秒后首次挂死。

同一份 kext 在 26.4 / 26.7.3 上无此问题（4K HEVC 硬解正常、GFX 挂死 0）。

## 旧系统与 26 的关键差异

| 项目 | 26.x | 14.8.9 |
|---|---|---|
| 视频补丁数据来源 | 运行时动态解析真实 dyld 缓存（`NRedVCNUserDynamicApplied=1` / `DynamicMatched=1`） | 静态 legacy fixture（`UserLegacyData::video(major)`） |
| 解码守卫（`VADecoder::Init` 蹦床） | 安装并正常工作 | 安装后解码提交挂死 |

两系统的 VCN 引擎分配、SubmitGuard、显示侧补丁均一致且正常。

## 定位过程

先否掉的两个方向（都被实测否决，勿再走）：

1. **按系统版本关闭 VCN 路径**（`-NRedVideoLegacy` 门控，kext `92f21faf…`）：
   14 直接无法启动（VCN 不加载 → X6000 侧引擎分配拿到空指针）。**该方向永久放弃。**
2. 8K/`hev1` 超规格样本压测：`hev1` 容器在会话层就被 VT 拒绝（`-12906`），与驱动无关。

改为**分组二分**，新增四个只在 legacy 分支生效的开关（26 上零影响，一份 kext 通吃）：

| 开关 | 作用 |
|---|---|
| `-NRedDecodeGuardLegacy` | 旧系统**显式打开**解码守卫（默认关闭） |
| `-NRedNoVCNUserLegacy` | 旧系统关闭 VCN 用户态补丁组 |
| `-NRedNoDRMVideoLegacy` | 旧系统关闭 FairPlay 掩码补丁组 |
| `-NRedNoVCNEngine` | 关闭本地 VCN 引擎分配（用户态补丁随之停用，仅用于对照） |

第一轮就命中：14.8.9 上以 `-NRedNoDecodeGuardLegacy`（守卫不装、其余全部打开）启动后

- `NRedVCNEngineAllocated=1`、`NRedVCNUserApplied=16`、`NRedVCNDecoderPID=4058`、
  `NRedVCNX5000ABIValidated=1` / `X6000ABIValidated=1`、`SubmitGuardApplied=16` → 硬解路径完整；
- 4K HEVC（`hevc_4k60P_main10_10bit_420_eos_r5c_1.mp4`）在 QuickTime/IINA 播放，
  VTDecoderXPCService 正常建会话、正常退出；
- 12 分钟窗口内 `VCN0Dec is hung` **0 次**（此前是每 7 秒一次）、`FullAsicReset 1` 0 次、桌面无冻死。

结论：**解码守卫在 14.8.9 上是 VCN 挂死的直接原因**（守卫蹦床把 `VADecoder::Init` 入口改成绝对跳转，
14.8.9 的会话初始化状态机在被重放后进入异常路径，提交的 PM4 让 CP 永远等不到 fence），
与 VCN 用户态补丁、FairPlay 掩码、引擎分配无关。

## 修复

- 默认配置：守卫只在 26.x 安装；旧系统默认不装，需要复现时用 `-NRedDecodeGuardLegacy` 打开。
- 守卫本身保留（26 上仍用于拒绝宽 > 4096 / 高 > 2304 的超规格会话，8K 不再把 VCN 挂死）。
- 代码位置：`NootedRed/UserVideoDecode.cpp`
  - `init()`：`legacyGuard = checkKernelArgument("-NRedDecodeGuardLegacy")`，`decodeGuard = !-NRedNoDecodeGuard`；
  - `wrappedSharedRegionCheck()`：`if (decodeGuard && !(legacyProfile && !legacyGuard))` 才安装守卫。

## 回归验证清单

26.x（249 / work1 26.4）：`NRedDecodeGuardApplied>=1`、`OffsetIndex` 命中对应候选、4K HEVC 硬解、GFX 挂死 0。
14.8.9：`NRedDecodeGuard*` 属性应全部缺失（守卫未装），`VCNUserApplied=16`、播放 4K HEVC 无 `VCN0Dec is hung`。
