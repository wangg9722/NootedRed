# HEVC 硬件解码试验（待重启验证）

## 现状结论（26.7.3 / 25G342，Cezanne `1638`）

- H.264 硬解正常（单路、三路并发均 `hwAfterStart=1`，零错误帧）
- HEVC 目前**按设计走软解**：`tools/build_video_decode_patch.py` 生成的 FairPlay 解码数据里，
  `VAFactory::createDecoder` 头部由 `cmp esi,6 / ja` 改为 `cmp esi,5 / jae`，
  令 HEVC(5) 与 AV1(6) 走“返回空解码器”出口，VideoToolbox 自动回退软件 HEVC。
- 关门的既有理由（记录在生成器注释）：VCN2 桥接的 HEVC 硬解曾稳定出现帧级
  `kVTVideoDecoderMalfunctionErr`，长流会把 VCN0Dec 引擎挂死并触发 GPU Reset + WindowServer 看门狗。

## 这次要验证什么

1. GL 已改走 Metal（`-NRedGLMetal`）后，HEVC 硬解错误是否**仍然**出现 —— 排除“之前只是被 GFX 风暴带崩”的可能；
2. 若出现，是否仍会累积到 VCN0Dec 挂死（决定 HEVC 硬解能否开放）。

## 试验开关

生成器新增仅测试用环境变量：

```bash
NRED_ALLOW_HEVC_HW=1 python3 tools/build_video_decode_patch.py
```

置 1 时不写 HEVC 门补丁（保留原始 `cmp esi,6 / ja`）。默认不设该变量，产物与现在一致。

## 测试版 kext（已构建，未装机）

- 249 上：`~/nred-fix/build/Release-test-hevc/NootedRed.kext`
- md5：`8a05ddb5302907fb55f45398ff1d1f01`
- 当前装机版（带门 + GL-on-Metal）：`2b275a0e1d72a78cb0b3ab698b62a9cd`

## 验证步骤（需要重启一次）

装机测试版 → 重启 → 依次执行：

1. `swift /tmp/vtdec2.swift /tmp/hevc.mov` 检查 `hwAfterStart` 是否为 1（== 真硬解成功）
2. 连续解码 ≥3 分钟 / 多路并发，同时监控：
   - `kVTVideoDecoderMalfunctionErr`（VT 返回码 -12909 左右）
   - `log show ... "VCN0Dec.*timeout|channel 12.*is hung|HangState"`
3. 观察是否出现 GPU Reset / WindowServer 卡顿

判据：

- 全程无 malfunction 且无 VCN 挂死 → 可以正式开放 HEVC 硬解（去掉门）
- 出现 malfunction 但不挂引擎 → 保留门，HEVC 继续软解（现状即最优）
- 出现 VCN0Dec 挂死 → 保留门，并记录挂死时的 TS/PID 用于后续定性

## 回滚

恢复 ESP 上 `NootedRed.kext.bak-glm-*`（= 当前带门 + GL-on-Metal 版本），重启即可。
