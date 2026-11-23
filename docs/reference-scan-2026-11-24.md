# 非官方提交扫描与初步评估（2026-11-24）

## 口径

- **官方基准**：`ChefKissInc/NootedRed` master（590 提交，HEAD `9ff85a6`，最后推送 2026-10-10），本地克隆 `reference/NootedRed-official`。
- **非官方提交**：各衍生仓库 master 上、不在 `official/master` 里的提交。按**功能点**归组（一个功能点可能跨多条提交，含后续修正/回退）。
- **去重并集 286 条**：vithalchowdary 133、weachy 144、steve12311 93、Kingtous 15、xiaot-evo 49（fork 嵌套，重叠已去重）。全量清单见文末附录。
- **评估四档**：推荐吸收 / 需要确认 / 不考虑 / 已在用（复核既有实现，维持现状），每条给原因与前提。
- **评估依据**：本机 Renoir（`0x15E7`）、macOS 14.8.9（Sonoma）、我方稳定线 `v0.9.1`（`7503ebf`）+ 三项开关分支 `feat/optional-switches`；以及历史已定案结论（GFXOFF 否决、26.x dual-quad NOP 致 FenceNotRetired、SubmitGuard 路径卡死、固定延时猜测代码不进主线）。

## 结论摘要

- 推荐吸收 **14 项**（P0 5 / P1 6 / P2 3）
- 需要确认 **8 项**（含 1 项「暂不迁移，记复评」）
- 不考虑 **6 项**
- 已在用 **2 项**（SubmitGuard-v1、命令缓冲尾部填充，属同一机制，复核后维持）
- 此前已吸收的（本轮复核维持既有决定）：显存分配器两项、`hasVCN0` 延后暴露、音频触发点对齐（均开关化）；`engineStarted` 门 + 登录期并发闸门（已上线 `v0.9.1`）。

## 逐项评估

