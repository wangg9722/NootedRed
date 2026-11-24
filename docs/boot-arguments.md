# 修复功能启动参数

本版只使用以下入口，不兼容此前的修复启动参数。

| 参数 | 功能 | 省略时 |
| --- | --- | --- |
| `-NRedDisplayFix` | 花屏／卡顿修复，包含 SubmitGuard 与 LegacyBlend 门修复 | 显示修复全部关闭 |
| `-NRedSubmitGuard` | 只启用提交守卫（命令缓冲空闲尾填 PM4 filler），不触发表面补丁集 | 单独不启用 |
| `-NRedSubmitGuardSig` | 提交守卫改用驱动映像解析定位（`AMDRadeonX5000MTLDriver` + 映像内偏移 `0x99240`），解析或核验失败回退候选表 | 只用候选表 |
| `-NRedNoSubmitGuard` | 只关提交守卫，显示修复不受影响 | 守卫随 `-NRedDisplayFix` 启用 |
| `-NRedVRAMAlloc` | APU 显存分配五级碎片回退（`AMDHWMemory::allocateLargeBlocks`） | 关 |
| `-NRedAllocPagesUnderflow` | `allocPages` 32 位空洞下溢修，与上面的回退互不依赖 | 关 |
| `-NRedVCNAfterInit` | VCN0 能力位只在引擎真正 init 成功后暴露，引擎槽位先判空再写 | 关（沿用 `VCN::ready()` 语义） |
| `-NRedScratch` | 在 26 上额外启用 SrdShared 与 ComputeScratch（26.0 形状机器码，语义未在 26.7.x 验证过） | 保持关闭 |
| `-NRedVideoFix` | VCN 硬件视频加速，包含现有解码和编码路径 | VCN 桥接关闭 |
| `NRedECFix=1` | 使用固件报告的 EC 背光来源 | EC 服务关闭 |
| `-NRedGLMetal` | 全局 GL 渲染器路由：`IOGLBundleName` 由 `AMDRadeonX5000GLDriver` 改为 `AppleMetalOpenGLRenderer`（GL-on-Metal），修旧 Electron 类应用的 GFX 挂死 | 保持 legacy GLDriver |

三项均启用的启动参数：

```text
-NRedDisplayFix -NRedVideoFix NRedECFix=1 -NRedGLMetal
```

只有需要主动切换固件控制来源时，才把 `NRedECFix=1` 改为 `NRedECFix=2`。
`=2` 沿用强制模式，仍须核验固件调用结果和报告来源；本机固件的来源读取固定返回 2，
不能仅凭该值证明控制权切换。`=0`、其他无效值或省略参数均不启动 EC 服务。

显示开关在 macOS 14/15 启用 LegacyBlend 和 SubmitGuard；在 26 启用提交守卫与 LegacyBlend，
提交守卫另有 `-NRedSubmitGuard`（只开守卫，不动显示修复）与 `-NRedNoSubmitGuard`（只关守卫）两个独立入口，
SrdShared、ComputeScratch 需额外加 `-NRedScratch` 才启用（其数据是 26.0 形状，26.7.x 语义未验证），
保留原生同步策略，不再启用 ImmediateSync。
26 的 LegacyBlend 是两条协同写入：放行能力位门（`74 28` → `90 90`）并把 `sete cl` 改为
`xor ecx,ecx`（`0F 94 C1` → `31 C9 90`），结果只对双源混合禁用双 quad。
`NRedDisplayFixVersion=2` 标记这个组合；`NRedImmediateSync*` 继续作为历史页面事务诊断名称，
`NRedImmediateSyncDisabled=1` 表示立即同步修复未启用。
**本版不含 HDRIntegerFilter 候选的运行代码与补丁数据。**
设备、缓存、完整代码和 ABI 核验仍然生效，具体版本边界见
[多系统适配](macos-multi-os-compat.md)。

显示、视频和 EC 三项可独立启用，关闭某一项只需移除对应参数并重启。
插件全局关闭 `-NRedOff`、调试参数及上游的 `AMDBacklight` 设置不在本次更名范围内；
`AMDBacklight=0` 仍会阻止 EC 服务启动。

以下旧修复参数应从配置中移除，本版不会读取：

```text
-NRedImmediateSync -NRedSrdShared -NRedLegacyBlend -NRedComputeScratch
-NRedTransfer1x1 -NRedVCN -NRedHDRIntegerFilter
NRedECBacklight=1 NRedECBacklight=2
```

诊断属性仍沿用模块名，例如 `NRedVCN*`、`NRedComputeScratch*`、`NRedECBacklight` 服务。
这些名称是运行状态，不是旧参数兼容入口。
`Enabled` 仅表示已注册；确认实际补丁写入还需检查 `Applied`／`WindowServerApplied`。

## FairPlay 客户端兼容

FairPlay 兼容随 `-NRedVideoFix` 默认启用，在 macOS 14/15/26 / Darwin 23/24/25 的
受支持 VCN 2.2 APU 上生效，不再需要 `-NRedDRMVideo`。所有持有 `com.apple.private.fpsd.client=true`
权限的进程统一应用 AMD 驱动侧解码子集及内容保护工厂 PSP 适配，不再按 Music／TV 等进程名区分；
AppleGVA 与编码代码保持原样。14/15 按各自完整原始页面核验，选择 13 项 AMD 解码补丁；
26 保留 18 项及原有动态匹配。三个系统采用相同策略，但补丁条目随已核实的版本数据不同。
`NRedDRMVideoRevision=4` 标识三系统统一权限策略，不代表 DRM 视频已修复。
既有 Music 实测为普通音乐正常、受保护 MV 有声且进度推进，但视频仍黑屏；统一版本的
Music／TV 实机回归尚待验证。范围、诊断和验证步骤见 [FairPlay PSP 兼容说明](fairplay-psp-compatibility.md)。
