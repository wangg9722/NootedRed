# 无线镜像（AirPlay）CPU 高：走的是软编，不是硬解失效

- 日期：2026-11-28
- 机器：本机 MacBookPro16,2 / macOS 14.8.9（kext 0.9.0 + 显存补丁）
- 现象：用"无线镜像显示器"投屏时 `AirPlayXPCHelper` / `VTEncoderXPCService` CPU 很高，怀疑硬解没生效

## 1. 结论

镜像链路是 **抓屏 → 编码 → 打包发送**，没有解码环节，"硬解不生效"这个方向不成立。真正的问题是：

**AirPlay 要求"低延迟 H.264 硬件编码"，当前驱动的 AppleGVA 编码器建不起来，于是回退到软件编码 `AppleH264SW`。**

软编 1080p60 在 Renoir 上约吃 1–1.8 个核，叠加 WindowServer 的抓屏与色彩转换，就是用户看到的高占用。

## 2. 现场证据（镜像进行中实测）

进程归属与占用：

```
PID 61070  VTEncoderXPCService   107–176% CPU   ← 由 peer[159] AirPlayXPCHelper 在 21:53:29 创建
PID 162    WindowServer          70–85% CPU     ← 抓屏 + 缩放/色彩转换
```

`sample 61070` 的叶子热点全部落在 `AppleH264SW`（软件 H.264 编码器）代码段，不是 `AppleGVA`。

会话建立日志（`log show`，AirPlay 发起编码的时刻）：

```
VTEncoderXPCService: (AppleGVA) GVA error: AVDCreateGPUAccelerator: Error loading GPU renderer
VTEncoderXPCService: (AppleGVA) GVA encoder error: AVFQTXENC_CreateInstance fails with error -12915, unsupported usage 20
```

`-12915 / unsupported usage 20` = 该用法（低延迟实时编码）不被当前 GVA 编码器接受，随后 VT 在同一 XPC 进程里创建软编会话。

镜像参数：`FigVirtualDisplayProcessor` 报 `1920 x 1080`、`60 fps`、`EncoderDrops/sec: 0`（软编追得上，只是费 CPU）。

## 3. 能力对照实验

自写探针（`VTCompressionSessionCreate` + `kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder`），同一系统同一 GPU：

| 配置 | create | usingHW | 吞吐 / 备注 |
|---|---|---|---|
| H.264 `h264.gva`，普通实时 1080p | 0 | 1 | 120–156 fps，客户端 CPU ≈ 0 |
| H.264 `h264.gva` + `EnableLowLatencyRateControl` | **-12902** | — | 会话创建失败（正是 AirPlay 要的用法） |
| H.264 `auto` + `EnableLowLatencyRateControl` | **-12903** | — | 同上 |
| HEVC `hevc.gva` | **-12908** | — | HEVC 硬编不可用（列表里标 HW 但建不起来） |
| 解码 | — | — | `VTIsHardwareDecodeSupported H264=1 HEVC=1`，正常走 VCN0Dec |

像素格式（420v / 420f / 444v / BGRA）与分辨率对普通硬编均无影响，说明瓶颈不是格式，而是 **usage（低延迟用法）**。

## 4. 影响面与后续

- 影响：无线镜像、FaceTime/连续互通相机这类"低延迟实时编码"场景会落到软编，CPU 高但可用；画面正确性不受影响。
- 硬解、普通硬编、HDR/显示、显存分配均不受本次结论影响。
- 后续试探（分支 `nred/vce-probe`，默认关、`-NRedVCEOn` 打开）：看 `hasVCE=false` 是否是低延迟用法拿不到的原因。若打开后 `AVFQTXENC usage 20` 仍失败，则该路径不可行，改为长期接受软编。

## 5. 复现用探针

`/tmp/vtsess.c`（编译：`clang -O1 -o vtsess vtsess.c -framework VideoToolbox -framework CoreVideo -framework CoreMedia`）

- 参数：`<encoderID|auto> <宽> <高> <帧数>`
- 环境变量：`VT_PIXFMT=420v|420f|444v|BGRA`，`VT_LL=1` 打开低延迟规格

`/tmp/vtprobe.c`：列出编码器清单与 `IsHardwareAccelerated`，以及 `VTIsHardwareDecodeSupported`。