| # | 功能点 | 代表提交 | 结论 | 备注（原因 / 前提） |
| --- | --- | --- | --- | --- |
| 1 | 补丁页权限恢复必须严格判定 | `7a81e78b`、`be32de04` | 推荐吸收（P0） | 我方历史上把 `vm_protect` 失败当容错放过，会留下「半打补丁」页；参考把 maximum 权限恢复失败直接判失败并逐页校验。改动小、收益直接。 |
| 2 | 钩子内栈压力受控 | `5ef479ae` | 推荐吸收（P0） | 与双 fault 教训同源：他们给出 Release 可见栈界约 4.2 KB 的可测约束并配套测试脚本；我方 `UserVideoDecode`/`UserSurfaceSync` 两个钩子应加同样的预算核算。 |
| 3 | 早期 VFS 读取移出 boot path | `b36ab686`、`f7e64872` | 推荐吸收（P0） | 早期启动阶段读系统文件会撞 VFS/缺页重入（他们因此删掉 `CompatTarget` 的 boot 期身份读取）。我方任何 boot 期读 dyld 缓存的动作都要延后到首个缓存验证之后，或放安全内核线程。 |
| 4 | 引擎分配前置条件 | `b8ed8540` | 推荐吸收（P0） | 缺 PM4 或 SDMA0 时引擎分配必须失败，而不是「分配出来但半可用」；与我们「能力位不得虚报」同一条纪律。 |
| 5 | SWScheduler 防页故障 | `c9467c96`（仅此单点） | 推荐吸收（P0） | 同一 SWScheduler 路径上的页故障防护，非 GFXOFF 类激进手段，可与显存优化开关并行验证。 |
| 6 | 跨版本掩码签名 + 数据节重定位 | `ee4590a5`、`4e8e5e22`、`572bc29a` | 推荐吸收（P1） | 用掩码吸收编译器差异（macOS 13 与 14–26 的寄存器准备指令），并支持按段名/节内偏移解析被重排的数据节。正对我方「按 build 写死偏移/预生成机器码」失效的根因。 |
| 7 | 按地址空间串行化替换全局闸门 | `6fb1aa6d` | 推荐吸收（P1） | 我方现在是全局单闸门，登录期所有新进程串行；参考改为有界 `vm_map` 注册表：跨进程并发、同地址空间互斥、等待者唤醒，并公布钩子栈界。收益明确，但要重验登录时序。 |
| 8 | AGFXHDA 已路由符号跳过 | `37813495` | 推荐吸收（P1） | 防双路由冲突，正好罩住我方「Widget 分支调错原函数」同域问题。 |
| 9 | 音频三处判据修正 | `2f952e12` | 推荐吸收（P1） | 内屏是否在用按 `IOFBCurrentPixelClock != 0`、`extOtg` 用哨兵并失败返回、DTO 复用需读 `DtoSource`。这三点解释「插拔/换分辨率后短暂无声」，可并入 `-NRedAudioLinkInfo` 实验。 |
| 10 | FairPlay 判据细化 + WindowServer 隔离 | `f7d36997`、`27e75c84`、`efef7e3a` | 推荐吸收（P1） | 由 entitlement 判定客户端、并让 WindowServer 不参与 VDA 补丁，比 PID/任务名判据稳，且缩小登录期风险面。 |
| 11 | 任务销毁 drain 不再阻塞 | `2bd1b74e`、`c9a71268` | 推荐吸收（P1） | 阻塞式 drain 是周期性卡顿的已知来源；改为自适应/异步后再回收资源。 |
| 12 | GL 资源预算补丁 | `7c690b88` | 推荐吸收（P2） | 纯用户态常量补丁 + 状态属性，风险低。 |
| 13 | OpenCL workgroup 收敛 | `8e32f464` | 推荐吸收（P2） | scratch kernel 的 workgroup 几何与 UBC 驱逐缓存，属计算侧兼容，不影响登录路径。 |
| 14 | 补丁缓存复用与进程过滤（性能） | `92af8d90`、`a1b890f2`、`e7ecf909`、`84e820d7` | 推荐吸收（P2） | 「完整镜像表读取成功后才缓存缺失结果」+ 引用计数证明快照 + 每 boot 拒绝记录，可减少冷扫描与误打补丁；放性能阶段做。 |
| 15 | 构建判定放宽到 minor series | `d63f796a`、`3038b08f` | 需要确认 | 方向对（23J/24H 这类系列匹配，免得一次系统更新全失效），但须先核实本机 14.8.9 的 build 前缀，否则放宽会错配。 |
| 16 | VCN 编码链路 | `1983f326`、`dcbd2895`、`5a941329`、`33c5f51f`、`72392609` | 需要确认 | 我方未开编码测试路径；先确认 `0x15E7` 的编码能力边界（是否只做 AVC）、以及 level/10-bit 限制是否安全。 |
| 17 | 解码桥扩大 APU/OS 面 | `3a8a41a1`、`142cad95`、`56f0e3a8`、`29377253` | 需要确认 | 参考用 DeviceProfile 门控后放开；我们不能无条件放开，必须先给 `0x15E7` 建独立 profile 再谈扩面。 |
| 18 | VP9 与 VideoToolbox 准入 | `0b9dda5b`、`4a7d3e57`、`098c9d8f`、`8f4194fa`、`39d0b911` | 需要确认 | 能力扩面明确（VP9 P0/P2 硬解 + VT admission），但新增解码后端就是登录期新的挂死面；须独立实验分支、默认关、先只装探针与准入。 |
| 19 | Metal/GL 残留项（Chromium 合成、SE 拓扑） | `09a40263`、`c81e686d` | 需要确认 | 需先能复现对应症状（Chromium 双源花屏、Issue #235 拓扑）再评估，不预先改。 |
| 20 | 显示早期竞态 / eDP online / link training 限制 | `d573a2d5`、`dd3720b9`、`544dd02a`、`7cb1fe58` | 需要确认 | 与内屏形态强相关；出现早期灰屏、唤醒丢画面或误判外接再吸收。 |
| 21 | 音频初始探测延时 500ms | `c0680a91` | 需要确认 | 属固定延时；按纪律不进主线，最多作为探测手段。 |
| 22 | 命令缓冲尾部填充 | `eb00c612` | 已在用（复核维持） | 这正是我方 SubmitGuard-v1 的职责：守卫内把「已用尾..容量尾」空闲区整段填 PM4 filler（`0x80000000`），随后执行原入口 15 字节并跳回。参考该提交只作边界条件对照，不重复实现。 |
| 23 | 迁移到 `cs_validate_page`、删除 shared_region 钩子 | `0884bc7e` | 暂不迁移（记复评） | 这是架构级替换，且与我方 `v0.9.1` 稳定点（私有映射 + 引擎门 + 并发闸门）方向相反。迁移前必须先解决「每物理页只验证一次、跳过即全局永久未打补丁」的约束。 |
| 24 | 哈希失配私有缓存补丁器 | `b6937493` | 不考虑 | 上游自己在后续提交里移除了它，回到页缓存 + 签名路线，不照搬。 |
| 25 | SubmitGuard 多 OS 演进 | `7387e010`、`65816cd0`、`ea046131` | 已在用（不跟其演进） | 我方 SubmitGuard-v1 已在用：`-NRedDisplayFix` 打开且无 `-NRedNoSubmitGuard` 时安装；14/15 走 legacy 偏移（Darwin23 `0x30355E210`、Darwin24 `0x3087C3580`），26.x 走生成表（26.7.3 `0x310CE9240`、26.4 `0x310BF1240`）。参考三条是同一机制的多 OS 版本与 ABI/entitlement 演进，且上游 `0884bc7e` 后来把 `UserSubmitGuard.hpp` 整段删除，故不跟随。另注：我方 26.x 偏移是绝对地址硬编码，与「预生成机器码」同类脆弱点，若长期保留应改为签名解析。 |
| 26 | Metal dual-quad / HwInfo 条件式系列 | `da411a3b`、`86c2f339`、`61e56018`、`1231d174`、`ab02dd36` | 不考虑 | 已定案：26.x NOP 该能力分支会导致 FenceNotRetired + FullAsicReset；条件式方案仍依赖 build 常量。 |
| 27 | GFXOFF 压制 | `c9467c96` 内 | 不考虑 | 用户明确否决激进手段。 |
| 28 | Cezanne 专属（VCN/加速渲染） | `02d3789c`、`be1545aa` | 不考虑 | APU 不匹配本机。 |
| 29 | satellite 外置用户态补丁架构 | `484f8d1b`、`1fe5a1ec`、`5c3db137` | 不考虑架构，取一条原则 | 独立 kext 拆分是另一套工程；只取「VFS 延后到首个缓存验证 + 安全内核线程」，已并入第 3 项。 |
| 30 | 工具链 / CI / NootedRedTool / NRPT | 多家 | 不考虑 | 工具链不同，且他们自己删了 NootedRedTool；仅兼容数据采集思路可留档。 |

## 复核更正（2026-11-24）

初稿把「SubmitGuard 系列」整体记为不考虑，属误判：我方 `UserSurfaceSync.cpp` 里确实有 SubmitGuard-v1（命令队列提交方法入口插桩 + 尾部 PM4 filler），只是与参考项目的 Metal SubmitGuard 多 OS 演进不同源。已改为「已在用」并补下具体门控与偏移来源。命令缓冲尾部填充（第 22 项）与我方实现重合，同样并入「已在用」。

## 建议推进顺序

1. **第一批（P0，低风险）**：1、2、3、4、5。都是单点加固或防护，不改变现有架构，可先在本机起实验分支，构建后实机验证再进主线。
2. **第二批（P1）**：6 先做签名机制（不换路由），7 与 9 各自独立分支（登录时序、音频出声都要单独实机验）。
3. **第三批（P2）**：12、13、14 属性能/计算侧，放在稳定线不再变动之后。

需要确认的 9 项，建议按「先取证、后改动」推进：15 与 17 要先读本机 profile 与 `0x15E7` 能力位；16、18 要先确认编码/VP9 的硬件与登录期风险；19、20 要能复现症状再谈。

## 附录：非官方提交全量清单（按功能点分组，286 条）

> `代表仓库` 只标首个出现该提交的仓库；同一条提交在多个仓库存在是 fork 嵌套所致，已去重。

### G02 动态签名补丁基础设施（16 条）

