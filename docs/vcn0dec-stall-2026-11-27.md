# VCN0Dec 复位风暴导致的关机停滞 / 看门狗断电（2026-11-27，macOS 14.8.9）

## 0. 结论（先看这一段）

1. **两次事故是同一个病**：11-27 09:23 那次“关机卡死”（`shutdownStall`）与 11-27 11:14 那次“整机卡死后看门狗断电”，抓到的阻塞线程都是内核 `IOServiceTerminateThread` 停在 `lck_mtx_sleep`，GPU 复位路径的终止流程走不完。
2. **触发源只有一个**：`Channel 12 VCN0Dec`。本机现存 **20 份 gpuRestart 报告，20/20 全部是 channel 12 + `VTDecoderXPCServ`**，跨 11-26 01:47 到 11-27 11:14，没有第二类。
3. **放大器是两个**：(a) 13.5 MB 大小的 VRAM 分配在“3 GB 固定区里还剩 3.1 GB 空、可用区只剩 66 MB”的状态下反复失败（分配器每次只回收 64 KB 再重试）；(b) VCN 的 `PendingTS` 永远不 retire，驱动每 ~50 s 整卡复位一次，同一 PID 连复 7 次，把一次通道故障升级成整机死锁。
4. **`-NRedVideoFix` 就是这条链的总闸**：按 `docs/boot-arguments.md`，该参数开启 VCN 硬解/硬编路径，去掉后“VCN 桥接关闭”。14.x 上若要立即止血，摘掉它是唯一零代码、可回退、下次开机即生效的动作。
5. **与显存容量无关**：事故时 VRAM 池 3 GB、GART 15 GB 几乎全空（`gartUsedBytes`≈120 MB）。这不是“显存不够”，是“固定区内该分配的落点拿不到、又不会回落到 GART”。
6. **对症的补丁已经在 dev-v2 上了**：`50b07e3`（`-NRedAllocPagesUnderflow` 空洞下溢修 + `-NRedVRAMAlloc` 五级碎片回退 + VCN 能力门只在引擎落地后才暴露），默认关，且不在本机这个构建里 —— 见 §8。

---

## 1. 环境（取证时的真值）

| 项 | 值 |
| --- | --- |
| 系统 | macOS 14.8.9 (23J631)，Darwin 23.6.0，x86_64 |
| 机型标识 | MacBookPro16,2（Hackintosh，AMD Renoir APU） |
| kext | `org.ChefKiss.NootedRed 0.9.0`（UUID `8DA0D43F-7478-340C-90D6-220DFF51F855`）。判定依据：`ioreg` 中存在 `NRedDisplayFixVersion=2` / `NRedDRMVideoRevision=4` / `NRedGLMetalApplied=2`，但**不存在** `NRedVCNCapabilityFlag`、`NRedDecodeGuardRevision`、`NRedVRAMAllocEnabled` → 该构建**不含** dev-v2 的 `50b07e3` |
| boot-args | `-v -no-compat-check -dacompat -NRedDPDelay keepsyms=1 -noht40 unfairgva=2 vm_compressor=4 -lilubetaall -vsmccomp -Xibtcompatbeta -Xbtlfxallowanyaddr Xagdpmod=pikera Xnpci=0x2000 -Xalcid=97 debug=0x144 -btleadv -itlawdl*(≈20 项) itlawdlintcoal=4 itlawdltxqueue=4096 -NRedDisplayFix -NRedVideoFix -NRedGLMetal` |
| 显存 | `VRAM,totalMB = 3072`；`gartSizeBytes = 15032385536` |

---

## 2. 证据清单（全部为本机现场文件，非推测）

系统级：`/Library/Logs/DiagnosticReports/`

