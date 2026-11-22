# 代码风险全面分析（2026-11-23）

基线：`master f2d8989`（tag `v0.9.0-20261123-audio`，视频 + HDMI/DP 音频）。
范围：`NootedRed/` 全部驱动侧源码（HWLibs、X5000、X6000FB、VCN、NRedAudio、AppleGFXHDA、
UserVideoDecode、UserSurfaceSync、DebugEnabler、Backlight/ECBacklight、NRed）。

本文只做记录与排序，不含改动。每一项都给出证据位置、机制、影响、判据、修复方向。

---

## 0. 结论摘要

当前线上偶发故障（登录后 45s~2min 内 `VCN0Dec` 环超时 → WindowServer 卡死）的候选成因集中在
「**硬解引擎是否真的就绪**」与「**MMIO 间接访问无锁**」两条线上。全部风险条目里，P0 三项与
该症状直接相关，P1 四项里有三项是确定缺陷或明显不合理的时序处理。

排序原则：与该症状关联度 > 一次改错的爆炸半径 > 修复成本。

---

## 1. 已排除的假设（有证据，勿再重复排查）

| 假设 | 证据 | 结论 |
|---|---|---|
| 显存分配器改动引入卡死 | 11-23 00:36（`b174c184`，改动前）与 01:11（`1b1ab850`，改动后）两份 gpuRestart 均为 `Application: VTDecoderXPCServ` / `Restart Channel: 12 VCN0Dec`，uptime 分别 1:10 / 1:28 | 与显存分配路径无关 |
| 某一次构建引入 | 11-22 04:21 起至 11-23 01:11 共 11 份 gpuRestart 全部 `VCN0Dec`，uptime 集中在 45s~2:20 | 长期存在，非单次引入 |
| 解码守卫未装 | 各 boot 均为 `NRedDecodeGuardApplied=16` / `OffsetIndex=1` / `Rejected=4` | 守卫已装，问题不在守卫缺失 |
| md5 可用于标识提交 | 同一棵树重建只有 `LC_UUID` 差异：`b174c184` 与 `b31b5b91` 逐字节 48/2882656 字节不同，集中在 `LC_UUID` 与签名区 | md5 不是提交标识，只能用 `git rev-parse` + 逐字节比对 |

---

## 2. P0：与 `VCN0Dec` 超时 / 界面卡死直接相关

### P0-1 `hasVCN0` 双赋值顺序缺陷

- 证据：`NootedRed/X5000.cpp:412-421`（`allocateHWEngines`：引擎分配失败 → `hasVCN0Field = false`）
  与 `NootedRed/X5000.cpp:349`（`wrapSetupAndInitializeHWCapabilities`：`hasVCN0Field = VCN::ready()`）。
- 机制：两个函数都写同一个字段，且没有先后约束。若 `setup…` 在 `allocateHWEngines` 之后执行，
  `ready()` 为真时会把「引擎没分到」的 false 覆盖回 true。
- 影响：对外宣称有 VCN0，用户态（`VTDecoderXPCServ`）把解码提交给空引擎 → 环超时 → gpuRestart；
  报告形态与该症状完全一致（全通道 `CompletedTS/SubmittedTS` 为 0）。
- 判据：探针属性 `NRedVCNT07`（allocate 时 ready）与 `NRedVCNT09`（setup 时发布值）的时刻与取值。
- 修复方向：单一事实来源。`setup…` 不再自行推断，只读引擎实际分配结果；或在 `setup…` 中改为
  `hasVCN0 = allocated()`（要求引擎已存在），避免用 `ready()` 二次覆盖。

### P0-2 X6000 可选 kext 加载与引擎分配的竞态

- 证据：`NootedRed/VCN.cpp:48-73`（`acceleratorStart` 内同步 `OSKextLoadKextWithIdentifier`，
  随后以 `has5000 && has6000 && hooks` 判定 `loadCompleted`）；
  `NootedRed/VCN.cpp:270-277`（`ready()` 四条件闸门）；
  `NootedRed/VCN.cpp:299-303`（`allocated()` = `ready() && engineAllocated`）。