- `08874065` 2026-10-19 weachy、vithalchowdary — UserPatches: Treat 0x15DD as the base Raven family
- `4b2f494a` 2026-10-19 weachy、vithalchowdary — UserPatches: Port the opt-in masked-context resolver into master
- `ac8afadf` 2026-10-20 weachy、vithalchowdary — refactor: extract macOS 15 (Sequoia) patches to UserPatchesSequoia.inc
- `ea71183b` 2026-10-20 weachy、vithalchowdary — feat(UserPatches): add signature-based resolver for macOS 14 Sonoma
- `35d04ef4` 2026-10-27 weachy、vithalchowdary — feat(vda): update VDA page patch tables for macOS 13, 14, 15, and 26
- `44e61bde` 2026-10-27 weachy、vithalchowdary — feat(vda): dynamic signature resolver for AppleGVA and remove 4KB static page tables
- `4e8e5e22` 2026-10-27 steve12311 — UserPatches: Handle relocated shared-cache data sections
- `b079c4ed` 2026-10-27 weachy、vithalchowdary — feat(vda): implement dynamic signature resolver for AMDRadeonVADriver2
- `8fd0b66e` 2026-10-28 xiaot-evo — VCN: runtime symbol-boundary resolver replaces static tables
- `d35956e3` 2026-10-28 xiaot-evo — UserPatch: exact-build page sets, pageOffset probe and semantic fallback
- `56179e55` 2026-10-31 steve12311 — UserPatches: Resolve Darwin 24/25 caches dynamically
- `690e6c52` 2026-11-01 steve12311 — UserPatches: Unify Darwin 23/24/25 repair paths
- `80c8c043` 2026-11-02 weachy、vithalchowdary — feat(user): complete dynamic multi-OS patching across macOS 13-26 and streamline driver size
- `572bc29a` 2026-11-08 weachy、vithalchowdary — fix(patcher): use non-destructive AddrLib wildcards and process-agnostic UBC patching
- `060c2126` 2026-11-22 weachy — refactor(vcn1): unify VCN 1.0 decode boot-args across Raven/Picasso/Raven2
- `ee4590a5` 2026-11-22 weachy — refactor(vcn1): migrate VCN 1.0 decode to cross-version dynamic signatures

### G03 补丁执行安全与页保护（6 条）

- `5ef479ae` 2026-10-23 steve12311 — fix: reduce stack pressure in chained shared-region hooks
- `7a81e78b` 2026-10-24 steve12311 — UserPatches: Fail when maximum page protection restoration fails
- `be32de04` 2026-10-25 weachy、vithalchowdary — fix(UserPatches): enforce strict maximum protection verification on restored pages
- `e8538e1d` 2026-11-03 steve12311 — UserOpenGL: Fix compressed page integrity verification
- `a08a1483` 2026-11-13 xiaot-evo — test: fail the fixture probe when no fixture is exercised
- `e3d1c606` 2026-11-13 xiaot-evo — test: count only enabled rules in the fixture probe

### G04 按地址空间串行化（1 条）

- `6fb1aa6d` 2026-10-28 steve12311 — UserPatches: Serialize VCN patch transactions by address space

### G05 补丁缓存与进程过滤（12 条）

- `31b65c0e` 2026-10-29 steve12311 — UserPatches: Add bounded resolution cache
- `7ba8b8d2` 2026-10-29 steve12311 — UserPatches: Add resolution diagnostics and latency probe
- `cef3ff56` 2026-10-29 steve12311 — UserPatches: Reuse verified video cache locations
- `d9232942` 2026-10-29 steve12311 — SharedCache: Reduce cold display and guard scan costs
- `f08df669` 2026-10-29 steve12311 — UserPatches: Improve shared-cache candidate prefilter
- `f7d9ce76` 2026-10-29 steve12311 — UserPatches: Verify cached shared-cache locations
- `fa08caf2` 2026-10-29 steve12311 — UserPatches: Reuse verified Metal cache locations
- `3c27adc6` 2026-11-14 steve12311 — VPF: Use a kernel spin lock for the FVRC cache
- `84e820d7` 2026-11-14 steve12311 — UserPatches: Skip non-GPU tools and make FVRC reusable
- `92af8d90` 2026-11-14 steve12311 — UserPatches: Harden filtering and completed-map proofs
- `a1b890f2` 2026-11-14 steve12311 — UserPatches: Skip proven re-scans and share the cache header
- `e7ecf909` 2026-11-14 steve12311 — UserPatches: Remember rejected shared caches per boot

### G06 统一补丁执行/协调器（7 条）

- `31fcc872` 2026-10-28 steve12311 — VPF: Consolidate verified shared-cache patch execution
- `4c0f41de` 2026-10-28 steve12311 — DVPF: Unify verified display patch execution
- `0f56032f` 2026-10-29 steve12311 — UserPatchCoordinator: Share state across kext translation units
- `880adce6` 2026-10-29 steve12311 — Display: Gate Metal and OpenGL installation through group planning
- `e32dde00` 2026-10-30 steve12311 — VPF: Reject partially overlapping plan sites
- `a941ff26` 2026-10-31 steve12311 — DVPF: Remove the HDRResearch display group
- `0884bc7e` 2026-11-05 weachy、vithalchowdary — refactor(user): streamline dynamic patching via cs_validate_page and remove shared region hooks

### G09 显存与调度（2 条）

- `c9467c96` 2026-10-31 weachy、vithalchowdary — fix(x5000): prevent SWScheduler page fault, fix VRAM allocator underflow, and disable GFXOFF
- `cf6dce95` 2026-11-20 weachy、vithalchowdary — fix(x5000): fix allocateNonContiguous overload and refine VRAM large-block allocator

### G10 OpenCL（2 条）

- `8d9ef6c1` 2026-11-13 xiaot-evo — chore: ignore local dist and scratch directories
- `8e32f464` 2026-11-16 weachy、vithalchowdary — feat(opencl): clamp workgroup geometry for scratch kernels and add UBC eviction cache

### G11 引擎分配前置条件（1 条）

- `b8ed8540` 2026-10-31 steve12311 — X5000: Fail engine allocation when PM4 or SDMA0 is missing

