"""定位 AMDRadeonVADriver2（dyld 缓存内）解码器类的函数与成员偏移。

用法：
    python3 tools/diagnostics/vadriver_probe.py [符号子串...]      # 默认 "9VADecoder"

输出：匹配符号的地址、函数长度，以及函数体内出现过的 [reg + off] 成员偏移集合。
用途：为「按分辨率拒绝超规格解码会话」寻找宽高字段的落点。
"""
import re
import struct
import sys

sys.path.insert(0, "tools")

import capstone  # noqa: E402

from metal_cache import CacheReader, macho  # noqa: E402

CACHE_MAP = "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h.map"
TARGET = "/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2"


def symbols_of_image(cache, path):
    lines = open(CACHE_MAP).read().splitlines()
    base = int(lines[lines.index(path) + 1].split()[1], 16)
    segments, _, table = macho(cache.read, base)
    link = next(s for s in segments if s[0] == "__LINKEDIT")
    offset, count, strings_offset, strings_size = table
    entries = cache.read(link[1] + offset - link[3], count * 16)
    strings = cache.read(link[1] + strings_offset - link[3], strings_size)
    result = []
    for i in range(count):
        index, kind, _, _, address = struct.unpack_from("<IBBHQ", entries, i * 16)
        if kind & 0xE != 0xE or not address:
            continue
        result.append((address, strings[index:strings.find(b"\0", index)].decode(errors="replace")))
    return base, sorted(result)


def main():
    keys = sys.argv[1:] or ["9VADecoder"]
    cache = CacheReader()
    _, table = symbols_of_image(cache, TARGET)
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for address, name in table:
        if not any(key in name for key in keys):
            continue
        end = min(a for a, _ in table if a > address)
        body = cache.read(address, end - address)
        offsets = {}
        for instruction in disassembler.disasm(body, address):
            match = re.search(r"\[(\w+) \+ (0x[0-9a-f]+)\]", instruction.op_str)
            if match:
                key = f"{match.group(1)}+{match.group(2)}"
                offsets[key] = offsets.get(key, 0) + 1
        print(f"{name} {address:#x} len={len(body)}")
        print("   ", sorted(offsets))
    cache.close()


if __name__ == "__main__":
    main()
