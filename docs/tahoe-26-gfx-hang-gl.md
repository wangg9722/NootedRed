# macOS 26.4：channel GFX 反复挂死 / 轻微花屏（legacy GL 未路由到 Metal）

## 现象（work1，macOS 26.4 / 25E246，Cezanne 1002:1638）

- 能进桌面，日常操作可用。
- Chrome 打开网页时出现**很小范围的画面异常（花屏块）**，随后整页明显卡顿，等 1~2 秒自动恢复。
- 偶发一次操作后系统卡一会儿，不重启也能自己缓过来。

## 日志证据

`log show --last 6h | grep -E "is hung|GPURestartBegin|PerEngineReset"`：

```
channel 24 GFX event timeout; pending stamps [3504..3569]
HW Channel 0 GFX is occupied by channel 24 stamp 3504
channel 24 GFX is hung! (lastReadTimestamp=0x00000daf)
IOAccelEventMachine2::restart_channel(): GPURestartBegin stampIdx=24 type=2
GPU HangState 0x0000000e, HangFlags 0x00000005:
  IndividualEngineHang 1, NonEngineBlockHang 0, FenceNotRetired 1,
  PerEngineReset 1, FullAsicReset 0
```

- 24 小时统计：`channel 24 GFX is hung` × **13**，全部是 GFX，**没有一条 VCN（解码）挂死**。
- 对照组 249（macOS 26.7.3，同一份 kext，boot-args 含 `-NRedGLMetal`）：24 小时 **0 次**。

## 排除项

- **不是解码**：解码守卫在位且生效（`NRedDecodeGuardRejected = 4`，说明 4 次超规格 HEVC 会话被拒、回退软解），VCN 通道零挂死。
- **不是 26.4 与 26.7.3 的版本差异**：两机 kext 二进制相同（md5 `72644996…`），唯一系统性差异是 work1 的 boot-args **缺 `-NRedGLMetal`**。

## 根因

work1 上 `IOGLBundleName` 仍是 `AMDRadeonX5000GLDriver`（legacy GL），而 249 已被 NRed 改成
`AppleMetalOpenGLRenderer`（`NRedGLMetalApplied = 2`）。

legacy GL 提交在 Cezanne 上会占住 HW Channel 0 GFX 且 fence 不退役 →
`checkGPUProgress` 判定超时 → 触发 `PerEngineReset`（只重置引擎、不重启整机）。
重置瞬间该通道上未完成的绘制丢失，表现为**小面积花屏**；重置 + 语义重建期间页面卡顿 1~2 秒，
之后通道恢复，所以"会自己缓过来"。

当前 work1 上仍映射 legacy GLDriver 的进程：`uTools`（Electron 的 GL 客户端），
与当初优酷挂死属同一类触发者。

## 处置

在 work1 的 `config.plist` boot-args 追加 `-NRedGLMetal`（已备份 `config.plist.bak-gfxhang-*`，
`plutil -lint` 通过），重启后生效。验证判据：

1. `NRedGLMetalApplied` > 0，`IOGLBundleName = AppleMetalOpenGLRenderer`；
2. `channel .* GFX is hung` 计数在 Chrome / Electron 正常使用下保持 0；
3. `NRedDecodeGuardOffsetIndex` 仍命中（26 系列为 0），解码守卫不受影响。

## 备注

- `PerEngineReset=1 / FullAsicReset=0` 是"能自己恢复"的关键，和 8K 解码挂死（`channel 12 VCN0Dec`，
  且会拖死 WindowServer）不是同一类故障。
- macOS 14 分区的"进桌面即卡死"是独立问题，见其他文档；不要与本次 GFX 挂死混为一谈。
