"""离线核对：把 UserSurfaceSyncData.hpp 里的补丁字节与目标系统的真实缓存逐一比对。

用于在装机重启之前判断 resolver 能否走到"打补丁"那一步，避免又一次盲试。
比对口径与 resolver 一致：整段字节 + 允许 rel32 位移位置不同。

用法：
    python3 tools/verify_surface_sync_bytes.py            # 当前系统的缓存
    python3 tools/verify_surface_sync_bytes.py --cache DIR
"""
import argparse
import re
import sys
from pathlib import Path

import metal_cache

DATA = Path(__file__).resolve().parent.parent / "NootedRed" / "UserSurfaceSyncData.hpp"
ARRAY_RE = re.compile(r"inline constexpr UInt8 (\w+)\[\]\s*=\s*\{(.*?)\}", re.S)
BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{2})")


def arrays():
    text = DATA.read_text()
    out = {}
    for name, body in ARRAY_RE.findall(text):
        out[name] = bytes(int(v, 16) for v in BYTE_RE.findall(body))
    return out


def constants():
    text = DATA.read_text()
    out = {}
    for name, value in re.findall(
            r"inline constexpr (?:UInt64|UInt32) (\w+) = (0x[0-9A-Fa-f]+(?:ULL|UL|U)?|[0-9]+);", text):
        out[name] = int(value.rstrip("UL"), 0)
    return out


def displacement_positions(expected):
    """标出可能是 rel32 位移的字节位置（与 resolver 的豁免口径一致）。"""
    positions = set()
    for pos in range(len(expected)):
        back = max(0, pos - 3)
        for start in range(back, pos + 1):
            b = expected[start]
            if b in (0xE8, 0xE9) and 1 <= pos - start <= 4:
                positions.add(pos)
            if start + 2 < len(expected) and b in (0x48, 0x4C):
                op, modrm = expected[start + 1], expected[start + 2]
                if op in (0x8D, 0x8B, 0x89, 0x39, 0x3B) and (modrm & 0xC7) == 0x05 \
                        and pos > start + 2 and pos - (start + 2) <= 4:
                    positions.add(pos)
    return positions


def compare(reader, image_base, rva, expected, label):
    actual = reader.read(image_base + rva, len(expected))
    diffs = [i for i in range(len(expected)) if actual[i] != expected[i]]
    exempt = displacement_positions(expected)
    strict = [i for i in diffs if i not in exempt]
    verdict = "一致" if not diffs else (
        f"{len(diffs)} 处差异（其中 {len(strict)} 处不在 rel32 豁免范围）" if strict else
        f"{len(diffs)} 处差异，全部落在 rel32 豁免范围")
    print(f"  {label:<22} rva {rva:#08x} 长度 {len(expected):<4} 头32字节{'一致' if actual[:32] == expected[:32] else '不同'} → {verdict}")
    if strict:
        print(f"    非豁免差异索引（前 20）：{strict[:20]}")
    return len(strict) == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", default=str(metal_cache.CACHE))
    parser.add_argument("--image", default=metal_cache.IMAGE)
    args = parser.parse_args()
    metal_cache.CACHE = Path(args.cache)
    metal_cache.IMAGE = args.image

    data, const = arrays(), constants()
    reader = metal_cache.CacheReader()
    try:
        print(f"缓存 {reader.cache_id}  image base {reader.image_base:#x}")
        ok = True
        ok &= compare(reader, reader.image_base, const["InitRVA"], data["Original0"], "Init(Original0)")
        ok &= compare(reader, reader.image_base, const["OverrideRVA"], data["Original1"], "Override(Original1)")
        ok &= compare(reader, reader.image_base, const["ConstructorRVA"], data["BlendConstructor"], "BlendConstructor")
        ok &= compare(reader, reader.image_base, const["NativeOverrideRVA"], data["NativeOverride"], "NativeOverride")
        ok &= compare(reader, reader.image_base, const["ScratchRVA"], data["ScratchOriginal"], "Scratch(仅 -NRedScratch)")
    finally:
        reader.close()
    print("\n结论：" + ("全部补丁点在目标缓存上可核验" if ok else "存在无法核验的补丁点，resolver 会在对应步骤拒绝"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
