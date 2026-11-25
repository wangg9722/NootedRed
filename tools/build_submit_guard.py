"""生成提交守卫数据：核验 AMD Metal 驱动方法入口，汇编守卫并嵌入头文件。"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/submit-guard"
OUT = ROOT / "NootedRed/UserSubmitGuardData.hpp"
sys.path.insert(0, str(ROOT / "tools"))

from metal_cache import CacheReader, macho  # noqa: E402

# -[GFX9AMD_MtlCmdQueue submitCommandBuffers:count:] 的缓存内偏移与入口前 15 字节。
# 该方法在 AMDRadeonX5000MTLDriver 内的映像相对偏移恒为 0x99240，缓存相对偏移随映像基址变化；
# 每个候选都必须在目标缓存上逐字节核验入口，核验失败只跳过，不猜测。
CANDIDATES = [
    ("macOS 26.7.3", 0x310CE9240),   # 映像基址 0x7FFB10C50000
    ("macOS 26.4", 0x310BF1240),     # 映像基址 0x7FFB10B58000
]
# 提交方法在 AMDRadeonX5000MTLDriver 映像内的相对偏移；26.4 与 26.7.3 实测一致，
# 因此可先用路径解析映像基址再相加，避免为每个系统版本写一条缓存级绝对地址。
IMAGE_RELATIVE_OFFSET = 0x99240
METHOD_ORIGINAL = bytes.fromhex("554889e54883ec10488d45f0488938")
METHOD_PATCH_SIZE = len(METHOD_ORIGINAL)
PLACEHOLDER = bytes.fromhex("49bb8877665544332211")  # movabs $0x1122334455667788,%r11


def array(name: str, data: bytes) -> str:
    lines = [f"inline constexpr UInt8 {name}[] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> None:
    cache = CacheReader()
    verified = []
    for label, offset in CANDIDATES:
        entry = cache.read(0x7FF800000000 + offset, METHOD_PATCH_SIZE)
        if entry == METHOD_ORIGINAL:
            verified.append((label, offset))
    print("verified in this cache:", [(l, hex(o)) for l, o in verified])
    if not verified:
        raise SystemExit("AMD Metal 提交方法入口未核实，拒绝生成")
    # 头文件里写入全部候选：本机缓存只能核验本机版本，其余候选由运行时按 15 字节入口核验后才会安装。

    WORK.mkdir(parents=True, exist_ok=True)
    obj = WORK / "submit_guard.o"
    subprocess.run(["as", "-arch", "x86_64", str(ROOT / "tools/submit_guard.s"), "-o", str(obj)],
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

    header = "// 由 tools/build_submit_guard.py 生成，不手工编辑。\n#pragma once\n#include <IOKit/IOTypes.h>\n"
    header += "namespace UserSubmitGuardData {\n"
    header += "inline constexpr UInt64 Offsets[] = {\n"
    for label, offset in CANDIDATES:
        mark = "verified here" if (label, offset) in verified else "runtime-verified"
        header += f"    0x{offset:X}ULL, // {label} ({mark})\n"
    header += "};\n"
    header += "inline constexpr UInt32 OffsetCount = sizeof(Offsets) / sizeof(Offsets[0]);\n"
    header += f"inline constexpr UInt64 ImageRelativeOffset = 0x{IMAGE_RELATIVE_OFFSET:X}ULL;\n"
    header += array("MethodOriginal", METHOD_ORIGINAL)
    header += f"inline constexpr UInt32 MethodPatchSize = {METHOD_PATCH_SIZE};\n"
    header += f"inline constexpr UInt32 PayloadJumpOffset = {jump_offset};\n"
    header += array("Payload", payload)
    header += "} // namespace UserSubmitGuardData\n"
    OUT.write_text(header)
    print(f"submit guard: {len(CANDIDATES)} offsets payload {len(payload)} bytes "
          f"jump offset {jump_offset} -> {OUT}")


if __name__ == "__main__":
    main()
