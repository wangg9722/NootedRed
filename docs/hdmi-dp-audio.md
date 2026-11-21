# 外接 HDMI / DP 音频（DCN 侧管线编程）

## 问题

上游 issue #225：外接显示器的音频设备（AppleGFXHDA 枚举出的 HDMI/DP 输出）可见、可选中，
但播放无声。

原因不在 HDA 控制器侧，而在 DCN 显示侧的音频管线没有被配置：

| 环节 | 作用 | 未配置时的表现 |
| --- | --- | --- |
| DCCG Audio DTO（`DCCG_AUDIO_DTO_SOURCE` / `DTO0_PHASE` / `DTO0_MODULE`） | 音频时钟由像素时钟派生 | 音频包时基不对，链路上没有有效时戳 |
| Azalia endpoint（`AZF0ENDPOINT` 的 hot-plug / speaker / descriptor 寄存器） | 声明连接类型（HDMI / DP）、声道分配、LPCM 描述符 | 端点在链路层「有声但静音」 |
| DIG AFMT（`DIG0_AFMT_*`） | 封装音频信息帧 / 60958 包、打开通道使能 | 没有音频包输出 |
| DP_SEC（`DP0_DP_SEC_CNTL` / `DP_SEC_AUD_N` / `DP_SEC_TIMESTAMP`） | DP 音频流使能（STREAM / ASP / ATP / AIP） | DP 口完全无声（HDMI 口不走这条） |

## 设计（事件驱动，无定时器）

模块**没有初始化入口**：是否启用每次按 boot-arg 现算（纯函数、无状态），因此不存在「谁的 init 先跑」这类顺序依赖。

模块 `NRedAudio`（`NootedRed/NRedAudio.cpp`）只挂在**既有**的驱动调用点上，不新增函数路由、
不启动轮询线程、不做按固定时长等待的重试：

| 事件 | 调用点 | 动作 |
| --- | --- | --- |
| GPU / framebuffer 上电 | `X6000FB::wrapControllerPowerUp` | **零 MMIO**：只置一个内存状态位 `NRedAudioPowerUpDone`。该函数与 HW 引擎分配同处 GPU 上电链 |
| 音频控制器 match 成功 | `AppleGFXHDA::wrapProbe`（上游既有的钩子） | 真正的寄存器访问点：先检查「GPU 已上电」，再按外接 fb 的 `IOFBCurrentPixelClock != 0` 判断链路已定型，最后配一次 |

登录期路径（GPU 上电、`reportCapabilities_LinkInfo`、`dp_receiver_power_ctrl`）上音频**不做任何寄存器访问**。
DP 链路训练钩子只保留原函数调用，不插延时。


配置函数本身是**幂等**的：已经处于目标状态的寄存器不再写（例如 AFMT 正在给正确 endpoint
打音频时只刷新 DTO 时钟）。因此重复事件不会造成反复写。若当前还无法配置（外接 framebuffer
未激活、DIG 后端未使能），只记 `NRedAudioPending=1` 并返回 `false`，等下一个自然事件再试——
整个模块里**没有任何 sleep / thread_call / 延时轮询**。

## 默认行为

同一份代码在 14 / 15 / 26 上默认启用，**不需要任何 boot-arg**。

| 参数 | 作用 |
| --- | --- |
| （无参数） | 完整编程（DTO + Azalia + AFMT + DP_SEC） |
| `-NRedNoAudio` | 整体关闭本模块 |
| `-NRedAudioProbe` | 只读诊断：只发布寄存器快照，绝不写寄存器 |

参数命名遵循「否定开关前缀在前」的约定（`-NRedNo*`），避免与其它 `-NRed*` 参数发生
`PE_parse_boot_arg` 的前缀吞并。不设分段开关：早先为逐段二分加过 `-NRedAudioAz/-Afmt/-DpSec/-Dto`
与阶段掩码，默认全开之后它们在正常路径上恒等于全开，属于调试脚手架，已删除。

## 诊断属性

`ioreg -c NootedRed` 里可直接读：

| 属性 | 含义 |
| --- | --- |
| `NRedAudioMode` | 0 关闭 / 1 只读 / 2 启用写入 |
| `NRedAudioPending` | 1 = 上次事件点还不具备配置条件，等下一个事件 |
| `NRedAudioPasses` | 成功配置的累计次数 |
| `NRedAudioCfgDIG` / `CfgOTG` / `CfgEP` / `CfgDP` | 实际选中的 DIG / OTG / Azalia endpoint / 是否 DP |
| `NRedAudioRegionInvalid` | 1 = 基址读到全 1，判定 IP 块地址不成立，放弃写入 |
| `NRedAudioFB<n>Conn/Clk/Codec`、`NRedAudioDIG<n>*`、`NRedAudioAZ<n>*`、`NRedAudioDTO*` | 只读拓扑快照 |

## 致谢

`dp_receiver_power_ctrl` 挂钩**不再承载任何音频动作**（只保留原函数调用）：该函数位于 DP 链路训练链上，在其中插延时或访问 DCN 寄存器会破坏链路建立时序。

寄存器序列、事件点选择（framebuffer 上电 + `reportCapabilities_LinkInfo`）移植自
vithalchowdary/NootedRed 的 HDMI/DP 音频修复分支。上游 ChefKiss/NootedRed 没有音频模块
（issue #225 仍为已知未修问题），因此本模块全部代码在本仓库内新增。

与原实现的差异只有两处，均为本仓库约束所致：上电阶段改为只读（原实现在上电阶段直接写）、
以及去掉了原实现里的固定延时轮询（改为事件驱动，失败只记 `NRedAudioPending`）。
