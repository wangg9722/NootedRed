"""生成解码规格守卫数据：核验 VADecoder::Init 入口字节，汇编守卫并嵌入头文件。"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/decoder-guard"
OUT = ROOT / "NootedRed/UserDecoderGuardData.hpp"
sys.path.insert(0, str(ROOT / "tools"))

from metal_cache import macho  # noqa: E402

CACHE_BASE = 0x7FF800000000
# 各版本 dyld 缓存里 AMDRadeonVADriver2 的 VADecoder::Init 入口偏移（相对缓存 base）。
# 运行期不去比版本号，而是逐个候选读入口字节，命中 EntryOriginal 的那个才用。
#   26.7.3 / 25G342 : 镜像 0x7FFB10B00000，Init 镜像内 0x7CC6
#   14.8.9 / 23J631 : 镜像 0x7FFB0339C000，Init 镜像内 0x3FA4
CANDIDATES = [
    ("macOS 26.7.3", 0x7FFB10B00000 - CACHE_BASE + 0x7CC6),
    ("macOS 14.8.9", 0x7FFB0339C000 - CACHE_BASE + 0x3FA4),
]
# 入口前 24 字节（前 6 条指令，止于 0x18 的指令边界）。
ENTRY_ORIGINAL = bytes.fromhex(
    "554889e5415653" "89b728010000" "48c787f0000000 00000000"
)
PATCH_SIZE = len(ENTRY_ORIGINAL)
PLACEHOLDER = bytes.fromhex("49bb8877665544332211")  # movabs $0x1122334455667788,%r11


def array(name: str, data: bytes) -> str:
    lines = [f"inline constexpr UInt8 {name}[] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> None:
    if PATCH_SIZE < 14 or PATCH_SIZE > 64:
        raise SystemExit(f"补丁长度不支持：{PATCH_SIZE}")

    WORK.mkdir(parents=True, exist_ok=True)
    obj = WORK / "vad_decode_guard.o"
    subprocess.run(["as", "-arch", "x86_64", str(ROOT / "tools/vad_decode_guard.s"), "-o", str(obj)],
                   check=True, timeout=60)
    data = obj.read_bytes()
    _, sections, _ = macho(lambda a, n: data[a:a + n], 0)
    text = next(s for s in sections if s[0] == "__text")
    payload = data[text[3]:text[3] + text[2]]
    if len(payload) >= 4096:
        raise SystemExit("守卫超过一页")
    if payload.count(PLACEHOLDER) != 1:
        raise SystemExit("守卫占位符数量不为 1")
    jump_offset = payload.index(PLACEHOLDER) + 2

    header = "// 由 tools/build_decoder_guard.py 生成，不手工编辑。\n#pragma once\n#include <IOKit/IOTypes.h>\n"
    header += "namespace UserDecoderGuardData {\n"
    header += "inline constexpr UInt64 Offsets[] = {\n"
    for label, offset in CANDIDATES:
        header += f"    0x{offset:X}ULL, // {label}\n"
    header += "};\n"
    header += "inline constexpr UInt32 OffsetCount = sizeof(Offsets) / sizeof(Offsets[0]);\n"
    header += f"inline constexpr UInt32 PatchSize = {PATCH_SIZE};\n"
    header += array("EntryOriginal", ENTRY_ORIGINAL)
    header += f"inline constexpr UInt32 PayloadJumpOffset = {jump_offset};\n"
    header += array("Payload", payload)
    header += "} // namespace UserDecoderGuardData\n"
    OUT.write_text(header)
    print(f"decoder guard: {len(CANDIDATES)} offsets "
          f"({', '.join(hex(o) for _, o in CANDIDATES)}) payload {len(payload)} bytes "
          f"jump offset {jump_offset} -> {OUT}")


if __name__ == "__main__":
    main()
