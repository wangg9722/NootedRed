# 26.7.3 复位风暴回归：VCN 能力门被剔除（2026-11-28）

对象机：`hl-king 的 iMac Pro`（249，macOS 26.7.3 / 25G342，Renoir APU）
触发动作：把 0.9.4（master + 显存分配补丁，**不含 VCN 能力门**）部署进该机 ESP 后重启。

## 1. 现象

重启后 UI 卡死（鼠标/窗口无响应），但机器没死：ssh 仍可登录，`uptime` 显示开机 2 分钟、load average 22。

## 2. 现场证据

- 1 分钟内产出 **8 份** `Kernel_*.gpuRestart`（19:08:22 → 19:09:13），随后 `WindowServer-2026-11-28-190903.ips` + `*.userspace_watchdog_timeout.spin`。
- 最新一份复位报告：`Restart Channel: 12 VCN0Dec`，`FirstPendingCB: PID=830, Name=VTDecoderXPCServ, SubmitContext=Video`，`PendingTS = 0x29, sent at 0:03:05, TS = 0xb`——**提交出去的时间戳永不 retire**，开机 3 分 12 秒时触发复位。
- WindowServer 判定：`unresponsive work processor(s): WindowServer main thread`，`5 total successful checkins`，`39 seconds since last crashed by watchdogd`。
- 两个补丁均已生效：`NRedAllocPagesPatched = 1`、`NRedVRAMAllocEnabled = 1`。
- **显存分配失败 0 条**——所以这次不是显存碎片问题，是 VCN 提交路径。

## 3. 根因

0.9.4 的代码谱系是「master + `50b07e3` 的第 1、2 部分」，第 3 部分（VCN 能力门）在 `0bd3723` 被**刻意剔除**，理由当时是"保持单变量、只看显存分配行为"。

被剔除的那段正是 26.x 的止血门：

- `wrapSetupAndInitializeHWCapabilities` 里 `hasVCN0Field = VCN::ready()`（master 口径）——只要 VCN 初始化过就对外声称"有 VCN0 编解码能力"。
- `allocateHWEngines` 里无条件把引擎槽位写进 pm4 引擎数组，即使 `allocateEngine()` 返回空。

后果：VideoToolbox 在登录期就认为硬解可用并提交解码命令，VCN0Dec 通道的 PendingTS 不 retire，驱动按超时整卡复位，10 秒级连发，最终 WindowServer 看门狗判死 → UI 卡死。这与 `50b07e3` 提交正文的判断完全一致（"空槽位会让 VideoToolbox 以为媒体能力可用，登录期一提交就是 VCN0Dec 超时 + GPU reset（UI 卡死）"）。

旁证：该机 11-21 也留下过 20 份同通道（`12 VCN0Dec`）的 gpuRestart，说明它对这条路径本来就敏感；14.8.9 那台因为 VCN 提交能正常 retire，所以同样的剔除没有暴露问题。

## 4. 处置

1. **现场回滚**：把 ESP 里的 `NootedRed.kext` 换回备份 `NootedRed.kext.bak-v094-20261128`（旧构建 `d719aaf4`，0.9.0），保证下一次开机不带着这个坑。
2. **修代码**：`1eae870` 恢复能力门——槽位仅在 `VCN::allocateEngine()` 成功时写入，`hasVCN0 = VCN::allocated()`，并把 `NRedVCNCapabilityFlag` 落属性，便于开机核对。
3. **提版本**：`dda914b` → `0.9.5`，md5 `3a660fa480d846c1ac60a5205bcc9aac`，UUID `3C4EB592-F9B7-381F-9C89-7215C1635988`，tag `v0.9.5-20261128-vcn-gate`。
4. **重新部署**：0.9.5 已写进 249 的 ESP（`disk0s1`），等下次开机验证。

## 5. 第二刀：能力门在 26.x 上是空转

回滚后重新读现场属性，发现一条决定性事实：**`NRedVCNEngineAllocated = 1`**。

也就是说在 26.7.3 上 `VCN::allocateEngine()` 里的 `engineClass->alloc()` 是**成功**的，`engineAllocated` 为真。而能力门的判据是 `VCN::allocated() = ready() && engineAllocated`，于是在这台机器上第一刀（`1eae870`）**会直接放行**，`hasVCN0` 仍然是 true，重启后必然再走一遍同样的复位风暴。

真正坏掉的不是"槽位空"，而是 26.x 的 VCN0Dec 提交路径：引擎对象有了，命令发出去，`PendingTS = 0x29` 永不 retire。这层没法靠能力门挡住。

据此加第二道门——按内核版本封顶（`49c03185`）：

```cpp
static bool vcnExposedOnThisOS()
{
    if (currentKernelVersion() < MACOS_26) { return true; }
    return checkKernelArgument("-NRedVCNOn26");
}
```

- `allocateHWEngines`：`>= 26` 且没显式开 opt-in 时，不分配 VCN 引擎、槽位留空、`hasVCN0 = false`，并落 `NRedVCNGatedByOS = 1`。
- `wrapSetupAndInitializeHWCapabilities`：`hasVCN0 = VCN::allocated() && vcnExposedOnThisOS()`。
- 代价明确：26.x 默认**没有硬解**，VideoToolbox 退回软解；要再做 bring-up 时在 boot-args 加 `-NRedVCNOn26` 即可恢复暴露，不用重新编译。

## 6. 版本状态

| 版本 | 提交 | 26.7.3 可用性 |
|---|---|---|
| 0.9.4 | `ee7df9e` | **不可用**：缺能力门，登录期 VCN0Dec 复位风暴 |
| 0.9.5 | `dda914b` | 不够：能力门在 26.x 放行（引擎分配成功），风暴照旧 |
| 0.9.6 | `49c03185` | 已部署待验证：能力门 + 26.x 版本封顶，默认不暴露 VCN0 |

14.8.9 那台目前在跑 0.9.4（无能力门但 VCN 提交正常 retire），暂无需改动。

## 7. 下次开机的判据

- `kmutil showloaded | grep NootedRed` → UUID `A25CB964-E32A-3814-89D8-A5B9EC1AB7D5`，版本 0.9.6
- `ioreg -l | grep NRedVCNGatedByOS` → `1` 表示版本封顶已生效（预期）
- `ioreg -l | grep NRedVCNEngineAllocated` → 应为 `0`（引擎不再分配）
- `ls /Library/Logs/DiagnosticReports/*.gpuRestart` → 开机后不应再新增，尤其不应再有 `Restart Channel: 12 VCN0Dec`
- 若仍出现同通道复位：说明有不走 `hasVCN0` 的提交路径（例如用户态补丁绕过了能力查询），下一步只能摘掉 `-NRedVideoFix` 彻底断开 VCN 桥接

## 8. 操作备注

- 硬断电后 ESP（`disk0s1`，FAT）会出现脏目录，`diskutil mount` 报 `Volume on disk0s1 failed to mount`。先 `sudo fsck_msdos -y /dev/disk0s1` 修复再挂载，否则部署脚本会在空 `$mp` 上静默失败。

## 7. 回滚

```bash
sudo bash -c 'diskutil mount disk0s1; mp=$(mount | grep "^/dev/disk0s1 on " | sed "s/^.* on //; s/ (.*$//"); rm -rf "$mp/EFI/OC/Kexts/NootedRed.kext"; cp -R "$mp/EFI/OC/Kexts/NootedRed.kext.bak-v094-20261128" "$mp/EFI/OC/Kexts/NootedRed.kext"; diskutil unmount disk0s1'
```
