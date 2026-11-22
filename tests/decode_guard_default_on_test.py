#!/usr/bin/env python3
"""解码守卫默认开启的离线回归测试（不需要重启机器）。

被测对象 `NootedRed/UserVideoDecode.cpp`：
  1. `init()` 里的赋值逻辑：14/15 不再把 `decodeGuard` 关掉（`legacyNoGuard = false`）；
  2. `wrappedSharedRegionCheck()` 里的门控表达式 `decodeGuard && !(legacyProfile && legacyNoGuard)`。

背景（2026-11-22/23 实测）：
  14.8.9(23J631) 上把守卫关掉后，解码一走 VCN 就 FenceNotRetired，
  gpuRestart 报 `VTDecoderXPCServ / VCN0Dec`，随后 WindowServer 卡死。
  守卫开回来后会按 `VADecoder::Init` 的 24 字节特征选偏移（14 = 第 2 档候选），实测正常。

用法：python3 tests/decode_guard_default_on_test.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "NootedRed" / "UserVideoDecode.cpp"

failures: list[str] = []


def check(desc: str, cond: bool) -> None:
    print(f"[{'PASS' if cond else 'FAIL'}] {desc}")
    if not cond:
        failures.append(desc)


def has(boot_args: str, name: str) -> bool:
    """模拟 Lilu 的 PE_parse_boot_arg：前缀匹配，不是精确匹配。"""
    return any(token.startswith(name) for token in boot_args.split())


def gate(boot_args: str, has_legacy_profile: bool) -> bool:
    """复刻 init() 赋值 + 门控表达式。"""
    legacy_no_guard = False                                     # 与源码一致：守卫不被关
    decode_guard = not has(boot_args, "-NRedNoDecodeGuard")
    return decode_guard and not (has_legacy_profile and legacy_no_guard)


BOOT_ARGS = "-v debug=0x144 -NRedDisplayFix -NRedVideoFix -NRedGLMetal"


def main() -> int:
    src = SRC.read_text(encoding="utf-8")

    # --- 静态检查：源码里不允许再把守卫关掉 ---
    check("源码存在 legacyNoGuard = false", bool(re.search(r"legacyNoGuard\s*=\s*false;", src)))
    check("源码不再把 decodeGuard 置 false", not re.search(r"decodeGuard\s*=\s*false;", src))
    check(
        "源码不再按内核版本关守卫",
        not re.search(r"major\(\)\s*<=\s*MACOS_15_X", src),
    )
    check(
        "decodeGuard 仍由 -NRedNoDecodeGuard 关闭",
        bool(re.search(r'decodeGuard\s*=\s*!checkKernelArgument\("-NRedNoDecodeGuard"\)', src)),
    )
    check(
        "门控表达式保持 decodeGuard && !(legacyProfile && legacyNoGuard)",
        bool(
            re.search(
                r"if\s*\(\s*decodeGuard\s*&&\s*!\(\s*legacyProfile\s*&&\s*legacyNoGuard\s*\)\s*\)",
                src,
            )
        ),
    )

    # --- 行为检查：14/15/26 都装守卫，除非显式 -NRedNoDecodeGuard ---
    check("14 有 legacy profile → 装守卫", gate(BOOT_ARGS, True) is True)
    check("14 无 legacy profile → 装守卫", gate(BOOT_ARGS, False) is True)
    check("显式 -NRedNoDecodeGuard → 不装", gate(BOOT_ARGS + " -NRedNoDecodeGuard", True) is False)
    check("裸 boot-args → 装守卫", gate("-v debug=0x144", True) is True)

    # --- 前前缀污染回归：legacy 开关不得命中 26 侧开关 ---
    check(
        "-NRedLegacyNoVCNUser 不会命中 -NRedNoVCNUser",
        has(BOOT_ARGS + " -NRedLegacyNoVCNUser", "-NRedNoVCNUser") is False,
    )
    check(
        "-NRedLegacyNoDRMVideo 不会命中 -NRedNoDRMVideo",
        has(BOOT_ARGS + " -NRedLegacyNoDRMVideo", "-NRedNoDRMVideo") is False,
    )

    total = len(failures) + 0
    print()
    if failures:
        print(f"FAILED: {len(failures)} 项")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
