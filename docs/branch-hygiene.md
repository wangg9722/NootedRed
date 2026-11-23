# 分支收纳约定

目的：仓库里只保留「主线 + 一条开发线」，历史试验与场景验证分支统一收进 `attic/` 命名空间，
既不被误 checkout，也不丢历史。

## 保留的分支

| 分支 | 角色 | 说明 |
| --- | --- | --- |
| `master` | 主线 | 已打 tag 的稳定基线（`v0.9.1-20261123-login-guard` = `7503ebf`） |
| `feat/optional-switches` | 开发线 | 所有新功能点都在这里按「一功能点一提交 + 独立开关」叠加 |

## 收纳的分支（`attic/*`）

`attic/` 下的分支只读：不复用、不再开新提交。需要其中某段代码时，从 `attic/` 的提交
新建分支再收拾，不要把 `attic` 分支直接改回工作状态。

| 收纳分支 | 来源 | 内容 |
| --- | --- | --- |
| `attic/fix_login-path-guard` | `fix/login-path-guard` | 登录期解码守卫并发闸门（已合入 master） |
| `attic/feat_vram-allocator` | `feat/vram-allocator` | 显存分配碎片回退，已并入 `-NRedVRAMAlloc` |
| `attic/fix_vcn-capability-after-init` | `fix/vcn-capability-after-init` | 能力位延后（已否，仍卡死） |
| `attic/fix_hasvcn0-single-source` | `fix/hasvcn0-single-source` | hasVCN0 单源（已否） |
| `attic/fix_audio-trigger-align` | `fix/audio-trigger-align` | 音频触发对齐（已否） |
| `attic/feat_audio-default-on`、`attic/feat_audio-hdmi-dp`、`attic/feat_audio-hdmi-dp-v2`、`attic/backup_audio-6a983ad`、`attic/analysis_audio-v2`、`attic/tmp-audio`、`attic/tmp-audio2` | 音频各试验线 | HDMI/DP 音频写入序列试验 |
| `attic/analysis_display-audio` | `analysis/display-audio` | DCN 侧音频管线分析（远端仍存在） |
| `attic/analysis_vp9-reference` | `analysis/vp9-reference` | VP9 多版本表分析 |
| `attic/docs_risk-analysis` | `docs/risk-analysis` | 风险清单复核文档（`docs/risk-analysis-2026-11-23.md`） |
| `attic/debug_vcn-init-timing` | `debug/vcn-init-timing` | 初始化时序探针（仅排查用） |
| `attic/fix_legacy-flag` | `fix/legacy-flag` | 旧系统开关前缀（已合入 master） |
| `attic/fix_baseline-no-submitguard` | `fix/baseline-no-submitguard` | 基线不默认装守卫（远端仍存在） |
| `attic/fix_submitguard-scope` | `fix/submitguard-scope` | 旧系统默认不装显示修复（远端仍存在） |
| `attic/fix_hevc-overspec-guard` | `fix/hevc-overspec-guard` | 超规格 HEVC 守卫回退（远端仍存在） |
| `attic/revert_submitguard-e06bf70` | `revert/submitguard-e06bf70` | 一次 revert 记录（远端仍存在） |

远端同名分支保持原样，避免他人引用失效；本地只是改名，没有删除任何提交。

## 开关纪律（与分支同一套）

- 一个功能点一条提交，一个功能点一个开关；不同功能点之间不共用开关。
- 有风险或未验证的功能默认**关**（省略参数时等于旧行为）；实机验证通过后再改成默认开。
- 开关命名统一 `-NRed<功能>`，关闭用 `-NRedNo<功能>`，定位/策略变体用功能名后缀。

## 恢复某个 attic 分支

```bash
git switch -c feat/<新用途> attic/<收纳分支名>
```

确认只取其中部分提交时，用 `git cherry-pick <sha>` 而不是整分支合并。
