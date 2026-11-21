# 工具目录

按用途分四类。所有生成器都要求在**目标系统的缓存**上运行（`metal_cache.CACHE`
指向 Cryptex dyld 目录），生成结果直接写入 `NootedRed/*.hpp`，不手工编辑。

## 生成器（产出 kext 数据）

| 工具 | 产出 |
| --- | --- |
| `vdec_mode.swift` | 一条命令判断某文件走硬解还是软解（VT 会话属性，最权威） |

| `build_vcn_kernel_patch.py` | VCN 内核侧补丁 |
| `build_video_decode_patch.py` | `UserVideoDecodeData.hpp`（含 HEVC 硬解门） |
| `build_surface_sync_patch.py` | `UserSurfaceSyncData.hpp`（LegacyBlend / scratch） |
| `build_compute_scratch_patch.py` | compute scratch 数据 |
| `build_submit_guard.py` | SubmitGuard 数据 |
| `import_legacy_compat.py` | 14/15 legacy 数据导入 |
| `legacy_cp_factory.py` | legacy CP 工厂数据 |

## 装机前核验（重启之前必跑）

| 工具 | 用途 |
| --- | --- |
| `verify_surface_sync_bytes.py` | 渲染侧补丁点与目标缓存逐段比对（rel32 豁免） |
| `verify_video.py` | 视频侧全部补丁 original 字节与目标缓存逐条比对 |
| `blend_gate_probe.py` | 双机判定 LegacyBlend 门形态（14=`75 0b`，26=`74 28`+allow） |
| `audit_video_driver.py` | 只读核对 VCN 接入所需驱动 / 虚表 / 识别代码 |

## 缓存读取基础设施

| 工具 | 用途 |
| --- | --- |
| `metal_cache.py` | dyld 共享缓存 reader（可 monkey-patch `CACHE`/`IMAGE` 切换目标） |
| `cache_resolver_data.py` | resolver 锚点数据构建 |
| `vcn_capabilities.py` | 设备能力表（与内核侧共用判定） |

## 回归测试（`test_*.py`）

- `test_legacy_blend_multi_os.py`、`test_multi_os_compat.py`：多系统门形态回归（当前 PASS）
- `test_vcn_*`：VCN 桥接 / ABI / 引擎 / GVA 选择回归
- `test_surface_sync_*`、`test_compute_*`、`test_cache_resolver_reference.py`：resolver 与补丁回归
- `test_ec_backlight_*`：EC 背光回归
- 已知既有失败（非回归，待修）：`test_surface_sync_dynamic.py`、`test_legacy_blend_gate.py`、`test_surface_sync_patch.py`

## 汇编载荷

`*.s`：`submit_guard.s`、`surface_sync_init.s`、`surface_sync_srd_shared_init.s`、
`video_addr_convert.s`，由生成器经 `compile_payload` 内联。

`fixtures/`：模板与参考数据（`legacy-compat.json`、entitlement、EC 背光模板）。