| 文件 | 用途 | 关键内容 |
| --- | --- | --- |
| `Kernel_2026-11-27-111420_MacBook-Pro.gpuRestart` | 最后一次复位 | `Restart Channel: 12 VCN0Dec`；`FirstPendingCB: PID = 11838, Name = VTDecoderXPCServ; SubmitContext = Video, VMID = 5`；`PendingTS = 0xfa, sent at 1:48:14.880059, TS = 0xa`；`TotalVideoRAMBytes 0xC0000000` |
| `WindowServer-2026-11-27-111440.ips`（bug_type 409） | 看门狗判定 | `unresponsive dispatch queue(s): com.apple.WindowServer.HIDEvents`、`unresponsive work processor(s): WindowServer main thread`、`40 seconds since last successful checkin` |
| `WindowServer_2026-11-27-111445_*.userspace_watchdog_timeout.spin` | 死锁现场堆栈 | `IOServiceTerminateThread`(0x70) 12/12 采样停在 `IOAcceleratorFamily2 → AMDRadeonX6000 → AMDRadeonX5000 → IOAcceleratorFamily2 → lck_mtx_sleep`；WindowServer 主线程 / `com.apple.coreanimation.mtl_dealloc` / `com.Metal.DeviceCleaupQueue` 全部挂在该 turnstile 上 |
| `shutdown_stall_2026-11-27-092347_*.shutdownStall` | **上一次关机卡死** | spindump 二进制，`spindump -i` 解析后：`IOServiceTerminateThread` 100/100 采样停在 `IOService::terminateThread → lck_mtx_sleep` |
| `Kernel-2026-11-26-015902 / 235807 / 2026-11-27-010522.panic` | 三次 panic | `userspace watchdog timeout: no successful checkins from WindowServer (… induced crashes) in 120 seconds` |
| `shutdown_stall_*`（共 11 份，11-26 00:45 起） | 长期性 | 关机停滞不是今天才有 |

gpuRestart 全量（20 份，全部 channel 12 / VCN0Dec / VTDecoderXPCServ）：

```text
11-26 014743, 014853, 015625, 015705-1, 015756
11-26 235534, 235615
11-27 000933, 005758, 005947, 010217, 010257, 010327
11-27 110840, 110941, 111012, 111052, 111153, 111235, 111420
```

运行期计数器（本次开机采样，`ioreg -l | grep PerformanceStatistics`）：

```text
inUseVidMemoryBytes            ≈ 1045 MB
vramFreeBytes                  ≈  556 MB
orphanedReusableVidMemoryBytes ≈ 1619 MB（1508 块，hitRate 95）
gartSizeBytes                  = 15 GB，gartUsedBytes ≈ 120 MB
TotalVideoRAMBytes             = 3 GB
```

三点可直接读出的事实：固定区里 1.5 GB 处于“可复用孤儿块”、可用碎片只剩 ~0.55 GB、GART 基本空着却没人用。

---

## 3. 时间线（11-27）

```text
09:23:42  WindowServer 进入 shutdown（CGXShutdownDisplays → ~Framebuffer）
09:23:43  launchd 生成 shutdown stall 报告；内核 IOServiceTerminateThread 停在 lck_mtx_sleep
          → 关机走不完，用户硬断电
09:26:16  重新开机（-NRedVideoFix 仍在）
11:06:03  显示/电源状态变化（kernel PMRD）
11:07:32  VRAM 分配失败爆发（1 秒 58 条）：
          Failed to allocate size:14155776 (13.5 MB)
          There is 66293696 free memory remaining, and 3135434688 fixed-free memory remaining
          （每次重试“free memory”只涨 65536 = 64 KB，说明是“回收一点再试”的循环）
11:08:20  WeChatAppEx Helper (GPU)[22218] 建立到 com.apple.coremedia.videodecoder 的连接
11:08:22  VTDecoderXPCService[11838] 被 launchd 按 ipc(mach) 拉起
11:08:40  第 1 次 GPU Reset：Restart Channel 12 VCN0Dec，FirstPendingCB = PID 11838，PendingTS 0xfa 不完成
11:09:41 / 11:10:12 / 11:10:52 / 11:11:53 / 11:12:35 / 11:14:20  同一通道、同一 PID 连续再复位 6 次
11:14:20  kernel 线程 IOServiceTerminateThread 在 AMDRadeonX5000 → IOAcceleratorFamily2 路径持锁
          WindowServer 主线程 / mtl_dealloc / DeviceCleaupQueue / HIDEvents 全部 blocked
11:14:40  WATCHDOG：40 秒无 checkin（累计 644 次成功 checkin）→ 判定 WindowServer 不活
11:16:28  重新开机（本次）
```

