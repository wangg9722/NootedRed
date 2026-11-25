# VP9 硬件解码（-NRedVP9Decode）

VP9 后端来自参考项目 `steve12311/NootedRed`（`098c9d8` / `39d0b91` / `d97e59d` / `b7d…` 系列），
在 dev-3 上按三段提交接入：数据集 → 能力发布 → 用户态安装。总开关 `-NRedVP9Decode`，**默认关**，
不打开时完全不触碰加速器属性、也不装任何站点，行为等同 dev-2。

## 组成

| 文件 | 作用 |
| --- | --- |
| `UserVP9DecodeData.hpp` / `…SequoiaData.hpp` / `…SonomaData.hpp` | Darwin 25/24/23 三套站点数据：整函数原始字节、rel32 引用、段表目标、ABI 槽位 |
| `UserVP9DecodeResolver.hpp` | 按版本选数据集，全量核验后产出 `Plan` |
| `UserSectionCache.hpp` / `UserSectionResolver.hpp` / `UserCacheSectionData.hpp` | 段感知的缓存读取器与解析器，**独立命名空间**，与已验证的 `UserSharedCache` / `UserCacheResolver` 互不影响 |
| `VP9Probe.hpp` | 能力键发布：`IOGVAXDecode=2`、`IOGVAVP9Decode=1`、`IOGVAVP9DecodeCapabilities{VTSupportedProfileArray=(0,2)}` |
| `VP9Message.hpp` / `VP9DefaultProbs.hpp` | 数据集引用的常量表 |

段感知解析器是复制而非替换，原因是参考项目的 `UserCacheResolver` 比本树现用版本多了
`ScanSize`/`selectImages`/段重定位，直接替换会把已验证的 H.264/HEVC 路径一起换掉，无法在不重启验证的前提下确认无回归。

## 安装顺序

1. `VCN::init` 读开关 → `vp9Requested`。
2. 视频补丁在本进程落定（或已是目标态）后，`VP9::install` 进入：
   - 按缓存 UUID + 基址做一次性解析（`Plan` 复用，失败允许下个进程重试）；
   - 全量读回每个站点，必须是「全原始」或「全已打」；
   - 页面保护必须是 RX，随后逐页 COW 成 RW 写入，写完恢复 RX；
   - 任何一步失败：回填已写站点、恢复保护、标记 `State::Failed`、撤回能力键。
3. 成功 → `VCN::setVP9Ready(true)` → 加速器上发布能力键 → `NRedVTDecoderAdmissionRevision=1`。
4. 失败 → `VCN::setVP9Ready(false)` → `IOGVA*` 键全部移除，客户端回退原解码选择。

## 判读属性

`NRedVP9Requested`、`NRedVP9State`（1 就绪待装 / 2 已解析 / 3 缓存未通过 / 4 提交失败 / 6 资源不足）、
`NRedVP9Applied`、`NRedVP9Already`、`NRedVP9Mixed`、`NRedVP9Region`、`NRedVP9Protection`、
`NRedVP9WriteFailed`、`NRedVP9Rollback`、`NRedVP9Probe`（VP9Probe 状态）、`NRedVTDecoderAdmissionRevision`。

## 验证步骤（待做）

样本：`Big_Buck_Bunny_1080_10s_1MB.webm`（VP9 1920×1080）、`vp9_4k_globe.webm`（VP9 2160×2160），
sha256 分别 `752ee359…`、`6077cb19…`。

1. boot-args 加 `-NRedVideoFix -NRedVP9Decode`，重启。
2. `ioreg -lw0 | grep -E "IOGVAXDecode|IOGVAVP9Decode|NRedVP9"` 应看到能力键与 `NRedVP9Applied ≥ 1`。
3. `vtdecoder` 认领：Chrome 播放 VP9 视频时 `NRedVCNDecoderPID` 出现新 PID，CPU 占用明显低于软解。
4. 无 `VCN0Dec` gpuRestart；`-NRedVP9Decode` 去掉后能力键应全部消失。

## 已知缺口

- Profile 2（10-bit）只有代码与能力声明支持，**没有可用的 10-bit 样本做实机验证**。
- AV1 与 VP9 的准入统一是数据层（`AvdCapability::checkCapabilityList` 整函数补丁），本树尚无 AV1 实机验证。
- 三段改动只做过本地构建，未在 14/15/26 上部署过。
