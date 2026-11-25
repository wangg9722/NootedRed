#!/usr/bin/env python3
"""dev-v2 开关矩阵静态检查：每个功能都有独立开关，且默认关（数据集/音频除外）。

不依赖硬件，只做源码级断言，防止重放时把默认值改错：
- 显存两项、VCN 能力位延后、submit-guard 两项、引擎就绪门、并发闸门、VP9：默认关；
- 14/15 走整函数数据集时不装运行期守卫（双重改写会互坏）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEO = ROOT / "NootedRed" / "UserVideoDecode.cpp"
SURFACE = ROOT / "NootedRed" / "UserSurfaceSync.cpp"
X5000 = ROOT / "NootedRed" / "X5000.cpp"
VCN = ROOT / "NootedRed" / "VCN.cpp"

failures = []


def check(name, condition):
    if condition:
        print(f"  ok   {name}")
    else:
        failures.append(name)
        print(f"  FAIL {name}")


def text(path):
    return path.read_text(encoding="utf-8")


def main():
    video = text(VIDEO)
    surface = text(SURFACE)
    x5000 = text(X5000)
    vcn = text(VCN)

    print("[1] 开关存在且默认关")
    check("engineGate 默认 false", bool(re.search(r"bool\s+engineGate\{false\}", video)))
    check("hookBusyGate 默认 false", bool(re.search(r"bool\s+hookBusyGate\{false\}", video)))
    check("engineGate 由 -NRedVCNEngineGate 打开", '-NRedVCNEngineGate' in video)
    check("hookBusyGate 由 -NRedHookBusyGate 打开", '-NRedHookBusyGate' in video)
    check("显存开关 -NRedVRAMAlloc", '-NRedVRAMAlloc' in x5000)
    check("下溢开关 -NRedAllocPagesUnderflow", '-NRedAllocPagesUnderflow' in x5000)
    check("能力位延后 -NRedVCNAfterInit", '-NRedVCNAfterInit' in x5000)
    check("submit-guard 独立开关", '-NRedSubmitGuard' in surface)
    check("submit-guard 映像解析开关", '-NRedSubmitGuardSig' in surface)
    check("VP9 总开关 -NRedVP9Decode", "-NRedVP9Decode" in video)
    check("VP9 局部开关默认关", bool(re.search(r"bool\s+enabled\{false\};", video)))
    check("VP9 能力发布按开关门控", "vp9Requested" in vcn)
    probe = text(ROOT / "NootedRed" / "VP9Probe.hpp")
    check(
        "VP9 能力键齐全",
        all(k in probe for k in ("IOGVAXDecode", "IOGVAVP9Decode", "IOGVAVP9DecodeCapabilities")),
    )

    print("[2] 放行判据按开关切换")
    check(
        "VP9 只在视频补丁落地后安装",
        bool(re.search(r"if\s*\(VP9::enabled\)\s*\{ VP9::install\(map", video)),
    )
    check("VP9 失败撤回能力声明", "VCN::setVP9Ready(false)" in video)
    check(
        "engineGate 打开时才用 engineStarted()",
        bool(re.search(r"engineReady\s*=\s*engineGate\s*\?\s*VCN::engineStarted\(\)\s*:\s*VCN::allocated\(\)", video)),
    )
    check("engineStarted 只由 initHardware 成功置位", 'engineStartedFlag' in vcn and "setProp32(\"NRedVCNEngineStarted\", 1)" in vcn)

    print("[3] 数据集路径不装运行期守卫")
    check(
        "守卫条件排除 sonoma/sequoia",
        bool(re.search(r"if\s*\(decodeGuard\s*&&\s*!sonomaPatches\s*&&\s*!sequoiaPatches\)", video)),
    )
    check(
        "legacy blind 闸门不拦数据集路径",
        bool(re.search(r"legacyProfile\s*&&\s*!legacyVCNUser\s*&&\s*!sonomaPatches\s*&&\s*!sequoiaPatches", video)),
    )

    if failures:
        print(f"\n{len(failures)} 项失败: {', '.join(failures)}")
        return 1
    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