同一个 `FirstPendingCB` + 同一个 `PendingTS` 连续 7 份复位报告，含义很明确：**驱动从来没有真正回收这个解码提交**，每次 reset 只是把通道重来一遍。

---

## 4. 机制链（把“一次解码超时”升级成“整机卡死”）

```text
[1] 创建硬解会话（AVC/HEVC，VideoToolbox 选 VCN）
      ↓  分配解码 surface / ring（落在 3 GB 固定区）
[2] 固定区碎片化：可复用孤儿块 1.6 GB 回收不及时 → 13.5 MB 分配失败（循环重试）
      ↓
[3] VCN0Dec 提交的 PendingTS 不 retire（时间戳永远等不到）
      ↓
[4] 驱动判超时 → 整卡 Reset（Apple accel 层，不区分“通道级/整卡级”）
      ↓  每 ~50 s 一次，同一 PID 连 7 次
[5] reset 期间 terminate 路径（IOServiceTerminateThread）在
    IOAcceleratorFamily2 → AMDRadeonX6000 → AMDRadeonX5000 → IOAcceleratorFamily2
    之间来回持锁 → 永久 lck_mtx_sleep（12/12 采样全部停在这条链上）
      ↓
[6] WindowServer 主线程 / CoreAnimation 释放队列 / HID 事件队列全部挂在该 turnstile 上
      ↓
[7a] 运行时：40 s 无 checkin → WATCHDOG 判死 → 断电重启（11:14）
[7b] 关机时：IOService 终止流程走不完 → shutdown stall，机器挂住（09:23）
```

第 [4]→[5] 步是本轮的**放大器**：通道级故障本来只该让一路解码失败，但整卡 reset 叠加锁序问题，直接把渲染与输入链路一起带走。这也是为什么“看视频/预览视频”这种低负载操作能把机器搞死。

---

## 5. 现有解码守卫的覆盖范围（重要）

源码侧（`master` 与 `dev-v2` 都有）`NootedRed/UserVideoDecode.cpp` + `tools/vad_decode_guard.s` 现在只做一件事：

```asm
cmpl $5, 120(%rdi)     // [self+0x78] codec == 5(HEVC)
jne  Lrestore
cmpl $4096, 124(%rdi)  // width  > 4096
ja   Lreject
cmpl $2304, 128(%rdi)  // height > 2304
ja   Lreject
```

即：**只有“HEVC 且超 4096×2304”会被拒绝并回退软解**。按 `docs/hevc-8k-hang-incident.md` 的字段表，**AVC（H.264）的 codec 字段是 0**，根本进不了这个判断。

而 11-27 11:08 的触发者是微信小程序宿主 + 系统 VideoToolbox（`VTDecoderXPCService`），内容大概率是 AVC。也就是说：**这套守卫天然挡不住这类事故**，这是设计覆盖面的缺口，不是守卫失效。另外本机运行构建里没有 `NRedDecodeGuardRevision` 属性，守卫是否已经装进 `AMDRadeonVADriver2` 尚无证据，需要按 `docs/decoder-mode-detection.md` 的口径复测一次。

---

## 6. 修复方案（按代价从低到高）

### 6.1 立即止血（零代码，下次开机生效）

14.x 卷的 OpenCore `config.plist` 中删掉 `-NRedVideoFix`：

- 效果：VCN 桥接关闭，`VCN0Dec` 通道不再存在 → 第 [3] 步没有提交、第 [4] 步没有复位、第 [5] 步没有死锁。
- 代价：没有硬解/硬编，视频全部 CPU 软解；同时 FairPlay/DRM 视频能力一并关闭（`docs/boot-arguments.md` 明确 FairPlay 随该参数启用）。
- 回退：把参数加回即可，不涉及 kext 版本。
- 注意：boot-args 真值源是测试 ESP 的 `EFI/OC/config.plist`，不是 NVRAM；改前先确认 ESP 身份（卷根 marker 文件），不要写死 `diskNsM`。

