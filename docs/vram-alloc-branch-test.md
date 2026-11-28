# 显存分配回退测试分支（nred/vram-alloc-fallback）

## 分支信息

| 项 | 值 |
| --- | --- |
| 基线 | `master` @ `0e256c2`（= `personal/master`，已含 `v0.9.3-20261126-audio`） |
| 分支 | `nred/vram-alloc-fallback` |
| 提交 | `3abd372`（cherry-pick 自 `personal/dev-v2` 的 `50b07e3`）、`0bd3723`（剔除同提交的 VCN 能力门） |
| 改动面 | 仅 `NootedRed/X5000.cpp`(+197) 与 `X5000.hpp`(+7)，相对 master 零删除 |
| 默认行为 | 与 master 完全一致：两个开关都是 `checkKernelArgument` 门控，不加 boot-arg 不进任何新代码 |

本分支相对原提交 `50b07e3` 的差异：**移除了“`hasVCN0` 只在引擎落地后才暴露”这一项**。原因是它没有开关、直接改变 VCN 能力暴露时序，会和显存试验混成两个变量；该项另案处理。

## 两个开关

| 开关 | 做的事 | 落属性（用于确认真的生效） |
| --- | --- | --- |
| `-NRedAllocPagesUnderflow` | `IOAccelMemoryAllocator2::allocPages` 空洞对齐起点越过 `holeEnd` 导致 32 位下溢、整段非连续分配失败；把该处 `je` 改 `jle` 跳过装不下的洞 | `NRedAllocPagesUnderflowEnabled=1`、命中时 `NRedAllocPagesPatched=1` |
| `-NRedVRAMAlloc` | 钩 `AMDHWMemory::allocateLargeBlocks`，改五级回退（非可见域连续 → 可见域连续 → 非可见域非连续 → 全 UMA 域 → 16K/4K 细粒度），入口先用 `totalFreeMemory` 快速失败避免刷日志 | `NRedVRAMAllocEnabled=1` |

两者互不依赖，可单独开。

## 测试顺序（249 纪律：一次一个变量）

1. **R0 基线**：本分支构建，**不加任何开关**。判据：行为与 master 一致，`ioreg` 中两个 `*Enabled` 属性均不存在。
2. **R1**：只加 `-NRedAllocPagesUnderflow`。
   - 主判据：`Failed to allocate size:*` 日志归零（事故现场是 1 秒 58 条）。
   - 副判据：`NRedAllocPagesPatched=1`（否则说明特征码没命中，看 `SYSLOG("X5000", ...)` 落到哪一支）。
3. **R2**：R1 有效后再叠加 `-NRedVRAMAlloc`。
   - 主判据：`gpuRestart` 报告数 0；`shutdown_stall` 0。
   - 副判据：登录期 `orphanedReusableVidMemoryBytes` 不再长期挂在 1.5 GB 量级。

判据取数命令见 `docs/vcn0dec-stall-2026-11-27.md` §7。

## 回退

两个开关都是 boot-arg 级回退：删参数即可回到 master 行为，不需要换 kext。分支本身不推送上远端，验证通过后再决定合入哪条线。

## 未验证项（构建前必须知道）

- 本机无 Xcode（只有 CommandLineTools），`xcodebuild` 不可用，因此这次移植**没有经过编译验证**；语法级检查被 Lilu 头文件的 kernel 原子接口挡住（环境问题，非本次改动）。
- `AMDHWMemory::allocateLargeBlocks / totalFreeMemory / allocate / allocateNonContiguous` 的符号解析是运行期 `solveSymbol` + `clearError()` 兜底，符号缺失只会跳过并打 `SYSLOG`，不会 panic。
- `allocPages` 是原地改内核代码字节（`MachInfo::setKernelWriting`），改前有 prologue 四字节与 16 字节特征串双重校验，不匹配即跳过。
- 需要在 249 / 26 卷上构建后再谈“可用”。
