# 衍生参考项目（外部参考源）与跟踪表

本仓库是 NootedRed 的自研分支。下列第三方衍生仓库作为**只读参考源**：只对照实现思路与约束，不直接合并他们的代码，也不引入其发行包。

本地克隆统一放在 `<工作区>/GitResources/opensource/reference/NootedRed-<user>`；我方主参考另有 `NootedRed-vithalchowdary`。克隆/更新前先导出代理：

```bash
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890 all_proxy=socks5://127.0.0.1:7890
git -C reference/NootedRed-<user> fetch --all --tags
```

> 本机 macOS 无 `timeout` 命令；`git fetch` 大仓库可能长时间无输出，放后台跑再读输出。

## 仓库清单（快照 2026-11-23）

| 仓库 | 关系 | 默认分支 | 最近推送 | 快照 HEAD / 版本 |
| --- | --- | --- | --- | --- |
| [vithalchowdary](https://github.com/vithalchowdary/NootedRed) | 主参考：VCN1/Raven2 全链路、动态签名后端 | master | 2026-11-22 | `52158395` = `v1.3.1` + 8 提交 |
| [weachy](https://github.com/weachy/NootedRed) | 同一 1.3.x 线，更新最勤 | master | 2026-11-23 | `c89751f2`（bump `1.3.2`） |
| [steve12311](https://github.com/steve12311/NootedRed) | 0.9.x 预发布线，视频解码能力扩面 | master | 2026-11-18 | `098c9d8f`（VP9 Profile 2 硬解，`v0.9.0-pre-release.19`） |
| [Kingtous](https://github.com/Kingtous/NootedRed) | 0.9.x 线，用户态补丁策略 | master | 2026-10-25 | `27e75c84`（统一 FairPlay 客户端） |
| [xiaot-evo](https://github.com/xiaot-evo/NootedRed) | 0.8.x/0.9.x 线，独立 UserPatch 组件 | master | 2026-11-18 | `6d229431`（`v0.9.3` + `NootedRedUserPatch 0.3.2`） |
| （内部）`NootedRead` | 我方 VCN 骨架干净实现 | `nred/vcn-skeleton-clean` | — | `38f863f`；硬解未成，但启动路径从不挂死 |

## 跟踪表（按仓库）

| 仓库 | 已分析范围 | 采纳 | 否决 / 不采纳 | 待分析 |
| --- | --- | --- | --- | --- |
| vithalchowdary | `v1.3.1` 全量：显存分配器、音频触发、`allocPages` 下溢、GFXOFF、能力位时序；11-22 新增 8 提交 | VRAM 分配器两项（开关化）、音频触发点对齐（开关化）、`hasVCN0` 单源/延后（开关化）、`engineEnabled()` 写法 | GFXOFF 压制；私有共享缓存补丁器整段（后被上游移除）；全局页缓存按进程/时机跳过 | 编码链路（HEVC Main10、slice delta）尚未细看 |
| weachy | 仅元数据 | — | — | `1.3.1 → 1.3.2` 的 diff 未看 |
| steve12311 | 仅元数据 | — | — | VP9 Profile 2 硬解实现未看 |
| Kingtous | 仅元数据 | — | — | FairPlay 客户端统一 + 保留原生 sync 未看 |
| xiaot-evo | 仅元数据 | — | — | UserPatch 拆分与版本号策略未看 |
| NootedRead（内部） | `HANDOFF.md`、`X5000.cpp`、`e9a4f56` | 「引擎 init 成功后才 publish `hasVCN0`」；私有槽位塞 VCN 的做法（作为对照） | — | 其余未看 |

## 决策明细

状态取值：**已上线**（已在主线 tag）、**开关化**（实机未验证）、**待做**、**不采纳**。

| 条目 | 来源 | 决定 | 落地位置 | 状态 |
| --- | --- | --- | --- | --- |
| 引擎 init 成功才放行用户态解码补丁 | 内部 NootedRead `e9a4f56` + 登录期卡死定案 | 采纳 | `VCN::engineStarted()`、`UserVideoDecode.cpp` 钩子 | 已上线（`v0.9.1-20261123-login-guard`） |
| 登录期 `shared_region_check` 钩子并发闸门 | 同上（登录期多进程同时 exec） | 采纳 | 钩子内原子闸门 | 已上线（同上） |
| `hasVCN0` 只在引擎 init 成功后置位 | 参考 `engineEnabled()` + 内部 `e9a4f56` | 采纳 | `-NRedVCNAfterInit` | 开关化（默认关） |
| 显存分配五级回退 + `allocPages` 32 位下溢修复 | 参考 `X5000.cpp` | 采纳 | `-NRedVRAMAlloc` | 开关化（默认关） |
| 音频只在 `reportCapabilities_LinkInfo` 触发 | 参考 `X6000FB.cpp` | 采纳 | `-NRedAudioLinkInfo` | 开关化（默认关，符号可解性待实机确认） |
| `initHardware` 失败撤回能力位 + 清 `engineAllocated/loadCompleted` | 参考 11-22 master | 待做 | `VCNBridge::initHardware` | 待做 |
| 跨版本掩码动态签名替代按 build 硬编码偏移 | 参考 `ee4590a5`、`VDASignatures.inc` | 待做（我方 26.x 机器码失效的根因对策） | 用户态签名后端 | 待做 |
| 音频三处：`IOFBCurrentPixelClock` 判内屏在用、`extOtg` 哨兵、DTO 读 `DtoSource` | 参考 `2f952e12` | 待做 | `NRedAudio::configureAudio` | 待做 |
| AppleGFXHDA Widget 调错原函数（应用 `orgCreateAppleHDAWidget`） | 参考 | 待做（一行修复） | `AppleGFXHDA.cpp` | 待做 |
| 私有映射生命周期管理（per-map 表、`_vm_map_destroy` 失效、子进程 verifyOnly 复核） | 参考 `b6937493` 思路 | 待评估（只取思路，不搬代码） | 我方 `UserVideoDecode` / `UserPatchMemory` | 待做 |
| `patchSWSchedulerAdvance`、`CachedPatchSite` 移植 | 参考 | 待评估 | — | 待做 |
| 私有共享缓存补丁器整段（改各任务私有 COW 页） | 参考 `b6937493` | 不采纳 | — | 上游后续自行移除，回归页缓存 + 签名路线 |
| GFXOFF 压制 | 参考 v1.3.x | 不采纳 | — | 用户明确否决（激进手段） |
| 改全局页缓存时按进程身份或加速器启动时机跳过 | 参考注释（`_cs_validate_page` 每物理页只调一次） | 禁止 | — | 跳过即该页以未打补丁状态进全局 vm_object，本次开机补不上 |
| 取反 / 翻转语义、`legacyNoGuard` | 历史实验 | 不采纳 | — | 语义摆放过窄，已否决 |
| `NRedAudio::init()` 早期调用、自创参考实现没有的钩子 | 历史实验 | 不采纳 | — | 用户否决 |
| 带固定延时（`IOSleep(250)` 等）的猜测代码进主线 | 历史实验 | 不采纳（仅作探测手段保留在实验分支） | — | 用户否决 |
| MMIO 间接访问无锁、X6000 可选加载竞态、`doGPUPanic` 10s+panic、版本表/PANIC_COND | 继承项 | 降级不处理 | — | 参考实现同形，且旧分支已存在 |

## 更新日志

| 日期 | 仓库 | 变更 | 处置 |
| --- | --- | --- | --- |
| 2026-11-23 | vithalchowdary | `v1.3.1` 之后新增 8 提交（VCN1 动态签名重构、dyld 缓存 hash mismatch 修、音频 DTO 检查、HEVC Main10 编码、工具链自检） | 已归纳为 4 条待做/1 条不采纳，见上表 |
| 2026-11-23 | weachy / steve12311 / Kingtous / xiaot-evo | 首次建跟踪，建立全量克隆 | 待逐个分析 |

## 查阅原则

1. 某风险点若**旧分支就存在**且**参考项目也存在**，大概率不是问题源，优先级后置；只处理我方独有偏差。
2. 只取机制与约束，不取逐字机器码：他们的页偏移/立即数绑定具体 OS build，我方参数须按版本自适应。
3. 参考源只读；任何采纳都要先进实验分支、实机验证后再进主线并打 tag。