### 6.2 保留硬解时的补丁（需要编译，本机无 Xcode 无法验证）

把守卫从“HEVC 超规格”扩成“AVC/HEVC 全拒（可 gate）”：

1. `tools/vad_decode_guard.s` 增加一个符号，例如
   `_vad_decode_block_all: movl $3, %eax; ret`（3 是原函数内既有的失败返回值，语义安全：会话创建失败 → 播放器回退软解）。
2. `tools/build_decoder_guard.py` 同时汇编两份、输出 `Payload` 与 `PayloadBlockAll`（后者无跳回占位符，`PayloadBlockAllJumpOffset = 0`）。
3. `UserVideoDecode.cpp::installDecodeGuard` 接受 `(payload, jumpOffset)` 两个参数，`jumpOffset == 0` 时跳过 continuation 回填与幂等校验里的跳转比对；入口用 `checkKernelArgument("-NRedVcnSwOnly")` 选择 payload。

风险与收益：最坏情况是“所有硬解会话创建失败”，播放器一律软解（功能降级但不挂机），不会比现状更差；收益是把 `VCN0Dec` 从系统里彻底摘掉，同时保留其它 `-NRedVideoFix` 能力。

### 6.3 治放大器（属于 Apple accel 层，不在本仓库可改范围）

- [4] 改通道级 recovery：`PendingTS` 停止前进时只复位 video 通道，不整卡 reset。
- [5] 复位路径锁序：`AMDRadeonX5000` → `IOAcceleratorFamily2` 的顺序在 terminate 线程与 reset 线程之间必须一致。
- [2] 分配回落：固定区不可用时把 surface 分配到系统内存（GART）；以及 `orphanedReusable` 的回收阈值（现在 1.6 GB 挂着、hitRate 95%）。

这三条都需要 Apple 二进制层面的 RE 或 NootedRed 对 accel 层做 reimplementation，属于下一阶段工作，不适合在“今天止血”的范围里动。

---

## 7. 复现与取证命令

```bash
# 1) 复位历史：通道 + 客户端一次看完
for f in /Library/Logs/DiagnosticReports/Kernel*gpuRestart; do
  echo "$f | $(grep -m1 'Restart Channel' "$f") | $(grep -m1 'FirstPendingCB' "$f" | sed 's/^ *//')"
done

# 2) 关机停滞（spindump 二进制要先解析）
spindump -i /Library/Logs/DiagnosticReports/shutdown_stall_*_*.shutdownStall
grep -m3 "IOServiceTerminateThread\|lck_mtx_sleep" /tmp/spindump.txt

# 3) 现场显存/回收状态
ioreg -l | grep -o '"PerformanceStatistics" = {.*}' | tr ',' '\n' |
  grep -E "inUseVidMemory|vramFreeBytes|orphanedReusable|gartFreeBytes|TotalVideoRAM"

# 4) 分配失败爆发
log show --start "<事故前 1 分钟>" --end "<事故时刻>" \
  --predicate 'eventMessage CONTAINS "Failed to allocate"' --style compact
```

---

## 8. 与 dev-v2“显存优化”的关系（回答本轮提问，结论：**强相关，就是对症的那一条**）

指向 `personal/dev-v2` 的 `50b07e3 video: 显存分配回退与 allocPages 下溢修（-NRedVRAMAlloc / -NRedAllocPagesUnderflow，默认关）`（作者 hl-king，11-26 00:56，从 `dev(3044d9c)` 重放，只改 `X5000.cpp/.hpp`，216 行）。

它里面有三块，正好一一对应本事故现场：

