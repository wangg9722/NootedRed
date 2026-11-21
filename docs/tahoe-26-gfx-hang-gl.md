# macOS 26.4：channel GFX 反复挂死 / 轻微花屏（显示修复未安装）

## 现象（work1，macOS 26.4 / 25E246，Cezanne `1002:1638`）

- 能进桌面、日常可用；Chrome 打开网页时出现很小范围的花屏块，随后页面卡 1~2 秒自行恢复。
- 每次卡顿都对应一次 `PerEngineReset`。

## 一开始的错误判断（保留作为教训）

work1 的 boot-args 缺 `-NRedGLMetal`，而 249 有，24h 挂死数 0 vs 13，于是先按
[GL 路由](gl-metal-routing.md)处理。补上 `-NRedGLMetal` 后（`NRedGLMetalApplied=2`、
`IOGLBundleName=AppleMetalOpenGLRenderer`）**挂死没有减少**：WebGL 压测 60 秒仍 12 次。
说明本条故障与 legacy GL 无关；对照组差异是**系统小版本**（26.7.3 vs 26.4），不是参数。

## 根因

`-NRedDisplayFix` 的显示修复（`ImmediateSync` 的设备设置初始化改写 + `LegacyBlend` 门）
在 26 上走"缓存 UUID 已知"或"动态解析"两条路。26.4 的缓存 UUID
（`3291C199-4BF9-3866-AB8D-71C8CA1CA31A`）与参考版本不同，进入动态解析；
解析器在 `BlendConstructor` 一步失败（`NRedImmediateSyncDynReason=33`），
而它是全有或全无的，于是**四个补丁点一个都没写**：

| 属性 | 249 (26.7.3) | work1 修复前 |
| --- | --- | --- |
| `NRedLegacyBlendApplied` | 16 | 缺失 |
| `NRedLegacyBlendWindowServerApplied` | 1 | 缺失 |
| `NRedImmediateSyncWindowServerApplied` | 1 | 缺失 |

失败的直接原因是差异预算：参考构造函数 8766 字节，26.4 上"计数差异" **116 处**，
超过 resolver 的 96 预算。差异不是语义变化，而是编译器把一批 `REX` 前缀换成了等价的
`VEX` 前缀（例如 `44 fa 30 31` ↔ `c4 fa 30 31`），指令语义相同。
门本身在 26.4 上位置与字节都正常：`ctor+0x101A` = `74 28`、`ctor+0x103C` = `0F 94 C1`
（`tools/blend_gate_probe.py` 实测），镜像布局整体只平移了 `0x156000`。

## 修复（同一份 kext 支持多个小版本）

`NootedRed/UserSurfaceSyncResolver.hpp` 增加第二档匹配 `findConstructorWide()`：

1. 仍然要求前 32 字节严格一致（仅豁免位移字节）；
2. 差异预算放宽到 256（覆盖 26.4 的 116）；
3. **额外要求** `ctor+BlendGateInConstructor` 必须等于 `BlendGateOriginal`（`74 28`）、
   `ctor+BlendAllowInConstructor` 必须等于 `BlendAllowOriginal`（`0F 94 C1`）；
4. 仍要求全文本唯一命中。

先按原预算 96 匹配，失败才走这条回退，因此 26.7.x 的路径与代价不变；命中时置
`NRedImmediateSyncDynRelaxed=1` 作为现场证据。写入阶段原有的逐点 original 字节比对
（不符即 `CodeMismatch` 中止）保持不变，误匹配不会被写进进程。

## 实测（work1，macOS 26.4，重启 + `-NRedDisplayFix -NRedVideoFix -NRedGLMetal`）

| 项 | 结果 |
| --- | --- |
| `NRedLegacyBlendApplied` / `WindowServerApplied` | 16 / 1（WindowServer pid 197） |
| `NRedImmediateSyncWindowServerApplied` | 1 |
| `NRedImmediateSyncDynRelaxed` | 1 |
| 开机至今 `GFX is hung` | 0 |
| Chrome WebGL（aquarium）压测 60 秒 | 挂死 0（修复前同脚本 12 次） |

## 仍然存在、未处理

- SubmitGuard 未安装（work1 与 249 都没有 `NRedSubmitGuardApplied`）：
  `UserSubmitGuardData::MethodOffset`（`0x310CE7240`）在当前缓存里已不对应
  `MethodOriginal` 的 15 字节，`tools/build_submit_guard.py` 在 26.4 上直接拒绝生成。
  这台机器上的实际修复来自 `LegacyBlend` + 设备设置改写，SubmitGuard 属独立缺口。
- macOS 14 分区"进桌面即卡死"是另一个问题，见 `version-selection-matrix.md`。