### G12 VCN2 解码桥（初始）（3 条）

- `3a8a41a1` 2026-10-19 weachy、vithalchowdary — VDA: Add 15.8.1 VCN2 hardware decode bridge
- `c7b5d0dc` 2026-10-19 xiaot-evo — VDA: VCN2 hardware decode/encode bridge validated on 15.8.1 Renoir 0x1638
- `d1a522e8` 2026-10-19 weachy、vithalchowdary — VDA: Validate VCN2 hardware decode on 15.8.1 Renoir 0x1638

### G13 兼容范围与设备/构建判定（4 条）

- `56f0e3a8` 2026-10-19 weachy、vithalchowdary — DeviceProfile: Enable the VDA engine on the other VCN 2.x APUs
- `d63f796a` 2026-10-20 weachy、vithalchowdary — feat: relax VCNBridge build check to match minor release series (23J for 14.8.x, 24H for 15.8.x)
- `3038b08f` 2026-10-21 weachy、vithalchowdary — VDA: Fix dual-GPU laptop support and refine macOS 15.8.x compatibility
- `29377253` 2026-10-27 weachy、vithalchowdary — fix(vcn): remove 0x1638 restriction on Ventura to support all VCN 2.x APUs

### G14 VCN 编解码（32 条）

- `6c7b0804` 2026-10-04 steve12311、Kingtous — VCN: Match Tahoe with native kernel version constants
- `ae8f6bc5` 2026-10-04 steve12311、Kingtous — VCN: Generalize VCN2 APU capability matching
- `338fff9b` 2026-10-05 steve12311、Kingtous — UserPatches: Share dynamic cache resolution
- `1983f326` 2026-10-07 steve12311、Kingtous — VCN: Fix hardware encode feedback and source pitch
- `d1a55d4f` 2026-10-18 steve12311、Kingtous — UserPatches: Restore page protections and keep FairPlay clients native
- `dcbd2895` 2026-10-19 weachy、vithalchowdary — VDA: Add VCN2 hardware encode feedback patches
- `f865422a` 2026-10-19 weachy、vithalchowdary — VDA: Make the exact-build gate late and reliable
- `fb3f6274` 2026-10-19 weachy、vithalchowdary — VDA: Validate VCN2 hardware encode and extend hwtest
- `fc32a727` 2026-10-19 weachy、vithalchowdary — VDA: Harden teardown, page matching and diagnostics
- `fca0e163` 2026-10-19 weachy、vithalchowdary — VDA: Address code-review findings
- `556b814e` 2026-10-20 weachy、vithalchowdary — fix: enable user-space VDA patches when -NRedAVC is specified
- `8e0bec0a` 2026-10-20 weachy、vithalchowdary — feat: add -NRedAVC boot-arg to restrict hardware decode to AVC/H.264 and fallback HEVC to software
- `689b3abc` 2026-10-21 weachy、vithalchowdary — feat: add macOS 26 Tahoe VDA/resolver support and extend VCN/VDA to macOS 14.7.x
- `5a941329` 2026-10-25 weachy、vithalchowdary — fix(VDA): cap HEVC Profile 2 to level 0 for safe fallback and add teardown drain protection
- `9a427e34` 2026-10-25 weachy、vithalchowdary — Enable VCN hardware video acceleration on Ventura 13.6.4
- `1abc701b` 2026-10-27 weachy、vithalchowdary — fix(vda): strictly isolate AMDRadeonVADriver2 signatures from dyld_shared_cache
- `4c4f142c` 2026-10-27 weachy、vithalchowdary — feat(config): enable GLFix and VDA by default with opt-out boot arguments
- `89164ea7` 2026-10-27 weachy、vithalchowdary — fix(vda): resolve AMDRadeonVADriver2 signatures inside dyld_shared_cache
- `27ce504d` 2026-10-28 steve12311 — UserPatches: Trim hot-path diagnostics and OpenGL page scanning
- `8bdc4dbc` 2026-10-28 steve12311 — UserPatches: Reduce dynamic cache resolver CPU and stack usage
- `8edefdbf` 2026-10-28 steve12311 — UserPatches: Optimize submit and video resolver paths
- `9e7b4076` 2026-10-28 weachy、vithalchowdary — feat(vda): fix hardware encoding artifacts, multi-site resolver matching, and remove teardown stalls
- `4ab22112` 2026-10-29 steve12311 — VCN: Accept allocated padding for aligned encode surfaces
- `c72dd931` 2026-10-29 steve12311 — UserPatches: Avoid page accounting in FVRC probes
- `df94540c` 2026-10-29 steve12311 — UserVideoDecode: Track all live video address spaces
- `33c5f51f` 2026-10-30 steve12311 — VCN: Support HEVC Main10 encoding and 10-bit regressions
- `74ec910b` 2026-10-31 weachy、vithalchowdary — feat(vda): expand VCN bridge OS support and split Sonoma/Sequoia resolver signatures
- `efef7e3a` 2026-10-31 weachy、vithalchowdary — fix(user): harden dynamic resolver signatures and isolate WindowServer from VDA patching
- `eedbc2d8` 2026-11-16 xiaot-evo — VCN: bridge the X5000/X6000 VM context metaclasses
- `57416db2` 2026-11-18 weachy、vithalchowdary — chore(version): bump version to 1.2.7
- `7487a913` 2026-11-20 weachy、vithalchowdary — feat(vda): add HEVC Main10 encode, 8K decode, and VCN media failure fallback
- `72392609` 2026-11-22 weachy — feat(vda): enable HEVC Main10 encode and slice delta signatures on Ventura and Sonoma

### G15 任务销毁 drain（2 条）

- `c9a71268` 2026-11-05 weachy、vithalchowdary — feat(vcn): add adaptive engine drain on task teardown and -NRedNoHEVC10 flag
- `2bd1b74e` 2026-11-08 weachy、vithalchowdary — fix(vcn): remove blocking task teardown drain to eliminate periodic stuttering

### G16 编解码守卫（3 条）

- `62405e63` 2026-10-31 steve12311 — VCN: Remove the codec format and size guards
- `30d2c407` 2026-11-14 steve12311 — VP9: Refuse profile 2 at session creation
- `ad6bf789` 2026-11-14 steve12311 — VP9: Fail oversized sessions and undecoded frames

