# 版本选择矩阵：14 / 15 / 26 与各自小版本是怎么被区分的

本文件回答三件事：**哪一层负责区分版本、按什么凭据区分、小版本变动时会发生什么**。
数据来源为仓库现有实现（`NootedRed/KernelVersion.hpp`、`UserLegacyCompat.hpp`、
`UserCacheResolver.hpp`、`UserSurfaceSync.cpp`、`UserLegacyData.hpp`、`UserSurfaceSyncData.hpp`）。

## 一、三条互不相关的判定通道

| 通道 | 代码入口 | 区分凭据 | 小版本变化时的行为 |
| --- | --- | --- | --- |
| 内核侧驱动实现 | `currentKernelVersion()`（`X5000.cpp`、`HWLibs.cpp`、`Backlight.cpp`、`VCNCapabilities.hpp` 等） | Darwin **major** 号（14/15/26），少量点版本（`MACOS_14_4`、`MACOS_13_4`） | 按 major 切换常量/函数实现；未识别的 major 直接拒绝。点版本差异只在已实测过的位置（如 `hwChannelSubmitCommandBuffer = 0x188`） |
| 14 / 15 用户态补丁 | `UserLegacyCompat::matches()` → `Profile` | **缓存 UUID**（14 = `4C1441FD…` / 15 = `42EC2890…`）+ 最多 16 个页面、每页 4096 字节**逐字节**核验 + 补丁表（≤32 条） | UUID 不一致 → 整组拒绝。也就是说 **14 只支持 14.8.9，15 只支持 15.8.1**，其他小版本一律不生效，且不会误打 |
| 26 用户态补丁 | `UserCacheResolver::resolve()` → `ResolverImages/Functions/Locations` | **代码内容**：image 的 path + text 大小 → 整段函数字节匹配（rel32 位移位置豁免）→ 函数内补丁偏移 → 候选唯一性（≤128 个） | 不认版本号。函数字节没变就照打；字节变了就匹配失败 → 拒绝并写入诊断码。**因此 26.x 小版本之间靠"代码是否还长这样"区分，而不是版本号** |

## 二、26 路径的判定链与诊断码

1. `UserSurfaceSync::init()` 先在 `UserSurfaceSync.cpp:543` 决定能力组合：
   `computeScratch = srdShared = submitGuard && majorMatches(MACOS_26)`；
   `legacyBlend = submitGuard`（14/15/26 都开）。
2. 设备与版本前置（`UserSurfaceSync.cpp:558-569`）：
   - 设备不在 `findDevice()` 表内 → `NRedImmediateSyncRejected=3`
   - 非 26 且没有 14/15 profile → `NRedImmediateSyncRejected=4`
3. `UserCacheResolver::resolve()` 逐级核验，失败写 `diagnosticStage()`：
   - `0x10+i`：第 i 个 image 的 path/size 或段布局不符
   - `0x100+i`：第 i 个函数整段字节不匹配（或候选不唯一/超过 128）
   - `0x200`：两个已解析函数的地址区间重叠
   - `0x300+i` / `0x400+p` / `0x420+p` / `0x430+p` / `0x440+p`：相对引用、补丁位置、补丁区间、位移重写失败
4. 14/15 profile 缺失或 UUID 不符时，走 `UserLegacyCompat::matches()`，任何一页任何一字节不同即拒绝。

## 三、实测证据：小版本之间到底差在哪

用 `tools/blend_gate_probe.py` 在两套系统上直接比对 ctor 内的能力位门：

| 项目 | macOS 14.8.9（23J631，开发机） | macOS 26.7.3（25G342，249） |
| --- | --- | --- |
| image base | `0x7ffb034f1000` | `0x7ffb10c50000` |
| ctor rva | `0x5afd4` | `0x286ee` |
| ctor 内能力位检查 | 1 处，`ctor+0x11e1` | 1 处，`ctor+0x100c` |
| 门指令 / 原始字节 | `test ..,2 ; jne` → **`75 0b`** | `test ..,2 ; je` → **`74 28`** |
| 形态 | `caps1_skip`（能力位为真才跳过） | `caps0_skip`（能力位为假就跳过计算） |
| 跳转目标 | `cmp dualSource; setne cl; bt [..],0x22` | `mov ecx,[r12+0x28]; and 0xFFFFFFFE; or ecx,eax; mov back` |

结论：**同名函数、同一条"能力位检查"，在两代系统上控制流方向是相反的**。
所以 14/15 的 `75 0B → EB 0B` 不能搬到 26；26 上把该门 NOP 掉会退化为
"对所有 pipeline 强制禁用双 quad"，这正是历史上出现 `FenceNotRetired / FullAsicReset` 的形态。

同时核实：仓库 26 模板里写死的那三条地址（`0x310D71A28`、`0x310D71AFF`、`0x310C77708`）
在 26.7.3 上全部落空（分别命中无关函数中段、数据对齐区、另一函数尾部）。
需要说明的是它们**并非死数据**：`UserSurfaceSync.cpp:313` 的判据是
`dynamic = !legacyProfile && memcmp(uuid, CacheUUID) != 0`，也就是说
**只有缓存 UUID 与 26.0 参考缓存完全一致时才走静态表**，其它 26.x 一律走 `resolve()`，
后者用 `output[p] = {address - base, …}` 重新计算写入位置。
因此这些地址在 26.7.3 上落空是预期行为，但保留它们仍有一处隐患：
表里写死的绝对偏移很容易被后来的人当成"当前系统的锚点"照抄，实际应当
只由 `resolve()` 从"函数内偏移"派生。

