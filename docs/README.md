# 文档索引

按主题分类；新文档请在对应分类下登记一行，保持「一个主题一篇文档」。

## 版本与兼容（先读这类）

| 文档 | 内容 |
| --- | --- |
| [version-selection-matrix.md](version-selection-matrix.md) | **入口文档**：14/15/26 三条通道如何区分版本、26 门形态双机实测、resolver 的策略耦合原则、装机前离线自检 |
| [macos-multi-os-compat.md](macos-multi-os-compat.md) | 多系统（14/15/26）适配范围与限制 |
| [boot-arguments.md](boot-arguments.md) | 全部启动参数：`-NRedVideoFix` / `-NRedDisplayFix` / `-NRedScratch` 等 |

## 视频解码（VCN）

| 文档 | 内容 |
| --- | --- |
| [vcn.md](vcn.md) | VCN 接入全景：设备能力表、内核 ABI 核验、用户态补丁、诊断字段含义、HEVC 硬解门（2026-11-26.7.3 实测） |
| [fairplay-psp-compatibility.md](fairplay-psp-compatibility.md) | FairPlay / PSP 内容保护兼容 |

## 渲染（Metal / SurfaceSync）

| 文档 | 内容 |
| --- | --- |
| [metal-rendering-fix.md](metal-rendering-fix.md) | Metal 渲染修复：LegacyBlend gate+allow、SubmitGuard |
| [user-cache-dynamic.md](user-cache-dynamic.md) | 26 用户态动态缓存匹配（resolver）设计 |

## 背光 / EC

| 文档 | 内容 |
| --- | --- |
| [ec-backlight-rewrite.md](ec-backlight-rewrite.md) | EC 背光重写 |
| [ec-backlight-handoff.md](ec-backlight-handoff.md) | 背光交接 |
| [ec-backlight-comparison.md](ec-backlight-comparison.md) | 方案对比 |

## 实测记录

| 文档 | 内容 |
| --- | --- |
| [vega-tahoe-r3-test.md](vega-tahoe-r3-test.md) | Vega / Tahoe r3 测试记录 |
