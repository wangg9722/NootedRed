"""只读探针：定位并校验 LegacyBlend 门（GFX9_MtlRenderPipelineState ctor 内的能力位分支）。

目的：把「26.x 的门到底在哪、语义是什么、能不能安全打补丁」变成可重复执行的检查，
避免再出现「按固定地址预生成字节」在系统小版本上打到无关代码的情况。

用法：
    python3 tools/blend_gate_probe.py
    python3 tools/blend_gate_probe.py --cache /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
    python3 tools/blend_gate_probe.py --json

退出码：0 = 门已定位且四条语义特征全部成立；1 = 未定位/特征不符（stderr 给出原因）。
"""
import argparse
import json
import sys
from pathlib import Path

import capstone

import metal_cache

CONSTRUCTOR_SYMBOL = ("-[GFX9_MtlRenderPipelineState initWithDevice:pipelineStateDescriptor:"
                      "vertexVariant:fragmentVariant:]")
CTOR_SYMBOL_HINTS = ("GFX9_MtlRenderPipelineState", "initWithDevice")
CAPS_OFFSET = 0xA0          # 能力报告结构内的 dual-quad 支持位所在偏移
CAPS_MASK = 2              # bit1
COLOR_CONTROL_OFFSET = 0x28  # CB_COLOR_CONTROL 在 pipeline 目标结构中的偏移
DISABLE_DUAL_QUAD_CLEAR = 0xFFFFFFFE
BRANCHES = {"je", "jne", "jz", "jnz", "jb", "jae", "jbe", "ja", "jl", "jge", "jle", "jg", "js", "jns"}


def find_constructor(reader):
    """按符号名定位 ctor；精确名缺失时退化为模糊匹配。"""
    for address, name in reader.symbols:
        if name == CONSTRUCTOR_SYMBOL:
            return address, name
    for address, name in reader.symbols:
        if all(hint in name for hint in CTOR_SYMBOL_HINTS):
            return address, name
    raise LookupError("符号表里找不到 GFX9_MtlRenderPipelineState 的 ctor")


def function_end(reader, address):
    ends = [a for a, _ in reader.symbols if a > address]
    return min(ends) if ends else address + 0x4000


def mem_disp(instruction):
    for op in instruction.operands:
        if op.type == capstone.x86.X86_OP_MEM:
            return op.mem.disp
    return None


def first_imm(instruction):
    for op in instruction.operands:
        if op.type == capstone.x86.X86_OP_IMM:
            return op.imm
    return None


def disassemble(md, data, address):
    return list(md.disasm(data, address))


def is_color_control_write(instructions):
    """检查一段指令里是否存在 CB_COLOR_CONTROL 的读取-清位-或-写回序列（只记录，不强行拒绝）。"""
    seen = {"load": False, "clear": False, "merge": False, "store": False}
    for ins in instructions[:12]:
        if ins.mnemonic == "mov" and mem_disp(ins) == COLOR_CONTROL_OFFSET and not seen["load"]:
            seen["load"] = True
            continue
        if ins.mnemonic == "and" and first_imm(ins) == DISABLE_DUAL_QUAD_CLEAR:
            seen["clear"] = True
            continue
        if ins.mnemonic == "or" and seen["clear"] and len(ins.operands) == 2 \
                and all(op.type == capstone.x86.X86_OP_REG for op in ins.operands):
            seen["merge"] = True
            continue
        if ins.mnemonic == "mov" and mem_disp(ins) == COLOR_CONTROL_OFFSET and seen["clear"]:
            seen["store"] = True
            break
    return all(seen.values()), seen