## 三点五、本轮的门形态改造

26 的门是 `caps0_skip`：能力位为假就跳过 `dualSource || !allowRBPlus` 的计算。
因此 `90 90`（NOP 门）会退化成"allowRBPlus 恒为假 → 对全部 pipeline 禁用双 quad"。
本轮把补丁改成两条协同一体的写入：

| 补丁 | 位置 | 原始字节 | 补丁字节 | 作用 |
| --- | --- | --- | --- | --- |
| BlendGate | ctor + `0x101a`（26.0 静态表 `0x310C77708`） | `74 28` | `90 90` | 放行门，让能力位为假时也进入计算 |
| BlendAllow | ctor + `0x103c`（26.0 静态表 `0x310C7772A`） | `0F 94 C1`（`sete cl`） | `31 C9 90`（`xor ecx,ecx`） | 让 `allowRBPlus` 取反结果恒为 0 |

两条合起来的结果是 `DISABLE_DUAL_QUAD = dualSource`：只有双源混合才禁用双 quad，
普通混合继续使用原生优化，与上游文档意图一致，也不再触碰
`FenceNotRetired / FullAsicReset` 那条路径。

`tools/blend_gate_probe.py` 会同时校验这两个位置：门形态、跳转目标是否为
`CB_COLOR_CONTROL` 写回序列、`sete` 处是否为 `0F94C1`。任一不符则退出码 1，拒绝打补丁。

## 四、由此得到的维护原则

1. **内核侧只看 major，不要引入对小版本号的硬编码推断**；确实需要点版本差异时，要有实测依据。
2. **14/15 的 UUID 是冗余前置**：整页逐字节核验本身已经是内容校验。UUID 不匹配但页面全一致时，
   代码与参考构建等价，可以放行并把 UUID 仅作为日志记录——这是解锁 14.x/15.x 其他小版本的低风险入口。
3. **26 的地址必须由 `resolve()` 派生，静态表只作为 26.0 基线保留并明确标注适用范围**：
   表里的字节与 location 才是补丁本体，绝对偏移只对 UUID 完全一致的原版缓存有效。
   任何新增补丁都应以"函数 + 函数内偏移"的形式进入 resolver，而不是再加一条绝对地址。
4. **新增/修改任何用户态补丁前，先跑 `tools/blend_gate_probe.py`**：它会在目标系统上判定门形态、
   验证目标是否为 `CB_COLOR_CONTROL` 写回序列，形态不符即退出码 1，从源头拦住"打错位置"。
5. 26.7.x 的 §三 证据说明"小版本一般只做增量"这一点在**函数布局**上基本成立（ctor 首 32 字节与
   门偏移 `0x101a` 在 26.7.3 依然吻合），漂移主要发生在**绝对地址与预生成机器码数据**上，
   这也是为什么"按内容匹配 + 函数内偏移"是唯一可持续的 26 路径。

## 六、resolver 的硬依赖必须跟着策略开关走

`resolve()` 里每一处 `findFunction()` 都对应一条补丁族。曾经的写法是

```cpp
if (!scratchMode && !findFunction(..., ScratchOriginal, ...)) { reason = 32; return false; }
```

意思是"即使不打 scratch 也要先确认 Scratch 函数形状"，作为基线锚点。这条假设在 26.7.x 上不成立：
`ScratchOriginal`（452 字节，26.0 形状）在 26.7.3 缓存里 14 处字节已经不同，于是整个 resolver 直接
在 reason 32 退出，**连带 LegacyBlend 的门一条都没打上**（`ioreg` 表现：

```
NRedImmediateSyncDynamicRejected = 1
NRedImmediateSyncDynReason = 32
NRedLegacyBlendApplied = 0
```

而所有其它检查其实都是通过的）。修法是让这条检查只在 `scratchMode` 下执行，即"某条策略的
前置匹配只在该策略开启时生效"。修完后同一台机器实测：

```
NRedLegacyBlendApplied = 16
NRedLegacyBlendWindowServerApplied = 1
NRedImmediateSyncDynamicApplied = 1
```

原则：**补丁族的形状校验属于该补丁族自身，不要拿来当全局前提。** 一个策略关掉时它既不该写字节，
也不该成为别的策略能否工作的条件。

## 七、装机前的离线自检

`tools/verify_surface_sync_bytes.py` 会把 `UserSurfaceSyncData.hpp` 里的字节数组逐段与目标缓存比对
（瞬态 rel32 位置豁免），在重启之前回答"resolver 能不能走到打补丁这一步"。26.7.3 上当前结果：

| 补丁点 | 长度 | 结果 |
|---|---|---|
| Init(`Original0`) | 215 | 完全一致 |
| Override(`Original1`) | 10 | 完全一致 |
| NativeOverride | 923 | 完全一致 |
| BlendConstructor | 8766 | 57 处差异，在 96 字节差异预算内，前 32 字节严格一致 |
| Scratch(`ScratchOriginal`) | 452 | 14 处差异，仅 `-NRedScratch` 会因此拒绝 |

凡是出现"整段一致"之外的差异，都要在装机前跑这个脚本，而不是靠重启来验证。

## 八、延伸阅读

各补丁族在 26.4 / 26.7.3 上的差异量对比（26.4 为 116 处，是当前漂移最大的小版本）、
Scratch 族缺少小版本容忍度的具体数据，以及 26 独有 compute 挂死（关闭 4K 预览卡顿）的排查结论，
见 [tahoe-26-minor-version-drift.md](tahoe-26-minor-version-drift.md)。
