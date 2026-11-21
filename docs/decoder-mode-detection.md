# 如何判断视频是硬解还是软解

视频“能播”不等于“走硬件解码”。Cezanne 上两者的差别是 CPU/功耗/发热，以及高码率 4K、
多路同播时能不能撑住。下面按可靠度排序，并记录实测过的坑。

## 1. 最准：`tools/vdec_mode.swift`

在真实解码若干帧后查询 VideoToolbox 会话属性
`kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder`：

- `1` → 硬解（命令提交到 VCN 引擎）
- `0` / `unknown` → 软解（Apple 软件解码器）

用法：

```bash
xcrun swiftc -O -o vdec_mode tools/vdec_mode.swift
./vdec_mode /path/to/video.mov        # 默认解 30 帧
./vdec_mode /path/to/video.mov 300    # 解 300 帧
```

249 上已安装到 `/usr/local/bin/vdec_mode`。实测（26.7.3 / 25G342）：

```
avc1 (H.264) 1920x1080 → 硬件解码 1(硬解)
hvc1 (HEVC)  1920x1080 → unknown（软解，符合 -NRedVideoFix 的 HEVC 门）
```

注意：该属性只描述**本次会话**。想判断别的应用在用什么，看下一条。

## 2. 运行中应用：看 `VTDecoderXPCService` 的 CPU

所有解码都在 `.../VideoToolbox.framework/XPCServices/VTDecoderXPCService.xpc` 里跑：

- 硬解：CPU 很低（1080p 约 3–15%，实测 H.264 硬解 13.8%）
- 软解：CPU 明显高（1080p HEVC 常 25% 以上，4K 更高）

活动监视器筛选该进程即可。不如第 1 条精确，但能覆盖“不知道正在播的是哪个文件”的场景。

## 3. 不可用 / 会误判的方法（已实测）

| 方法 | 问题 |
| --- | --- |
| `vmmap <pid> \| grep VADriver2` | 进程内可能残留旧的解码服务映射，软解时也能看到 VADriver2，误判为硬解 |
| `powermetrics --samplers gpu_power` | 本机不输出 Video Engine 计数 |
| `ioreg` 的 `HWChannel VCN0Dec \| Commands Submitted` | 该组统计在 NootedRed 下恒为 0，无法用于判断 |

## 4. 组合判断（推荐流程）

1. 用 `vdec_mode` 测同一段文件，确认系统层面该编码是否被门拦（HEVC 现在应显示软解）；
2. 应用播放时看 `VTDecoderXPCService` CPU：明显高即可判定软解；
3. 需要长时间观察时（例如 HEVC 硬解试验），同时监控 VT 返回码与
   `log show ... "VCN0Dec.*timeout|channel 12.*is hung|HangState"`。
