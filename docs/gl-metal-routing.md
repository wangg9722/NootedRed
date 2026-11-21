# 全局 GL 渲染器路由：legacy GLDriver → GL-on-Metal

## 背景

249（iMac Pro 机箱 / Cezanne `1002:1638` / macOS 26.7.3）上，Electron 系旧版应用（优酷）开启 GPU 后约 40 秒把
**channel 0 GFX 引擎挂死**（`IndividualEngineHang=1, FenceNotRetired=1, PerEngineReset=1`），
GPU 进程随后在 `glrATI_Hwl_SubmitPacketsWithToken → gpusSubmitDataBuffers → gpusKillClientExt` 处被杀。

已排除的因素（均有实验记录）：

- 与视频解码无关：`--disable-accelerated-video-decode` 下依旧挂（A/B 已做）
- 与显存容量无关：VRAM 4 GB，挂死时 `vramFreeBytes` 仍在 2.6 GB 以上
- 单进程 GL 负载复现不了：基础绘制 / 双源混合 / IOSurface 采样 / IOSurface 作为 FBO 附件 /
  1080p 全帧与子矩形持续上传 / 真窗口 `flushBuffer` 呈现，各跑 60 s 均零挂死

结论：问题出在 **`AMDRadeonX5000GLDriver`（legacy OpenGL 驱动）** 这条路径本身，而不是某个具体 GL 调用。

## 修复思路

同一台机器上 Chrome（走 Metal / ANGLE）稳定，说明 Metal 路径是健康的。
系统自带 GL-on-Metal 层 `AppleMetalOpenGLRenderer.bundle`（有 x86_64 切片与通用 `default.metallib`），
它是**全局**的：一旦生效，所有 legacy OpenGL 客户端一起换到 Metal 后端，无需按应用名单逐条处理。

渲染器由 `IOAccelerator` 服务上的 `IOGLBundleName` 属性决定，默认值 `AMDRadeonX5000GLDriver`。

## 实现

boot-arg `-NRedGLMetal` 门控（默认关闭，随时可回退）：

- `NRed::publishGLBundleName()` 在 GPU 早期（`processPatcher`）把 `IOGLBundleName` 写到 iGPU provider；
- 在 `X6000FB::wrapControllerPowerUp()` 里再补一次，此时 `IOAccelerator` 服务已发布，遍历子树覆盖其属性；
- 覆盖结果可查：`ioreg` 中 `IOGLBundleName` 与 `NRedGLMetalApplied`（写入到的加速器数量）。

## 实测结果（macOS 26.7.3 / 25G342）

| 项 | 结果 |
|---|---|
| `IOGLBundleName` | `AppleMetalOpenGLRenderer`，`NRedGLMetalApplied=2` |
| GL 客户端看到的版本串 | `GL_VERSION = 2.1 Metal - 90.5`（确认走 Metal 后端） |
| 真实优酷（`/Applications/优酷.app/Contents/MacOS/优酷`） | 连续 150 s 以上，GFX/VCN 挂死 **0 次**，无新崩溃报告 |
| 视频硬解 | `hwAfterStart=1`（H.264 仍硬解），HEVC 仍按 `-NRedVideoFix` 走软解 |

## 回退

- 去掉 boot-arg `-NRedGLMetal` 重启即可回到 legacy GLDriver；
- 或恢复 ESP 上 `NootedRed.kext.bak-glm-*`。

## 注意

- WindowServer 走 Metal，不受此开关影响，桌面不会因 GL 路由失败而黑屏；
- 若某应用依赖 legacy GL 的扩展/精度，可能出现兼容性差异，届时仅需回退开关。
