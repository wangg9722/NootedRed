# 衍生参考项目（外部参考源）

本仓库是 NootedRed 的自研分支。下列第三方衍生仓库作为**只读参考源**：只用于对照实现思路、定位差异，不直接向本仓库合并他们的代码，也不引用其发行包。

快照时间：2026-11-23（GitHub 元数据实取）。

## 仓库清单

| 仓库 | 与我方关系 | 默认分支 | 最近推送 | 最新版本 / HEAD |
| --- | --- | --- | --- | --- |
| [vithalchowdary/NootedRed](https://github.com/vithalchowdary/NootedRed) | 主参考源（VCN1/Raven2 全链路实现，含动态签名后端） | master | 2026-11-22 | tag `v1.3.1` + 11-22 追加 8 提交 |
| [weachy/NootedRed](https://github.com/weachy/NootedRed) | 同一 1.3.x 线，更新最勤 | master | 2026-11-23 | 刚 bump 到 `1.3.2`，tag 至 `v1.3.1` |
| [steve12311/NootedRed](https://github.com/steve12311/NootedRed) | 0.9.x 预发布线，偏视频解码能力扩面 | master | 2026-11-18 | `v0.9.0-pre-release.19`，HEAD 为「VP9 Profile 2 硬解」 |
| [Kingtous/NootedRed](https://github.com/Kingtous/NootedRed) | 0.9.x 线，偏用户态补丁策略 | master | 2026-10-25 | 无 tag，HEAD 为「Unify FairPlay clients and preserve native sync」 |
| [xiaot-evo/NootedRed](https://github.com/xiaot-evo/NootedRed) | 0.8.x/0.9.x 线，带独立 UserPatch 组件 | master | 2026-11-18 | `v0.9.3`，附 `NootedRedUserPatch 0.3.2` |
| 自研 `NootedRead`（内部，非公开） | 我方 VCN 骨架干净实现 | `nred/vcn-skeleton-clean` | — | 硬解未完成，但启动路径从不挂死 |

本地参考克隆约定放在 `reference/<user>-NootedRed`（当前已有 `reference/NootedRed-vithalchowdary`），保持只读，不提交、不改动。

## 取用方式

clone 走本地代理（GitHub 直连偶发失败）：

```bash
export https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890 all_proxy=socks5://127.0.0.1:7890
git clone --depth=50 https://github.com/<user>/NootedRed.git reference/NootedRed-<user>
```

只查更新、不拉对象时用远端探测（同样先设代理）：

```bash
git ls-remote --symref https://github.com/<user>/NootedRed HEAD
git ls-remote --tags https://github.com/<user>/NootedRed | grep -v '^{}' | tail
# 或走 API：https://api.github.com/repos/<user>/NootedRed/commits?per_page=N
```

> 注意：本机 macOS 无 `timeout` 命令，脚本里别用；`git fetch` 大仓库可能长时间无输出，放后台跑并读输出。

## 查阅原则

1. 某风险点若**在旧分支就存在**且**参考项目也存在**，大概率不是问题源，优先级往后排；只有我方独有偏差才优先处理。
2. 参考实现只取「机制与约束」，不取逐字机器码：他们的页偏移/立即数绑定具体 OS build，我方要按版本自适应。
3. 只读参考，不引入其发行 kext。

## 2026-11-22 参考源更新中确认值得学的点

来自 `vithalchowdary` 的 8 个新提交（`b6937493`、`ee4590a5`、`060c2126`、`e630f470`、`2f952e12`、`72392609`、`52158395`、`86e29c62`）：

- **initHardware 失败要撤回能力位**：`VCNBridge::initHardware` 失败时清 `engineAllocated`/`loadCompleted`，并 `setVCN0Capability(hw, false)`，同时打属性 `NRedVCNLastHardwareInit`。我方目前只打 flag，未回收能力位。
- **跨版本动态签名替代按版本硬编码**：VCN1 寄存器重映射改成「掩码上下文 + find/replace」签名（`VDASignatures.inc` 里 `kVcn1DecodeRegisterSignatures`），macOS 13 与 14–26 的寄存器准备指令差异用 6 字节通配掩码吸收，boot-arg 统一到 `isVcn1Mode()`（Raven/Picasso/Raven2 共用），decode 不再要求 OS build 字符串精确匹配（encode 仍要求）。这正是我方「预生成 26.0 机器码在 26.7.x 失效」的解法方向。
- **页缓存补丁的时机是硬约束**：`_cs_validate_page` 对每个物理页只在进入全局 shared-cache vm_object 时调一次，所以**不能**按进程身份或加速器启动时机决定跳过，否则该页以未打补丁状态进全局缓存、本次开机再也补不上。我方走的是私有映射（每进程各自 COW 改写），此约束不直接适用，但一旦回到「改全局页缓存」路线就必须遵守。
- **私有共享缓存补丁器（`b6937493`）后来被移除**：master 最终回到页缓存 + 动态签名路线，所以不要照搬；其中可取的只有「按 map 记录就绪状态 + `_vm_map_destroy` 失效 + 子进程 verifyOnly 复核」这套生命周期管理思路。
- **音频三处小修（`2f952e12`）**：内屏是否**在用**要看 `IOFBCurrentPixelClock != 0`（而不是仅看存在 LVDS 连接）；`extOtg` 用 `0xFFFFFFFF` 哨兵、找不到启用的外部 OTG 直接返回失败（原写法会默认落到 OTG0）；DTO 复用判据要读 `DccgAudioDtoSource`（source 必须等于 `extOtg` 且保留位为 0），不能只看 module/phase。
- **HEVC Main10 编码与 slice delta 签名**限定 Ventura/Sonoma 放开（`72392609`）；工具链脚本要求先确认 kext 已加载、`-NRedOff`/`-liluoff` 未生效再判定（`52158395`）。
