# HEVC 8K 挂死事故与"按规格拒绝"方案（2026-11-21）

## 一、事故现象

在装上 **HEVC 试验版 kext**（`NRED_ALLOW_HEVC_HW=1`，HEVC 硬解门放开，md5 `8a05ddb5302907fb55f45398ff1d1f01`）之后：

1. 12:24 用 IINA 打开 `hevc_8k60P_bilibili_*.mp4`（7680×4320），系统随即卡死；
2. 硬重启后，进系统 1~2 分钟内再次卡死，`loginwindow` 每 11 秒重启一次；
3. 三次重启三次复现，ssh 也失联。

## 二、根因

日志（`log show`，kernel）：

```
channel 12 VCN0Dec is hung!  (lastReadTimestamp=0x00008139 / 0x0000000a)
GPURestartBegin stampIdx=12 type=2
AMD Cail: GPU HangState 0x00000000, HangFlags 0x00000004
```

- 有 7680×4320 的 HEVC 码流被提交给 **VCN 解码引擎**，VCN 2.2 做不到这个规格，通道挂住后
  `lastReadTimestamp` 不再前进，**永不自愈**；
- 每次轮询都伴随一次 `GPURestartBegin`，WindowServer / loginwindow 被反复打断 → 界面卡死；
- 挂死的提交留在内核里清不掉，**只能重启**，重启后同样的事件再次发生 → 开机死循环。

触发者不止一个（每次开机自动发生）：

| 触发者 | 说明 |
|---|---|
| IINA（mpv） | 日志 `Using hardware decoding (videotoolbox-copy)`，直接把 8K 交给硬解路径 |
| Chrome | 会话恢复会重开 8K 标签页；播放时另用软解（`VTDecoderXPCService` ~350% CPU） |
| 系统动态壁纸 | `NeptuneOneWallpaper` / `WallpaperAerialsExtension` 常驻，动态壁纸实体是 HEVC 视频循环解码，`AppleGVA [HEVC]` 日志可见 |

第一次开机之所以"正常"，是因为那 20 分钟里没有出现超规格的 HEVC 硬解请求；一旦有，就进入不可逆状态。

## 三、实测数据（同一台机，试验版 kext）

| 文件 | 容器 | 分辨率 | 强制硬解会话 |
|---|---|---|---|
| `hevc_4k60P_main10_10bit_420` | hvc1 | 3840×2160 Main10 | OK，120 帧仅 EOS 噪声帧 |
| `hevc_4k24P_main10_1` | hev1 | 3840×2160 | -12906（容器问题，见下） |
| `hevc_8k60P_bilibili_1/2/3` | hev1 | 7680×4320 | -12907（硬件不支持该格式） |
| `hevc_test.mp4` | hev1 | 1920×1080 | -12906 |

补充结论：

- **`hev1`（参数集在码流内）不被 VideoToolbox 接收**：连软件会话都建不起来（-12906）。
  这不是驱动问题，`QuickTime` / `预览` 同样打不开；Chrome 能播是因为它自己解析码流重建参数集。
  把 fourcc 改成 `hvc1` 后，同文件立刻可以硬解。
- **8K 即使改成 hvc1 也拿不到硬解**（-12907），`允许硬解` 时回落软解（8K HEVC 软解约 3 核）。
- 4K / 4K Main10 走 VCN 硬解正常，且与软解**逐帧像素完全一致**（`vdec_compare` 按可见宽高比对，最大差值 0）。

## 四、方案（驱动级，不做应用白名单）

目标：**超规格的解码会话在驱动侧被拒绝**，让 VideoToolbox 干净回退软解；≤4K 继续走硬件。

- 拒绝条件（阈值）：宽 > 4096 或 高 > 2304（等价于 level > 5.1 的实际能力上限）；
- 落点候选（已确认符号存在）：
  - `AMDRadeonX5000MTLDriver`：`__ZN9VAFactory13createDecoderEiP9VAContext`（现有 HEVC 门即在此，`cmp esi,6 / ja`）；
  - `AMDRadeonVADriver2`：`__ZN9VADecoder4InitEj` 内有同构判定
    （`mov esi,[rdi+0x78]` 取 codec、`cmp esi,6 / ja` 拒绝 >6），codec 字段在 `+0x78`。
- **当前实现阻塞点**：`createDecoder / Init` 这两个入口只收到 codec 与对象指针，**没有直接拿到宽高**；
  需要确认宽高在 `VADecoder`（或其 `+0x58` 上级对象）中的偏移，才能写"读宽高 → 比较 → 返回空解码器"。
  取偏移必须做一次内存采样（一次 1080p/4K 硬解会话 + lldb 读对象），不能靠猜。

## 五、回滚与当前状态

- 已回滚：ESP `/EFI/OC/Kexts/NootedRed.kext` → `2b275a0e1d72a78cb0b3ab698b62a9cd`（带 HEVC 门的稳定版）；
- 试验版留存：`NootedRed.kext.rollback-hevc-test`（`8a05ddb5…`），不删除，便于后续对照；
- 回滚后指标：`VCN0Dec is hung` 计数 0、load ~0.4、HEVC 回到软解。

## 六、测试纪律（本次教训）

1. **不要在这台机器上直接喂 8K 给硬解路径**；验证"拒绝"只需看会话创建是否失败 + VT 是否回落软解。
2. 长跑/并发解码测试限定在 1080p / 4K。
3. 装机试验版前，先确认目标机上一个可用的回滚点已存在于 ESP。
4. zsh 远端命令里不要出现裸 `==`（会被当成 `=cmd` 展开）。
