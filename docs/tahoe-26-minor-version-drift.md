# macOS 26 小版本差异：实测数据与判据补充

本文补充 `version-selection-matrix.md` 未覆盖的两类信息：**各补丁族在不同 26.x 小版本上的实际差异量**，
以及 26 独有故障（关闭 4K 预览约 2 秒卡顿）的排查结论。所有数据来自只读核验，未修改系统缓存、未改 EFI。

## 一、26 路径的判据回顾

26.x 不走版本号，走**代码内容**：

1. 静态绝对地址表只在缓存 UUID 与 26.0 参考缓存（`17475689-B434-E8A3-4F56-C5D57216A1`）完全一致时启用
   （`UserSurfaceSync.cpp` 的 `dynamic = !legacyProfile && memcmp(uuid, CacheUUID) != 0`）。
2. 其余 26.x 一律由 `UserCacheResolver::resolve()` 用整段函数字节匹配定位，写入位置由“函数内偏移”派生。
3. 大函数允许差异预算：第一档 96 处，第二档 256 处（要求门/allow 两个锚点字节完全一致且全文唯一命中）。
4. 14/15 走 `UserLegacyCompat::matches()`：UUID + 最多 16 页、每页 4096 字节逐字节核验，任何一字节不同即整组拒绝。

## 二、各补丁族在 26.x 上的差异量

`python3 tools/verify_surface_sync_bytes.py` 在目标系统直接与运行中的缓存比对（rel32 位位移豁免）。

| 补丁点 | 长度 | 26.7.3（25G342） | 26.4（25E246） |
|---|---|---|---|
| Init(`Original0`) | 215 | 完全一致 | 完全一致 |
| Override(`Original1`) | 10 | 完全一致 | 完全一致 |
| NativeOverride | 923 | 完全一致 | 完全一致 |
| BlendConstructor | 8766 | 57 处差异（96/256 预算内，门锚点 `74 28`、`0F 94 C1` 一致） | **116 处差异**（第一档 96 不够，靠第二档 256 放行） |
| Scratch | 452 | 14 处差异，**前 32 字节即不同** → `-NRedScratch` 拒绝 | 同量级，同样拒绝 |

结论：

- 26.4 是当前已知**漂移最大**的小版本（116 处 > 26.7.3 的 57 处），它是唯一必须依赖第二档预算放行的版本。
- Init / Override / NativeOverride 三族跨小版本稳定，不需要额外数据。
- **Scratch 是唯一没有小版本容忍度的补丁族**：452 字节载荷取自 26.0 参考缓存，前 32 字节已不同，
  因此 26.x 上任何小版本都装不上；要支持 26.4/26.7 只能从各自缓存重新生成载荷，不能靠放宽预算。

## 三、26 独有的 compute 挂死（关闭 4K 预览约 2 秒卡顿）

现象：26.4 上硬解正常、播放正常、登录不挂；关闭 4K 预览时出现约 2 秒卡顿。14.8.9 上不复现。

`.gpuRestart` 报告（`/Library/Logs/DiagnosticReports`）证据：

- 重启通道是 compute 队列（`computeUQ1` / `computeUQ4`），`VCN0Dec` 的 `CompletedTS == SubmittedTS`，解码引擎干净。
- 挂住的命令缓冲来自系统进程：`mediaanalysisd` 的 `VTMTSComputeFunction`、`iconservicesagen` 的
  `ci_clampRect_premul_srgb_to_lin`，均为系统级色彩转换／图像分析算核。
- 内核日志时序：`IOAccelEventMachine2::waitForStamp: initial wait for 1 second expired. Continue wait for 4 seconds`
  → `Timestamp 0x6 on channel 7 computeUQ4 is not expired … wait time 1784ms` → `is hung!` →
  `HangState 0x0000000e, HangFlags 0x00000005: IndividualEngineHang 1, FenceNotRetired 1`。
  **用户感知的“2 秒”即这一段挂死判定 + 通道重启的时间，不是补丁安装阻塞。**

已排除项：

- 解码引擎：VCN0Dec 时间戳平齐；解码本身正常。
- SubmitGuard：`-NRedNoSubmitGuard` 对照仍挂。
- Compute 能力整体：自建 Metal 探针（buffer 运算、4096×2160 BGRA IOSurface 采样、CoreImage GPU 4K→1080 缩放）全部正常。
- GFXOFF 复位：该钩子在本仓库已安装并确认命中（参考项目 `a9ac3fd` 的等价实现），仍挂。

仍成立但未定论：

- 14 与 26 跑的是同一份 kext（同一钩子、同一 VCN 路径），14 不复现 → 差异在 26 的用户态 Metal 驱动路径，
  而不是内核侧钩子。
- 26 上真正缺失的补丁族是 Scratch（见第二节）；但 14 上 Scratch 同样未安装（`computeScratch` 需 26 且显式开关），
  因此 Scratch 缺失不能单独解释 14/26 差异，只能作为 26 端可补齐的候选。
- 26 的 BlendConstructor 在 26.4 上有 116 处差异却仍被放行，是当前 26 路径**最大的容忍度缺口**，
  需用 `tools/blend_gate_probe.py` 在 26.4 上确认门形态与跳转目标再下结论。

## 四、装机前的只读核验清单（不修改系统、不改 EFI）

```sh
python3 tools/verify_surface_sync_bytes.py        # 各补丁族差异量与拒绝点
python3 tools/blend_gate_probe.py                 # 门形态 / 跳转目标 / setne 字节
python3 tools/test_compute_vtable.py              # 计算提交虚表槽位与 ABI
```

在 work1（26.4）上跑前两条即可回答“26.4 是放行还是拒装”，这是后续决定“重生成数据”还是“改匹配策略”的前置条件。

## 五、维护原则（沿用并强化 `version-selection-matrix.md`）

1. 26 侧新增补丁必须写成“函数 + 函数内偏移”进入 resolver，不得新增绝对地址。
2. 每条补丁族的形状校验只在该族开启时执行，不得当作全局前置（历史 bug：Scratch 形状不符导致
   `DynReason=32`，连带 LegacyBlend 一条都没打上）。
3. 差异预算只是容忍，不是适配：预算内放行 ≠ 语义一致；小版本变化后必须用探针工具复核锚点，
   不能只看“装上了”。
