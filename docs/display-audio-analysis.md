# 外接显示器（Type-C/DP、HDMI）音频无声分析

环境：work1，macOS 14.8.9 (23J631)，AMD Renoir/Cezanne iGPU（显示 0x15E7，HDAU `1002:1637`），
显示器 `Sculptor` 1920x1080@60，连接方式 `Connection Type: Thunderbolt/DisplayPort`。

## 一、已确认的现场事实（ioreg + CoreAudio + 内核日志）

1. **驱动链路完整，不是「设备没起来」**
   - `AppleGFXHDAController@0,1`（provider `pci1002,1637`，`hda-gfx=<"onboard-1">`，iGPU 侧同为 `onboard-1`）
     → `AppleGFXHDADriver@0,1,0` → `AppleGFXHDAEngineOutputDP` → `AppleGFXHDAStream`；
   - NootedRed 的 hook 已生效（控制器上存在 `NootedRedGFXHDA = <00>` 属性）。
   - 系统音频列表里出现 `Sculptor`（`Transport: DisplayPort`，2 输出声道，48 kHz），
     说明 **ELD 已被正确解析**（显示器名字都来自 ELD）。
2. **播放时音频引擎确实在跑（不是「没提交」）**
   - 用 `afplay` 强制走该设备（默认输出切到 `Sculptor`，id 110）：
     `IOAudioEngineState = 1`，`AppleGFXHDAStream` 出现 `IOAudioStreamNumClients = 1`，
     内核日志有 `startDMAEngine` / `stopDMAEngine` 成对出现，**全程 0 错误**；
   - 流格式协商成功：24bit/48kHz/2ch（可选 16/20/24bit，32–48kHz）。
   即：CoreAudio → IOAudioFamily → AppleGFXHDA 这一段完全正常。
3. **音量不可调是「数字输出」的设计行为，不是故障**
   - 该引擎只挂了 `AppleGFXHDAAudioSelectorControlDP`（且 `!registered, !matched`），
     没有 `IOAudioLevelControl`；DP/HDMI 显示音频在 macOS 上是固定电平，
     音量由显示器一侧（OSD/遥控）控制，菜单栏音量键对它是灰的。这一点在 26/14、Intel/AMD 上一致。
4. **上游已知问题，至今无解**
   - ChefKiss/NootedRed 官方 FAQ：“My external display's audio appears, but no audio comes out of it — Known issue.”
   - GitHub issue #225 `HDMI/DisplayPort usually has no audio playback despite being visible in settings`
     （Label: `Status/TriageRequired`，里程碑 v1.0.0，2024-02 开，2026-05 仍在改标题）：
     维护者明确表示「已试过 AppleALC layout、换线、改音量、去掉 AppleALC，全部无效，原因未知」，
     只出现在**少数机型**上。
   - 该 issue 里有一条有价值的用户线索：多接口机器上**只有某一个接口有声**，
     换插槽后有声 → 疑似帧缓冲/音频实例存在 **off-by-one**。
     本机 DP Alt-Mode（Type-C）与 HDMI 均无声，符合「受影响机型」范围。

## 二、结论分层

| 层 | 状态 | 证据 |
|---|---|---|
| 显示器 EDID/ELD 与音频能力上报 | OK | `Sculptor` 名字、2ch 48k 出现在设备与引擎 |
| HDAU PCI 绑定 / `hda-gfx` 配对 | OK | 两侧均 `onboard-1`，控制器 & 驱动已 match |
| AppleGFXHDA 引擎创建与 IOAudioFamily 通路 | OK | 引擎 state=1、stream 有 client、DMA 起停无错误 |
| 蓝牙/AUX 等在 HDA controller 上的通道 | OK | `startDMAEngine` 正常 |
| **AFMT/音频 infoframe 到 DP 链路（DCN 侧）** | **疑点** | 引擎在跑但显示器无声，说明数据没被显示链路真正发出/或未使能 |
| 显示器本体音量/静音/输入源 | 未验证 | 需 OSD 侧确认 |

也就是说问题**不在我们这套视频解码/显示补丁的常规路径上**，而是
「AppleGFXHDA 把音频喂给 AMD 显示控制器（AFMT）→ DP 音频包」这一段，
与上游 issue 描述一致。

## 三、下一步实验（按代价从低到高）

1. **排除显示器自身**：用同一根线接别的信号源（手机/盒子/另一台电脑）确认内置扬声器能出声；
   OSD 里确认音量非 0、非静音，输入源选 DP；换一根确认支持音频的线（部分 Type-C 线只有 USB2+DP，无音频通道）。
2. **换接口对比**：Type-C DP Alt-Mode 与 HDMI 各测一次（本机两种都无声，但可确认是否只有某一路受影响）。
3. **换系统对比**：同一根线、同一个显示器，boot 进 26.4 分区测一次。
   若 26.4 有声 → 是 14 上 AppleGFXHDA（340.1）与显示驱动的差异，可直接按版本差异定位；
   若都无声 → 结构性问题，走第 4 步。
4. **排除我们的显示补丁**：14 上加 `-NRedNoSubmitGuard`（SubmitGuard 关）测一次，
   再加 `-NRedNoDecodeGuardLegacy` 已有默认关闭，确认不是 PM4 重写导致的 AFMT 下发丢失。
5. **需要时才做的侵入式实验**：给 `AppleGFXHDA` 加调试输出，dump
   （a）controller 的 stream 索引/entry 基址，（b）codec widget 的 converter 索引与 stream tag，
   （c）DP 侧 AFMT 实例号。目标是验证上游那条「off-by-one」线索：
   若引擎实际写的是 AFMT0 而当前活动链路在 AFMT1（或相反），把实例索引对齐即可修复。

## 四、已写入的复现工具

`tools` 之外的一次性小工具（未入库，源码见下，需要时再入库）：

`audioout.c`：列 CoreAudio 输出设备 / 切换系统默认输出 / 查询当前默认输出。
用于在没有 SwitchAudioSource 的机器上把默认输出指向显示器并跑 `afplay` 做闭环验证：

```
xcrun clang -O2 -o audioout audioout.c -framework CoreAudio -framework CoreFoundation
./audioout list
./audioout set Sculptor      # 或 set <deviceID>
./audioout get
```

配合的观测点：`ioreg -c AppleGFXHDAEngineOutputDP | grep IOAudioEngineState`、
`AppleGFXHDAStream` 的 `IOAudioStreamNumClients`、内核日志里的 `startDMAEngine/stopDMAEngine`。
