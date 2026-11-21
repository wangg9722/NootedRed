"""只读探针：在 AMDRadeonX5000GLDriver 中定位 dual-quad / dual-source blend 能力分支。

MTLDriver 侧的 LegacyBlend 门（GFX9_MtlRenderPipelineState ctor 内 test byte [reg+0xa0], 2
+ 条件跳过 + CB_COLOR_CONTROL 清位写回）已有成熟修法。本探针在 GL 驱动里找同类特征，
不写死任何地址/偏移，全部按符号与指令特征自适应。

用法：
    python3 tools/gl_blend_gate_probe.py [--json]

退出码：0 = 找到候选门；1 = 未找到。
"""
import argparse
import json
import sys
from pathlib import Path

import capstone

import metal_cache

GL_IMAGE = "/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/ATIRadeonX5000SCLib.dylib"

BRANCHES = {"je", "jne", "jz", "jnz", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg", "js", "jns"}
DQ_CLEAR_IMM = 0xFFFFFFFE  # and ..., ~1（清 dual-quad 位）的 32 位立即数
BLEND_SYMBOL_HINTS = ("blend", "Blend", "dualsource", "DualSource", "dual_source")


def mem_disp(instruction):
    for op in instruction.operands:
        if op.type == capstone.x86.X86_OP_MEM:
            return op.mem.disp
    return None


def first_imm(instruction):
    for op in instruction.operands:
        if op.type == capstone.x86.X86_OP_IMM:
            return op.imm & 0xFFFFFFFF
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", default=str(metal_cache.CACHE))
    parser.add_argument("--image", default=GL_IMAGE)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    metal_cache.CACHE = Path(args.cache)
    metal_cache.IMAGE = args.image

    reader = metal_cache.CacheReader()
    try:
        text = next(s for s in reader.sections if s[0] == "__text")
        base, size, _ = text[1], text[2], text[3]
        body = reader.read(base, size)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        instructions = list(md.disasm(body, base))

        # 1) 所有 and imm32 == 0xFFFFFFFE 的位置（dual-quad 清位强特征）
        dq_clears = [i for i in instructions
                     if i.mnemonic == "and" and first_imm(i) == DQ_CLEAR_IMM]

        # 2) test byte [mem], 2 且后面跟着条件跳转
        caps_tests = []
        for idx, ins in enumerate(instructions):
            if ins.mnemonic != "test":
                continue
            imm = first_imm(ins)
            disp = mem_disp(ins)
            if imm != 2 or disp is None:
                continue
            window = instructions[idx + 1: idx + 0x40]
            branch = next((i for i in window if i.mnemonic in BRANCHES), None)
            caps_tests.append({"address": ins.address, "rva": ins.address - reader.image_base,
                               "disp": disp, "branch": f"{branch.mnemonic} {branch.op_str}" if branch else None,
                               "branch_address": branch.address if branch else None})

        # 3) blend 相关符号
        blend_syms = [(a, n) for a, n in reader.symbols
                      if any(h in n for h in BLEND_SYMBOL_HINTS)]

        report = {
            "cache_id": reader.cache_id,
            "image_base": f"{reader.image_base:#x}",
            "text_size": size,
            "dq_clear_sites": [{"address": f"{i.address:#x}", "rva": i.address - reader.image_base,
                                "op": f"{i.mnemonic} {i.op_str}"} for i in dq_clears[:40]],
            "dq_clear_count": len(dq_clears),
            "caps_test_sites": [{**c, "address": f"{c['address']:#x}",
                                 "branch_address": f"{c['branch_address']:#x}" if c["branch_address"] else None}
                                for c in caps_tests[:60]],
            "caps_test_count": len(caps_tests),
            "blend_symbols": [{"address": f"{a:#x}", "name": n} for a, n in blend_syms[:60]],
            "blend_symbol_count": len(blend_syms),
        }
    finally:
        reader.close()

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"缓存：{report['cache_id']}  GLDriver base：{report['image_base']}  __text {size:#x}")
        print(f"and 0xFFFFFFFE 清位点：{report['dq_clear_count']}")
        for s in report["dq_clear_sites"][:20]:
            print(f"  {s['address']} (rva {s['rva']:#x})  {s['op']}")
        print(f"test [mem],2 + 条件跳转点：{report['caps_test_count']}")
        for s in report["caps_test_sites"][:20]:
            print(f"  {s['address']} (rva {s['rva']:#x}) disp={s['disp']:#x} -> {s['branch']}")
        print(f"blend 相关符号：{report['blend_symbol_count']}")
        for s in report["blend_symbols"][:30]:
            print(f"  {s['address']} {s['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