| 补丁 | 机制 | 与本次现场的对应 |
| --- | --- | --- |
| `-NRedAllocPagesUnderflow` | `IOAccelMemoryAllocator2::allocPages` 算空洞对齐起点 `(holeStart + align - 1) & ~(align - 1)` 时可能越过 `holeEnd`，32 位无符号减法变成巨大值 → **整个非连续分配直接失败**，而不是跳过这个装不下的洞。把 `je` 改成 `jle`（在 `allocateNonContiguous` 里反查唯一那条八参数 `call` 来定位，prologue/特征码双重校验） | 就是 11:07 那段日志的读数：`There is 66293696 free memory remaining, and 3135434688 fixed-free memory remaining` —— 3.1 GB “fixed-free” 明明在，可用区只剩 66 MB，13.5 MB 申请却失败。这是空洞下溢，不是内存不够 |
| `-NRedVRAMAlloc` | 钩 `AMDHWMemory::allocateLargeBlocks`，改为五级回退：①非可见域连续 → ②可见域连续 → ③非可见域非连续（chunk 取次大 2 的幂、失败退半、再退块粒度）→ ④全 UMA 域（APU 上可见/不可见是同一块系统内存，吃掉可见域那段碎片洞）→ ⑤16 KB / 4 KB 细粒度非连续。并在入口先用 `totalFreeMemory` 快速失败，避免 evict 小缓冲刷日志 | 让 13.5 MB 这种“中等尺寸”申请在碎片化时也能落地，而不是连环失败后一路把 GPU 拖进 reset |
| 同提交里的 VCN 能力门 | `setupAndInitializeHWCapabilities` 里 `hasVCN0Field = VCN::allocated()`，不再用 `VCN::ready()`；`allocateHWEngines` 只在引擎真正分配成功后才写槽位并置位 | 提交正文写得很直白：**空槽位会让 VideoToolbox 以为媒体能力可用，登录期一提交就是 VCN0Dec 超时 + GPU reset（UI 卡死）** —— 这正是本机 20/20 复位报告的形态 |

两个开关都是**默认关**，且必须显式进 boot-args 才生效；同时这条只在 `personal/dev-v2`，`personal/master` 与 GitHub `origin/master` 都不含（`git grep NRedVRAMAlloc master` 为空）。

本机现场判定：运行中的 kext（`org.ChefKiss.NootedRed 0.9.0`）**不含这条补丁** —— 同提交里无条件写入的 `NRedVCNCapabilityFlag` 在 `ioreg` 中不存在，`NRedVRAMAllocEnabled` / `NRedAllocPagesUnderflowEnabled` 也不存在。所以：

- 现在加 `-NRedVRAMAlloc -NRedAllocPagesUnderflow` 到 boot-args **不会有任何效果**，因为代码不在这个构建里；
- 要吃这条优化，必须先按 249 纪律构建并部署 dev-v2 的 kext（一次只改一个变量，先只开 `-NRedAllocPagesUnderflow`，它是最小、最机械的一处：单条 `je→jle` 字节改写 + 双重特征校验，失败自跳过并落 `NRedAllocPagesPatched` 属性）。

评估结论：这条“显存优化”**帮得上忙，而且比摘 `-NRedVideoFix` 更治本**（摘参数是“不用 VCN 所以不炸”，这条是“让分配不再炸”）。优先级建议：先在 dev-v2 上只开 `-NRedAllocPagesUnderflow` 跑一轮，看 `Failed to allocate` 是否归零、gpuRestart 是否归零；有效再叠加 `-NRedVRAMAlloc`。

其它进过检索、但与本事故无关的改动：`03b9ed9`（VRAM 信息重实现，只给通道数/位宽）、`c957d96`（DMCUB 固定区，反而多占）、`11de379`（`getVisibleSize` 偏移修正，属可见域尺寸读错，与“空洞下溢”不是同一处）。

---

## 9. 待办

- [ ] **首选**：249/26 构建 dev-v2 kext（含 `50b07e3`），只开 `-NRedAllocPagesUnderflow` 跑一轮；判据：`Failed to allocate` 归零、（可选）`shutdown_stall` 归零
- [ ] 有效后叠加 `-NRedVRAMAlloc`；两个开关互不依赖，各自有 `NRed*Enabled` 属性可查
- [ ] 备用止血：14.x 卷去掉 `-NRedVideoFix`（零代码，观察 24 h：gpuRestart 应为 0）
- [ ] 若需保留硬解：6.2 的守卫扩围在 249 机器上编译 + 装机验证（`-NRedVcnSwOnly` 默认关）
- [ ] 6.3 三条放大器修复单独立项
