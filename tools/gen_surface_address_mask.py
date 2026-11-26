"""生成补丁族的“地址字段掩码”：这些位置在不同小版本之间只承载 RIP 相对位移或分支目标，
字节必然变化但指令语义不变，匹配时应跳过。掩码表是只读常量，运行期不做反汇编。

用法：python3 tools/gen_surface_address_mask.py [--write]
不带 --write 时只打印统计，便于在改动数据表前后核对。
"""
import argparse
import re
import sys
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "NootedRed" / "UserSurfaceSyncData.hpp"
OUT = ROOT / "NootedRed" / "UserSurfaceSyncAddressMask.hpp"

ARRAY_RE = re.compile(r"inline constexpr UInt8 (\w+)\[\]\s*=\s*\{(.*?)\}", re.S)
BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{2})")

# 需要掩码的载荷：原始布局与补丁布局各自一张表。
TARGETS = [
    "ResolverOriginal0",
    "ScratchPatched",
    "ScratchPatched15D8",
    "ScratchPatched15DD",
    "ScratchPatched15E7",
    "ScratchPatched1636",
    "ScratchPatched164C",
    "BlendConstructor",
]

BRANCH = {"call", "jmp", "je", "jne", "jz", "jnz", "ja", "jb", "jae", "jbe", "jg", "jl", "jge", "jle", "js", "jns",
          "jo", "jno", "jp", "jnp", "jcxz", "jecxz", "jrcxz", "loop", "loope", "loopne"}


def payloads():
    text = DATA.read_text()
    return {name: bytes(int(v, 16) for v in BYTE_RE.findall(body))
            for name, body in ARRAY_RE.findall(text)}


def mask_of(code):
    """标记 RIP 相对位移与直接分支目标占用的字节位置。"""
    disasm = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    disasm.detail = True
    mask = set()
    for insn in disasm.disasm(code, 0):
        end = insn.address + insn.size
        for operand in insn.operands:
            if operand.type == capstone.x86.X86_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP:
                width = insn.encoding.disp_size or 4
                mask.update(range(end - width, end))
            elif operand.type == capstone.x86.X86_OP_IMM and insn.mnemonic in BRANCH:
                width = insn.encoding.imm_size or 4
                mask.update(range(end - width, end))
    return mask


def bitset(mask, size):
    out = bytearray((size + 7) // 8)
    for index in mask:
        out[index // 8] |= 1 << (index % 8)
    return bytes(out)


def emit(name, mask, size):
    lines = [f"inline constexpr UInt8 {name}[] = {{"]
    data = bitset(mask, size)
    for start in range(0, len(data), 16):
        chunk = ", ".join(f"0x{v:02X}" for v in data[start:start + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    data = payloads()
    header = ["// 由 tools/gen_surface_address_mask.py 生成，请勿手改。",
              "// 位 i 置位表示：参考载荷的第 i 字节是 RIP 相对位移或分支目标，跨小版本比较时必须跳过。",
              "#pragma once", "", "namespace UserSurfaceSyncAddressMask", "{"]
    stats = []
    for target in TARGETS:
        code = data[target]
        mask = mask_of(code)
        header.append(emit(f"{target}Mask", mask, len(code)))
        stats.append((target, len(code), len(mask)))
    header.append("}")
    header.append("")
    text = "\n".join(header)
    for name, size, masked in stats:
        print(f"{name:20s} {size:6d} 字节，掩码 {masked:5d} 字节（{masked / size:.1%}）")
    if args.write:
        OUT.write_text(text)
        print(f"已写入 {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