def analyse(reader):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    ctor, symbol = find_constructor(reader)
    size = function_end(reader, ctor) - ctor
    body = reader.read(ctor, size)
    instructions = disassemble(md, body, ctor)

    gates, caps_sites = [], []
    for index, ins in enumerate(instructions):
        if ins.mnemonic == "test" and mem_disp(ins) == CAPS_OFFSET and first_imm(ins) == CAPS_MASK:
            caps_sites.append(ins)
            following = instructions[index + 1:index + 0x40]
            branch = next((i for i in following if i.mnemonic in BRANCHES), None)
            if branch is None or first_imm(branch) is None:
                gates.append({"offset": ins.address - ctor, "branch": None})
                continue
            target = first_imm(branch)
            target_body = reader.read(target, 24)
            target_ins = disassemble(md, target_body, target)
            ok, parts = is_color_control_write(target_ins)
            dual_source = any(i.mnemonic == "setne" and first_imm(i) is None for i in following)
            rbplus = any((i.mnemonic == "test" and first_imm(i) == 0x400000000)
                         or (i.mnemonic == "bt" and first_imm(i) == 0x22)
                         or (i.mnemonic in ("movabs", "mov") and first_imm(i) == 0x400000000)
                         for i in following)
            form = {"je": "caps0_skip", "jne": "caps1_skip"}.get(branch.mnemonic, "other")
            gates.append({
                "offset": ins.address - ctor,
                "address": ins.address,
                "rva": ins.address - reader.image_base,
                "branch": f"{branch.mnemonic} {branch.op_str}",
                "branch_offset": branch.address - ctor,
                "branch_bytes": reader.read(branch.address, 2).hex(),
                "form": form,
                "target": f"{target:#x}",
                "target_is_color_control": ok,
                "target_parts": parts,
                "target_snippet": [f"{i.mnemonic} {i.op_str}" for i in target_ins[:5]],
                "computes_dual_source": dual_source,
                "checks_rbplus": rbplus,
            })

    text = reader.read(reader.image_base, 0x437EB3) if False else None  # 占位，避免误用
    return {"symbol": symbol, "constructor": f"{ctor:#x}", "constructor_rva": ctor - reader.image_base,
            "constructor_size": size, "caps_sites_in_ctor": len(caps_sites), "gates": gates}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", default=str(metal_cache.CACHE))
    parser.add_argument("--image", default=metal_cache.IMAGE)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    metal_cache.CACHE = Path(args.cache)
    metal_cache.IMAGE = args.image

    reader = metal_cache.CacheReader()
    try:
        report = analyse(reader)
        report["cache_id"] = reader.cache_id
        report["image_base"] = f"{reader.image_base:#x}"
    finally:
        reader.close()

    gates = report["gates"]
    if not gates:
        print("未在 ctor 内找到 test byte [reg+0xa0], 2 —— 该版本可能不使用此门", file=sys.stderr)
        if args.json:
            print(json.dumps(report, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1

    chose, advice = None, None
    for gate in gates:
        if gate["form"] == "caps0_skip":
            if not gate["target_is_color_control"]:
                print(f"门 ctor+{gate['offset']:#x} 是 caps0_skip，但目标不是 CB_COLOR_CONTROL 写回序列，拒绝打补丁",
                      file=sys.stderr)
                print(json.dumps(report, ensure_ascii=False, indent=2), file=sys.stderr)
                return 1
            chose = gate
            advice = ("26 形态（je 跳过计算）：补丁必须放行 gate 并让 allowRBPlus 判定在该分支视为真，"
                      "结果 = dualSource → 只对双源混合禁用；直接 NOP 该门等于对全部 pipeline 禁用双 quad，"
                      "会引发 FenceNotRetired / FullAsicReset")
            break
        if gate["form"] == "caps1_skip" and chose is None:
            chose = gate
            advice = ("14/15 形态（jne 跳过）：按上游做法把 75 0B 改成 EB 0B 即可，"
                      "保留后续计算与状态写入")
    if chose is None:
        print("找到能力位检查，但分支形态无法归类 —— 拒绝打补丁", file=sys.stderr)
        print(json.dumps(report, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1

    if args.json:
        report["selected_gate"] = chose
        report["advice"] = advice
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"缓存：{report['cache_id']}  驱动 image base：{report['image_base']}")
        print(f"ctor：{report['symbol']} @ {report['constructor']}（rva {report['constructor_rva']:#x}，"
              f"{report['constructor_size']} 字节）")
        print(f"ctor 内能力位检查点：{report['caps_sites_in_ctor']} 处")
        print(f"\n门：能力位检查 ctor + {chose['offset']:#x}，条件跳转 ctor + {chose['branch_offset']:#x}"
              f"（仓库锚点偏移；rva {chose['rva']:#x} = image+{chose['rva']:#x}）")
        print(f"  指令：test byte [..+0xa0], 2 ; {chose['branch']}（原始字节 {chose['branch_bytes']}）")
        print(f"  形态：{chose['form']}")
        print(f"  跳转目标：{chose['target']}，CB_COLOR_CONTROL 写回序列：{chose['target_is_color_control']} "
              f"{chose['target_parts']}")
        print(f"  目标前几条：{' | '.join(chose['target_snippet'])}")
        print(f"  分支内计算 dualSource：{chose['computes_dual_source']}；校验 allowRBPlus：{chose['checks_rbplus']}")
        print(f"\n结论：{advice}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