- 机制：`has6000` 只在 X6000 的 load 回调里置真。回调晚到、或该 kext 已是加载态而不再触发回调，
  都会让 `ready()` 为假或时真时假；而 `allocated()` 在用户态客户端创建时仍会再判一次，
  因此 `newVideo()` / `clientStart()` / `signalWork()` 可能在同一进程内落到不同实现上。
- 影响：引擎未分配 → 解码提交到不存在引擎（同 P0-1 后果）；或 start/stop 实现不配对。
- 判据：`NRedVCNT03`（X6000 prepared）与 `NRedVCNT07` 的先后；`NRedVCNT12` 的 detail。
- 修复方向：把「kext 就绪」与「引擎已分配」解耦——`acceleratorStart` 在调用原实现前显式等待/确认
  `has6000`；或改为在 X6000 回调内直接触发后续流程，而不是靠 start 内一次同步返回值判定。

### P0-3 MMIO 间接访问无锁

- 证据：`NootedRed/NRed.cpp:232-250`。`readReg32` / `writeReg32` 在寄存器地址超出映射长度时，
  用 `PCIE_INDEX2` / `PCIE_DATA2` 两拍间接访问，两拍之间没有任何锁。
- 机制：音频 thread_call（`NRedAudio::configureAudio`）与显示 modeset / HPD 路径并发时，
  index 会被另一路覆盖，于是读到或写到完全无关的寄存器。
- 影响：寄存器被误写（含 DCN/AFMT/DP_SEC 这类有副作用的寄存器）→ 显示异常、音频异常、
  甚至链路训练失败；属于「写入时机错误」的机制性解释。
- 判据：并发压测下同一序列的读回值不稳定；加锁后消失。
- 修复方向：用一个 `IOSimpleLock` 串行化 index/data 两拍（无行为变化、可独立验证）。

---

## 3. P1：确定缺陷或明显不合理的时序处理

### P1-1 DP 电源回调里的固定延时

- 证据：`NootedRed/X6000FB.cpp:594-603`，`wrapDpReceiverPowerCtrl` 调完原实现后 `IOSleep(250)`。
- 机制：在 DP 链路电源/训练路径上阻塞 250ms，可能落在 HPD 工作线程上，与 modeset 竞争。
- 影响：热插拔/唤醒时卡顿；与音频写入同一路径时放大 P0-3 的竞态窗口。
- 修复方向：删掉固定睡眠，复用电音侧已有的 `scheduleDelayedProbe` 延迟机制。

### P1-2 `AppleGFXHDA` Widget 创建分支调错原函数

- 证据：`NootedRed/AppleGFXHDA.cpp:101-109`。非匹配分支调用
  `FunctionCast(wrapCreateAppleHDAFunctionGroup, orgCreateAppleHDAFunctionGroup)(devId)`，
  而该文件已有正确字段 `orgCreateAppleHDAWidget`（`AppleGFXHDA.hpp:81`）未被使用。
- 机制：用「创建 FunctionGroup」的原函数从「创建 Widget」的入口返回调用，返回对象类型不匹配。
- 影响：任何非 Raven/Renoir 的 AMD HDMI 设备走到该分支即返回错误类型对象指针（几乎必然崩溃）；
  当前因设备 ID 恰好命中匹配分支而未暴露。
- 修复方向：改为 `FunctionCast(wrapCreateAppleHDAWidget, orgCreateAppleHDAWidget)(devId)`。

### P1-3 `NRedAudio` 跨上下文共享状态无同步

- 证据：`NootedRed/NRedAudio.hpp:55-62`（`delayedProbeCall` / `settleRetries` / `postLinkPassDone`）；
  `NootedRed/NRedAudio.cpp:238-262`（thread_call 回调与调度）、`:268-286`（两个入口写状态）、
  `:300-410`（`configureAudio` 在其中做 MMIO 读写）。
- 机制：三个上下文（framebuffer 上电、DP 电源回调、thread_call）读写同一组字段与同一 thread_call，
  没有锁；`thread_call_allocate` 只在空指针时创建一次，回调内又调度自身。