### G17 VP9 与 VT 准入（12 条）

- `3c28aba9` 2026-10-31 steve12311 — Display: Remove withdrawn VideoToolbox Scratch dependency
- `0b9dda5b` 2026-11-14 steve12311 — VP9: Add VCN decode backend for the AMD driver and its patch generator
- `3871783b` 2026-11-14 steve12311 — VP9: Add VCN decode message builder, verified offline against Mesa
- `4a7d3e57` 2026-11-14 steve12311 — VP9: Advertise the decoder to AppleGVA and support direct bitstream
- `65758807` 2026-11-14 steve12311 — VCN: Add -NRedVP9Trace diagnostic channel for the VP9 backend
- `d15c0f64` 2026-11-14 steve12311 — VCN: Add opt-in VP9 capability probe behind -NRedVP9Probe
- `e33626ea` 2026-11-14 steve12311 — VCN: Install the VP9 backend behind -NRedVP9
- `39d0b911` 2026-11-16 steve12311 — VideoToolbox: Unify VP9 and AV1 decoder admission
- `d97e59d1` 2026-11-17 steve12311 — VideoToolbox: Fix decoder admission test architecture
- `098c9d8f` 2026-11-18 steve12311 — VideoFix: Add VP9 Profile 2 hardware decoding
- `e9c72db9` 2026-11-18 steve12311 — VideoToolbox: Resolve decoder patches across Darwin 23–25
- `4af9e98a` 2026-11-20 weachy、vithalchowdary — feat(vp9): add hardware VP9 decode support across macOS 13-26

### G18 VP9 默认开启（1 条）

- `8f4194fa` 2026-11-18 steve12311 — VideoFix: Enable VP9 hardware decoding by default

### G20 Metal/GL 用户态补丁（13 条）

- `be1545aa` 2026-10-03 steve12311、Kingtous — X5000: Fix Cezanne accelerated rendering
- `09a40263` 2026-10-18 weachy、vithalchowdary — UserPatches: Fix Chromium dual-source blend corruption
- `1231d174` 2026-10-18 weachy、vithalchowdary — Revert Metal HwInfo source patch after visual artifacts
- `4b4421fb` 2026-10-18 weachy、vithalchowdary — analysis: Add Ghidra decompilation notes for MTL RB+ path
- `61e56018` 2026-10-18 weachy、vithalchowdary — research: Fix Metal HwInfo capability at the source
- `77419566` 2026-10-18 weachy、vithalchowdary — analysis: Verify source-level HwInfo patch after reboot
- `86c2f339` 2026-10-18 weachy、vithalchowdary — analysis: Accept conditional dual-quad patch after user validation
- `c155952e` 2026-10-18 weachy、vithalchowdary — analysis: Study midterm conditional dual-quad feasibility
- `da411a3b` 2026-10-18 weachy、vithalchowdary — UserPatches: Conditionally disable dual-quad for Metal
- `b17ff6f0` 2026-10-19 weachy、vithalchowdary — UserPatches: Add experimental 26.7.1 Metal dual-quad patch
- `f44dd3ae` 2026-10-22 weachy、vithalchowdary — UserPatches: implement Triple-Gate resolver hardening and fix Sequoia allowOffset
- `269c0965` 2026-10-25 weachy、vithalchowdary — feat(UserPatches): add Ventura support, process-aware patching, and fine-grained Metal blend controls
- `ac69f84e` 2026-10-28 steve12311 — UserPatches: Reduce shared-cache scan overhead

### G21 SE 拓扑（1 条）

- `c81e686d` 2026-11-12 weachy、vithalchowdary — fix(metal): correct shader engine topology in shared cache and resolve Issue #235

### G22 GL 资源预算（4 条）

- `25c5806e` 2026-10-19 weachy、vithalchowdary — UserPatches: Add experimental macOS 26.7.1 GL patch set
- `d71f047c` 2026-10-19 weachy、vithalchowdary — feat: add macOS 26 (Tahoe) user patches for OpenGL and Metal drivers
- `986e69d0` 2026-10-22 weachy、vithalchowdary — UserPatches: eliminate unsafe single-site fallback and add Gate 0 vnode filter in patchNativePage
- `7c690b88` 2026-11-20 weachy、vithalchowdary — feat(gl): add GL resource budget patch and expose native media patch status

### G23 SubmitGuard/命令缓冲（4 条）

- `eb00c612` 2026-10-18 steve12311、Kingtous — UserPatches: Fill AMD command-buffer free tails before submit
- `ea046131` 2026-10-21 steve12311、Kingtous — UserPatches: Preserve used command buffer contents and add regression tests
- `65816cd0` 2026-10-22 weachy、vithalchowdary — feat(UserPatches): fix SubmitGuard syscall ABI and add FairPlay entitlement check
- `7387e010` 2026-10-22 weachy、vithalchowdary — feat(UserPatches): add multi-OS SubmitGuard for Metal command queues

### G25 AGFXHDA 路由（1 条）

- `37813495` 2026-10-19 weachy、vithalchowdary — AGFXHDA: Skip symbols already routed by another plugin

### G26 音频（3 条）

