#!/usr/bin/env python3
"""解码守卫门控的离线单元测试（不需要重启机器）。

被测对象 `NootedRed/UserVideoDecode.cpp` 两段：
  1. `init()` 里的赋值逻辑（开关解析 + 低版本把 NoDecodeGuard 一并打开）；
  2. `wrappedSharedRegionCheck()` 里的门控表达式（本次**未改动**，仍是
     `decodeGuard && !(legacyProfile && legacyNoGuard)`）。

关键背景：Lilu `checkKernelArgument()` -> `PE_parse_boot_arg()` 是**前缀匹配**，
所以 `-NRedNoDecodeGuardLegacy` 会命中查询 `-NRedNoDecodeGuard`。legacy 作用域的开关
因此一律前缀前置（`-NRedLegacy*`），避免互相污染。

用法：python3 tests/decode_guard_gate_test.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def has(boot_args: str, name: str) -> bool:
    """模拟 PE_parse_boot_arg：token 前缀匹配。"""
    return any(token.startswith(name) for token in boot_args.split())


def gate(boot_args: str, major: int, has_legacy_profile: bool) -> bool:
    # --- init() 赋值逻辑 ---
    legacy_no_guard = True                                  # NRedLegacyNoDecodeGuard = true（不做取反）
    decode_guard = not has(boot_args, "-NRedNoDecodeGuard")
    if legacy_no_guard and major <= 24:
        decode_guard = False                                # 低版本等价于 NoDecodeGuard 打开
    # --- 门控表达式（保持原样，未改动）---
    legacy = has_legacy_profile
    return decode_guard and not (legacy and legacy_no_guard)


THREE = "-NRedDisplayFix -NRedVideoFix -NRedGLMetal"
CONFIGS = {
    "三个正常参数": THREE,
    "三个参数 + -NRedLegacyNoDecodeGuard": THREE + " -NRedLegacyNoDecodeGuard",
    "三个参数 + -NRedNoDecodeGuard": THREE + " -NRedNoDecodeGuard",
    "裸 boot-args": "-v debug=0x144",
}
PLATFORMS = [("14.8.9", 23, True), ("15.x", 24, True), ("26.x", 25, False), ("14 无 profile", 23, False)]

failures: list[str] = []


def check(desc: str, cond: bool) -> None:
    print(f"[{'PASS' if cond else 'FAIL'}] {desc}")
    if not cond:
        failures.append(desc)


def source_flags() -> list[str]:
    flags: set[str] = set()
    for path in (ROOT / "NootedRed").rglob("*"):
        if path.suffix in (".cpp", ".hpp"):
            text = path.read_text(encoding="utf-8", errors="ignore")
            flags.update(re.findall(r'"(-NRed[A-Za-z0-9]+)"', text))
    return sorted(flags)


def main() -> int:
    print("== 门控真值表（是否安装解码守卫） ==")
    print(f"{'配置':<40}" + "".join(f"{n:<14}" for n, _, _ in PLATFORMS))
    table = {}
    for cfg, ba in CONFIGS.items():
        row = []
        for name, major, prof in PLATFORMS:
            v = gate(ba, major, prof)
            table[(cfg, name)] = v
            row.append(v)
        print(f"{cfg:<40}" + "".join(f"{str(v):<14}" for v in row))

    print("\n== 不变量 ==")
    for name, major, prof in PLATFORMS:
        if major <= 24 and prof:
            check(f"{name} 默认不装守卫（无需任何参数）", not table[("三个正常参数", name)])
            check(f"{name} 低版本任何参数组合都不会装守卫",
                  all(not table[(cfg, name)] for cfg in CONFIGS if cfg != "三个参数 + -NRedNoDecodeGuard"))
            check(f"{name} -NRedLegacyNoDecodeGuard 不改变默认行为（仍不装）",
                  not table[("三个参数 + -NRedLegacyNoDecodeGuard", name)])
        elif major > 24:
            check(f"{name} 默认装守卫（26 行为不变）", table[("三个正常参数", name)])
            check(f"{name} -NRedNoDecodeGuard 显式关闭",
                  not table[("三个参数 + -NRedNoDecodeGuard", name)])

    print("\n== 开关命名：legacy 前缀前置、互不为前缀 ==")
    flags = source_flags()
    print(f"共 {len(flags)} 个：{' '.join(flags)}")
    ALLOWED_OVERLAP = {("-NRedDebug", "-NRedDebugUltra")}
    active = flags
    bad = [(a, b) for a in active for b in active
           if a != b and b.startswith(a) and (a, b) not in ALLOWED_OVERLAP]
    check("生效开关之间没有前缀关系", not bad)
    for a, b in bad:
        print(f"  冲突：{a} 是 {b} 的前缀")
    legacy = [f for f in active if "Legacy" in f]
    check("legacy 开关全部前缀前置", all(f.startswith("-NRedLegacy") for f in legacy))
    check("不再存在反向开关 -NRedLegacyDecodeGuard", "-NRedLegacyDecodeGuard" not in flags)
    print(f"  legacy 开关：{' '.join(sorted(legacy))}")
    check("旧的 -NRedNoDecodeGuardLegacy 已彻底移除", "-NRedNoDecodeGuardLegacy" not in flags)

    print(f"\n结果：{'全部通过' if not failures else str(len(failures)) + ' 项失败'}")
    for f in failures:
        print(f"  - {f}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