- 影响：重试计数/二次 latch 判定错乱（表现为「有时没声音」）；极端情况下同一 MMIO 序列被并发执行。
- 修复方向：状态字段用 `atomic` 或 `IOSimpleLock` 保护；把「是否已调度」显式化，避免重复 enter。

### P1-4 音频写入与显示 modeset 无协调

- 证据：`NootedRed/NRedAudio.cpp:379-410`（AFMT / DP_SEC / DCCG DTO 写入）；
  `NootedRed/X6000FB.cpp:585-591`（`wrapControllerPowerUp` 内直接调 `onControllerPowerUp`）。
- 机制：参考实现也是在 powerUp / link 回调里触发，但我们的路径更长（powerUp 内即触发 + 250ms 睡眠 +
  循环重试），窗口更大。
- 影响：与 P0-3 叠加构成「上电阶段写入 vs modeset」竞态，是卡死风险之一。
- 修复方向：写入只在链路稳定后做一次（去掉回调内的同步触发），并把 MMIO 串行化（见 P0-3）。

---

## 4. P2：结构性风险（当前不怀疑，但需登记）

| 编号 | 位置 | 风险 |
|---|---|---|
| P2-1 | `X5000.cpp`（15 处）、`X6000FB.cpp`（29 处）、`HWLibs.cpp`（28 处）`PANIC_COND` | 符号/特征漂移即 panic；14.8.9 可过，26.x 风险显著 |
| P2-2 | `VCN.cpp:237-259`（`install`：在版本固定偏移写 `call signalGPUWorkSubmitted`） | 只校验 `original` 字节，偏移表漂移会改错指令 |
| P2-3 | `VCNKernelProfiles.hpp` / `UserLegacyData.hpp` / `UserVideoDecodeData.hpp` | 全表按内核版本硬编码，无版本即静默降级或 panic |
| P2-4 | `UserVideoDecode.cpp:298-306`（`shared_region_check_np` 钩子） | 缺「解析中闸门」；历史上 dyld 缓存读会缺页重入，需确认 `stage()` 之外没有重入保护 |
| P2-5 | `UserVideoDecode.cpp:184-207, 480-495`（`vm_protect` + 还原） | 多处忽略返回值，失败可能留下未还原的页保护 |
| P2-6 | `DebugEnabler.cpp:238-252`（`doGPUPanic`：`IOSleep(10000)` 后 `panic()`） | 命中即 10s 卡死再关机 |
| P2-7 | `UserSurfaceSync.cpp`（638 行用户态补丁，含 WindowServer 特判） | 风险面大，当前未被怀疑，改动前需单独评估 |
| P2-8 | `UserPatchPolicy.hpp:9-17`（FairPlay 判定 `IOUserClient::copyClientEntitlement`） | 在 `shared_region_check_np` 路径内调用，失败/超时行为未验证 |

---

## 5. 排查顺序（一次只推进一项）

1. **P0-1 / P0-2**：用探针分支 `debug/vcn-init-timing`（基于 `master`，提交 `704c0a2`，
   md5 `838dfe6e3fa88351d15fe3b5d74f96ba`）抓一次启动数据即可定性，不改逻辑。
   探针属性：`NRedVCNTnnC`（code<<8|detail）、`NRedVCNTnnM`（uptime 毫秒）。
2. **P0-3**：MMIO 间接访问加锁。纯防御、无行为变化，单独一版验证。
3. **P1-1**：去掉 `IOSleep(250)`，改为延迟回调。
4. **P1-2**：Widget 分支修正（零风险）。
5. **P1-3 / P1-4**：在 2、3 落地后复测「有时没声音」，若消失则不改。

## 6. 验证盲区

- 26.x（26.4 / 26.7）未验证：所有偏移与特征表只覆盖到 14 / 15 实测。
- HDMI 侧出声只验证过 DP，HDMI 未实测。
- 后接显示器（开机后再插）与热插拔重配未验证。
- `NRedAudio` 音量控件在 `AppleGFXHDA` 侧不可设属基线行为，与本清单无关。