- `0642de11` 2026-11-18 weachy、vithalchowdary — feat(audio): add event-driven DCN 1.0/2.1 HDMI & DP audio support (#225)
- `e45f369b` 2026-11-20 weachy、vithalchowdary — fix(audio): support 5 Azalia endpoints, multi-display DIG matching, and refresh-rate DTO sync
- `2f952e12` 2026-11-22 weachy — fix(audio): check active internal panel and DTO source/clock mismatch for DP/HDMI audio

### G27 音频延时（1 条）

- `c0680a91` 2026-11-23 weachy — perf(audio): reduce initial delayed probe to 500ms for faster HDMI/DP audio convergence

### G28 显示/eDP（7 条）

- `f8179e5c` 2026-10-25 steve12311 — UserOpenGL: Apply page-local display fixes after validation
- `d500fdd1` 2026-10-31 steve12311 — Legacy: Keep historical video profiles out of the kext
- `d573a2d5` 2026-11-05 weachy、vithalchowdary — fix(display): resolve early-boot race conditions and online detection for internal eDP panels
- `544dd02a` 2026-11-08 weachy、vithalchowdary — fix(display): restrict eDP link training retry strictly to internal panel FB0
- `7cb1fe58` 2026-11-09 weachy、vithalchowdary — fix(display): restrict DP receiver power control delay strictly to power-on
- `dd3720b9` 2026-11-09 weachy、vithalchowdary — fix(display): restrict early-boot online override to internal eDP and fix monitor power-off signal recovery
- `54a12cee` 2026-11-14 weachy、vithalchowdary — docs(display): clarify embedded-panel fallback covers LVDS and eDP

### G29 背光/OLED（4 条）

- `dda9f708` 2026-10-21 steve12311、Kingtous — Backlight: Isolate EC brightness control from AMD callbacks
- `128b9806` 2026-10-22 steve12311、Kingtous — UserPatches: Support macOS 14/15/26 and unify fix controls
- `378d23a3` 2026-11-23 weachy — fix(fb): hook doPowerStateChange to re-power controller on wake and support OLED flags
- `abd98a2d` 2026-11-23 weachy — feat(backlight): add -NRedOLED support and clamp minimum AUX nits to prevent OLED shutoff

### G30 兼容层架构（32 条）

- `02d3789c` 2026-10-03 steve12311、Kingtous — VCN: Add experimental Cezanne hardware decoding
- `08126d38` 2026-10-04 steve12311、Kingtous — UserSurfaceSync: Support Vega APUs and compatible Tahoe caches
- `02217aa5` 2026-10-19 weachy、vithalchowdary — DeviceProfile: Make profile table constexpr for invariants
- `142cad95` 2026-10-19 weachy、vithalchowdary — compat: Stay inert on unknown APUs and drive VDA gates from the profile
- `1e43a4e0` 2026-10-19 weachy、vithalchowdary — VDA: Implement Phase 1 media capability skeleton
- `5081a403` 2026-10-19 weachy、vithalchowdary — feat: Add macOS 14 (Sonoma) UserPatches and fix personality injection
- `55091632` 2026-10-19 weachy、vithalchowdary — UserPatches: Split patch sets by APU family
- `6aee2cd1` 2026-10-19 weachy、vithalchowdary — NRed: Restore the original device detection behavior
- `880b6754` 2026-10-19 weachy、vithalchowdary — compat: Add capability profiles, target identity capture and compat logging
- `9e047af9` 2026-10-19 weachy、vithalchowdary — DeviceProfile: Add compile-time profile invariants
- `b36ab686` 2026-10-19 weachy、vithalchowdary — compat: Remove the early VFS identity read from the boot path
- `c21640d6` 2026-10-19 weachy、vithalchowdary — DeviceProfile: Add unified graphics/media capability layer
- `d5998bf4` 2026-10-19 weachy、vithalchowdary — compat: Wrap long lines and refresh references after the branch merge
- `f7e64872` 2026-10-19 weachy、vithalchowdary — docs: Record the confirmed early-VFS boot hazard
- `0c65bd65` 2026-10-20 weachy、vithalchowdary — fix: add missing VDA/VCNBridge sources to Xcode project and enable Darwin 23.6 in VCNBridge
- `569b4532` 2026-10-20 weachy、vithalchowdary — feat: implement macOS 14 (Sonoma) VDA hardware decode acceleration and multi-OS dispatch
- `3968581f` 2026-10-22 weachy、vithalchowdary — feat(tools): auto-detect macOS version and format nred-compat output folder
- `4e5b0c97` 2026-10-22 weachy、vithalchowdary — fix(tools): preserve full version string when generating zip in collect_compat_data.py
- `a847c89f` 2026-10-22 weachy、vithalchowdary — feat(VDA): add -NRedAVCEncode mode, suppress GFXOFF hangs, and fix compute vtable hang
- `ab02dd36` 2026-10-22 weachy、vithalchowdary — feat(UserPatches): add multi-OS dynamic LegacyBlend Metal DualQuad fix for UI dropdowns
- `122ce3b9` 2026-10-23 steve12311、Kingtous — UserPatches: Share page helpers and speed up legacy validation
- `27e75c84` 2026-10-23 steve12311、Kingtous — UserPatches: Unify FairPlay clients and preserve native sync
- `f7d36997` 2026-10-23 steve12311、Kingtous — UserVideoDecode: Unify FairPlay PSP compatibility by entitlement
- `454d73a7` 2026-10-25 weachy、vithalchowdary — tools(VDA): enhance multi-OS patch generators and diagnostic tools
- `ac1a08e2` 2026-10-25 steve12311 — UserPatches: Port Tahoe fixes to dynamic Sequoia matching
- `cadb94c7` 2026-10-28 steve12311 — UserPatches: Guard VCN2 codec requests and bound kernel stack
- `f98468e3` 2026-10-28 xiaot-evo — UserPatch: deliver the profile through the injected personality
- `d1170388` 2026-10-29 xiaot-evo — profile: NRPT v1 engine, generator, validator and version fixtures
- `8842003b` 2026-10-30 xiaot-evo — profile: NRPT v3 engine, bundle selector and injector
- `20df9cff` 2026-11-13 xiaot-evo — nred: retire the compatibility target layer
- `db5ad14c` 2026-11-13 xiaot-evo — nred: restore the iGPU guard in setProp32
- `52158395` 2026-11-22 weachy — fix(tools): verify NootedRed kext load and -NRedOff/-liluoff before live cache warning

### G31 satellite 外置补丁（24 条）

- `484f8d1b` 2026-10-25 xiaot-evo — satellite: move user-space patching into NootedRedUserPatch
- `cba79f1a` 2026-10-29 xiaot-evo — UserPatch: v2 direct-map coverage, hardware triage and hook-only verdict
- `44010363` 2026-10-30 xiaot-evo — docs: split documentation into usage docs and local research records
- `1131d2b1` 2026-11-07 xiaot-evo — analyzer: write the full 13-byte rule 13 replacement
- `1fe5a1ec` 2026-11-07 xiaot-evo — satellite: defer analyzer VFS until the first dyld cache validation
- `27de24be` 2026-11-07 xiaot-evo — analyzer: add host-test skeleton and byte source
- `2c6d42da` 2026-11-07 xiaot-evo — analyzer: locate images in the dyld shared cache
- `4ee3f6ac` 2026-11-07 xiaot-evo — satellite: apply resolved sites in the page validation hook
- `5c3db137` 2026-11-07 xiaot-evo — satellite: run the analyzer in a VFS-safe kernel thread
- `628605af` 2026-11-07 xiaot-evo — analyzer: parse Mach-O load commands
- `6528aa75` 2026-11-07 xiaot-evo — analyzer: resolve the user-space patch rules and shape variants
- `70bc9568` 2026-11-07 xiaot-evo — userpatch: remove the profile pipeline and update docs
- `a83094e5` 2026-11-07 xiaot-evo — analyzer: resolve symbols and function ranges
- `c6eb98ce` 2026-11-07 xiaot-evo — satellite: log analyzer trigger and VFS diagnostics
- `da79d106` 2026-11-07 xiaot-evo — satellite: analyze live images at boot
- `e304c256` 2026-11-07 xiaot-evo — satellite: mirror analyzer diagnostics to the kernel log
- `f75d7ac0` 2026-11-07 xiaot-evo — satellite: fix cross-block reads in the vnode block cache
- `19684e93` 2026-11-13 xiaot-evo — analyzer: reject rules without a target image path
- `3ecaa914` 2026-11-13 xiaot-evo — style: wrap over-long analyzer lines
- `73fdfaa1` 2026-11-13 xiaot-evo — userpatch: cache parsed images and extract the block reader
- `8c2c83e3` 2026-11-13 xiaot-evo — userpatch: drop cached images when closing analyzer files
- `bdab2997` 2026-11-13 xiaot-evo — docs: fix the satellite build command and test index
- `bfb0b830` 2026-11-13 xiaot-evo — release: NootedRed 0.9.2, NootedRedUserPatch 0.3.2
- `6d229431` 2026-11-18 xiaot-evo — release: NootedRed 0.9.3, NootedRedUserPatch 0.3.2

### G32 工具/CI/文档（87 条）

- `86defd19` 2026-10-18 weachy、vithalchowdary — analysis: Record kernel-delivered conditional patch verification
- `0b91c7b3` 2026-10-19 weachy、vithalchowdary — build: Put artifacts under a per-branch directory
- `0d9c098e` 2026-10-19 weachy、vithalchowdary — docs: Add dedicated VDA hardware decode implementation plan
- `18fcf5ad` 2026-10-19 weachy、vithalchowdary — docs: Expand Chromium fix usage guide
- `214b4c2d` 2026-10-19 weachy、vithalchowdary — tools: Add host-side patch-set selection checks
- `257fa2c6` 2026-10-19 weachy、vithalchowdary — docs: Confirm shared 26.7.1 page data across Renoir APUs
- `2582567a` 2026-10-19 weachy、vithalchowdary — tools: Optimize platform collector default payload
- `2c02d9bf` 2026-10-19 weachy、vithalchowdary — docs: Add second 26.7.1 Renoir dataset
- `4282763e` 2026-10-19 weachy、vithalchowdary — docs: Record the validated hardened VDA build
- `4b4f4530` 2026-10-19 weachy、vithalchowdary — docs: Clarify that -NRedGLFixResolve is only in the resolver branch
- `4c27e847` 2026-10-19 weachy、vithalchowdary — docs: Index external platform dataset
- `4da55abe` 2026-10-19 xiaot-evo — DeviceProfile: unified graphics/media capability layer
- `51782794` 2026-10-19 xiaot-evo — build: per-branch artifacts, CI layout and VDA hardening
- `58d5de23` 2026-10-19 weachy、vithalchowdary — tools: Add the focused compatibility data collector
- `5d9d4927` 2026-10-19 weachy、vithalchowdary — docs: Add the multi-version compatibility architecture
- `6032d46b` 2026-10-19 weachy、vithalchowdary — build: Remove duplicate branch-dir helper
- `64692a8b` 2026-10-19 weachy、vithalchowdary — docs: Index external platform dataset
- `65e23b0c` 2026-10-19 xiaot-evo — UserPatches: version/APU-aware patch sets and the Chromium dual-source fix
- `66c6c7d5` 2026-10-19 xiaot-evo — compat: multi-version compatibility architecture with target identity
- `78e3e26a` 2026-10-19 weachy、vithalchowdary — tools: Add host-side patch-data consistency test
- `7c5b0aae` 2026-10-19 weachy、vithalchowdary — tools: Add platform dataset importer
- `9cd5c51a` 2026-10-19 weachy、vithalchowdary — build: Put artifacts under a per-branch directory
- `a849b850` 2026-10-19 weachy、vithalchowdary — docs: Add contributor guide for UserPatches
- `c71cca4d` 2026-10-19 weachy、vithalchowdary — docs: Link the device profile guide
- `ca97c426` 2026-10-19 weachy、vithalchowdary — build: Remove duplicate branch-dir helper
- `cdedd762` 2026-10-19 weachy、vithalchowdary — tools: Add comprehensive platform data collector
- `cf72a265` 2026-10-19 weachy、vithalchowdary — UserPatches: Add version and APU-aware patch sets
- `d8fdfea5` 2026-10-19 weachy、vithalchowdary — CI: Match per-branch build artifact layout
- `29b3330d` 2026-10-20 weachy、vithalchowdary — chore: Reorganize the ignore rules
- `429031fd` 2026-10-20 weachy、vithalchowdary — docs: Add the GL investigation notes and probe to the repository
- `deb0714a` 2026-10-20 weachy、vithalchowdary — docs: Merge the archived research documentation into master
- `157c4b64` 2026-10-21 xiaot-evo — VCN/GL: multi-build data, semantic fallbacks and Sonoma/Tahoe support
- `3b37dfc6` 2026-10-22 weachy、vithalchowdary — docs(tools): update README-collect-compat-data.md with new usage and --out behavior
- `d84fd585` 2026-10-22 weachy、vithalchowdary — chore: bump version to 1.0.0
- `efe90c3e` 2026-10-22 weachy、vithalchowdary — chore: bump version to 1.0.1
- `a39270c6` 2026-10-25 weachy、vithalchowdary — chore: bump version to 1.1.0
- `d7f2ec99` 2026-10-26 weachy、vithalchowdary — chore: bump version to 1.1.1
- `83463c3f` 2026-10-27 weachy、vithalchowdary — docs: add VDA dynamic resolver architecture and adaptation roadmap
- `d95aa09b` 2026-10-27 weachy、vithalchowdary — chore: bump version to 1.2.0
- `802ecc5d` 2026-10-28 xiaot-evo — NootedRedTool: macOS toolkit with offset-profile generator
- `a8bbb362` 2026-10-28 weachy、vithalchowdary — docs: add release notes for v1.2.0
- `7ef0c4b6` 2026-10-29 steve12311 — .github: Refresh issue templates
- `90b763d0` 2026-10-30 xiaot-evo — NootedRedTool: GUI 0.2.0 with EFI service, health checks and snapshots
- `9d03ea92` 2026-10-31 weachy、vithalchowdary — docs: add release notes for v1.2.1
- `c63d4b3d` 2026-10-31 weachy、vithalchowdary — docs: include macOS 26 Tahoe in CoreMedia command requirements
- `d36370fc` 2026-10-31 weachy、vithalchowdary — tools: add --sys-root support for offline volume data collection
- `d69a8d16` 2026-10-31 weachy、vithalchowdary — chore: bump version to 1.2.1
- `d7a59fa2` 2026-10-31 steve12311 — Tools: Read AMD kexts from the KernelCollection on Tahoe
- `82c63464` 2026-11-05 weachy、vithalchowdary — chore(release): bump version to 1.2.3
- `182a5959` 2026-11-07 xiaot-evo — docs: drop obsolete migration plan and profile test note
- `35459ff8` 2026-11-07 xiaot-evo — docs: add runtime user-space patching design and plan
- `b0249947` 2026-11-07 xiaot-evo — docs: record 24H232 hardware verification and reader invariants
- `1a9beaa2` 2026-11-08 xiaot-evo — docs: remove NootedRedTool references
- `4551876d` 2026-11-08 xiaot-evo — chore: remove NootedRedTool and legacy tooling
- `4674c3e2` 2026-11-08 weachy、vithalchowdary — chore(release): bump version to 1.2.4
- `a0ee359e` 2026-11-11 steve12311 — ci: enable automatic builds on every branch push
- `3c5ae6ca` 2026-11-12 weachy、vithalchowdary — chore(release): bump version to 1.2.5
- `58cc40a7` 2026-11-13 xiaot-evo — test: cover the short final block and backing failures
- `054a5a6b` 2026-11-14 steve12311 — tools: Add a streaming VP9 decode comparator
- `32ad523e` 2026-11-14 steve12311 — docs: Record VP9 chroma and 1080p verification
- `3439a6e0` 2026-11-14 steve12311 — docs: Document process filter and FVRC reuse
- `695b7cbf` 2026-11-14 steve12311 — tools: Add a performance mode to the VP9 probe
- `72d815a4` 2026-11-14 steve12311 — tools: Add in-process and luma dump modes to probe_vp9_decode
- `7829500f` 2026-11-14 steve12311 — docs: Close phase 5 and refresh next steps
- `7cf4754a` 2026-11-14 steve12311 — tools: Dump the CbCr plane in the VP9 probe
- `94648dc9` 2026-11-14 steve12311 — tools: Take VP9 profile and bit depth from the first frame in the probe
- `996f3956` 2026-11-14 steve12311 — tools: Add a VP9 browser test monitor
- `99750afa` 2026-11-14 steve12311 — docs: Add video survey, VP9 hardware decode plan and V0/V1 findings
- `a34332d8` 2026-11-14 steve12311 — HWLibs: Add opt-in VCN power-gate trace behind -NRedVCNPowerTrace
- `bddfaaf4` 2026-11-14 steve12311 — UserPatches: Add per-phase timing behind -NRedPhaseTiming
- `d7d6e37b` 2026-11-14 steve12311 — docs: Record VP9 4K, long clip and resolution change tests
- `da1baea8` 2026-11-14 steve12311 — docs: Record VP9 V3 capability gate and first correct frames
- `daef1fc5` 2026-11-14 steve12311 — docs: Add VP9 V3 driver integration design
- `eeea5ccc` 2026-11-14 steve12311 — docs: Close VP9 V3 and record V4 performance
- `f75808bf` 2026-11-14 steve12311 — docs: Record the first VP9 browser test
- `f8bc72c6` 2026-11-14 steve12311 — docs: Record VP9 profile 2 refusal on the installed kext
- `d400baed` 2026-11-15 steve12311 — VP9: Validate dynamic backends and transactional installation
- `548631e1` 2026-11-16 weachy、vithalchowdary — chore(version): bump version to 1.2.6 for upcoming cycle
- `8cad8856` 2026-11-18 weachy、vithalchowdary — docs(solutions): add final solution docs for issues #28, #137, #158, #235, and #270
- `96ba5487` 2026-11-18 steve12311 — UserPatches: Unify Sonoma with the dynamic patch pipeline
- `9c1cccff` 2026-11-18 steve12311 — UserPatches: Rebase Sequoia display references by section
- `a1836217` 2026-11-18 steve12311 — UserPatches: Unify Sequoia and Tahoe patch pipelines
- `58dc08ec` 2026-11-19 weachy、vithalchowdary — chore(version): bump version to 1.2.8
- `208ca8f0` 2026-11-20 weachy、vithalchowdary — chore(version): bump version to 1.3.0
- `6b36d576` 2026-11-20 weachy、vithalchowdary — Release v1.3.1
- `86e29c62` 2026-11-22 weachy — chore(version): bump version to 1.3.1 and add v1.3.0 release notes
- `c89751f2` 2026-11-23 weachy — chore(version): bump version to 1.3.2

### G33 哈希失配私有补丁器（1 条）

- `b6937493` 2026-11-22 weachy — Fixed Hash Mismatchin dyld shared cache
