#!/usr/bin/env python3
"""把参考实现的 VP9 站点数据转为本仓库的纯字节补丁表（只读参考实现，不复制其代码结构）。

规则（全部以真实 dyld 缓存的字节为准，数据集里的地址字段只在验证通过时才采信）：
  1. 数据集 CacheUUID 必须与本地缓存 UUID 完全一致，否则拒绝生成；
  2. 站点地址按三级解析：数据集偏移（字节完全吻合时）→ 目标镜像 text 内唯一命中 →
     全缓存唯一命中；任何一级不唯一即报错退出，不产出半成品；
  3. 输出为"相对主缓存 unslid 基址的偏移"，运行时地址 = shared region 基址 + 该偏移，
     因此不需要在运行时再做镜像查找。
"""
import argparse
import os
import re
import struct
import sys

PATCH_RE = re.compile(r'\{(0x[0-9A-Fa-f]+)ULL,\s*Original(\d+),\s*Patched\2,\s*sizeof\(Original\2\)\}')
ARRAY_RE = re.compile(r'(Original|Patched)(\d+)\[\]\s*=\s*\{(.*?)\};', re.S)
UUID_RE = re.compile(r'CacheUUID\[\]\s*=\s*\{(.*?)\};', re.S)


def word(buf, off):
    return struct.unpack_from('<I', buf, off)[0]


def wide(buf, off):
    return struct.unpack_from('<Q', buf, off)[0]


def cache_files(dyld):
    base = os.path.join(dyld, 'dyld_shared_cache_x86_64h')
    files = [base]
    for i in range(1, 32):
        p = f'{base}.{i:02d}'
        if os.path.exists(p):
            files.append(p)
    return [f for f in files if os.path.exists(f)]


def read_mappings(path):
    with open(path, 'rb') as f:
        head = f.read(0x2000)
    off, count = word(head, 16), word(head, 20)
    mappings = []
    for i in range(count):
        m = head[off + i * 32:off + i * 32 + 32]
        mappings.append((wide(m, 0), wide(m, 8), wide(m, 16)))
    return wide(head, 224), mappings


def text_range(map_path, image_path):
    """从 .map 里取镜像 __TEXT 的 [start, end)。"""
    image = None
    with open(map_path, 'r', errors='ignore') as f:
        for line in f:
            s = line.strip()
            if s.startswith('/'):
                image = s
            elif image == image_path and s.startswith('__TEXT'):
                parts = [t for t in s.split() if t != '->']
                if len(parts) >= 3:
                    return int(parts[1], 16), int(parts[2], 16)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True)
    ap.add_argument('--major', required=True, type=int)
    ap.add_argument('--dyld', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--constant', required=True)
    args = ap.parse_args()

    data_file = {25: 'UserVP9DecodeData.hpp', 24: 'UserVP9DecodeSequoiaData.hpp',
                 23: 'UserVP9DecodeSonomaData.hpp', 22: 'UserVP9DecodeVenturaData.hpp'}[args.major]
    text = open(os.path.join(args.data, data_file)).read()

    arrays = {}
    for kind, index, body in ARRAY_RE.findall(text):
        arrays[(kind, int(index))] = bytes(int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', body))
    patches = [(int(off, 16), int(i)) for off, i in PATCH_RE.findall(text)]
    uuid = bytes(int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', UUID_RE.search(text).group(1)))

    files = cache_files(args.dyld)
    main_path = files[0]
    unslid, _ = read_mappings(main_path)
    blobs = {p: open(p, 'rb').read() for p in files}
    maps = {p: read_mappings(p) for p in files}

    def to_vm(path, offset):
        for start, size, file_off in maps[path][1]:
            if file_off <= offset < file_off + size:
                return start + (offset - file_off)
        return None

    if uuid != blobs[main_path][0x58:0x68]:
        print(f'[x] UUID 不匹配：数据集 {uuid.hex().upper()} / 缓存 {blobs[main_path][0x58:0x68].hex().upper()}',
              file=sys.stderr)
        return 2

    target = text_range(os.path.join(args.dyld, 'dyld_shared_cache_x86_64h.map'), args.image)
    if target is None:
        print('[x] .map 里找不到目标镜像的 __TEXT', file=sys.stderr)
        return 2

    sites = []
    for off, index in patches:
        original, patched = arrays[('Original', index)], arrays[('Patched', index)]
        # 原始区是校验模式，补丁是实际写入长度：注入型站点的写入区可以比校验模式更长
        vmaddr = None
        # 1) 数据集偏移（0x3... 编码为 .03 子缓存内的文件偏移）在字节上成立时优先采信
        if off >= 0x300000000:
            candidate = None
            for path in files:
                file_off = off - 0x300000000
                if file_off + len(original) <= len(blobs[path]) and blobs[path][file_off:file_off + len(original)] == original:
                    candidate = to_vm(path, file_off)
                    break
            if candidate:
                vmaddr = candidate
        # 2) 目标镜像 text 内唯一命中
        if vmaddr is None:
            hits = []
            for path in files:
                for start, size, file_off in maps[path][1]:
                    begin, end = max(file_off, 0), file_off + size
                    segment = blobs[path][begin:end]
                    pos = segment.find(original)
                    while pos >= 0:
                        abs_vm = start + (begin - file_off) + pos
                        if target[0] <= abs_vm < target[1]:
                            hits.append(abs_vm)
                        pos = segment.find(original, pos + 1)
            if len(hits) == 1:
                vmaddr = hits[0]
            elif len(hits) > 1:
                print(f'[x] 站点 {index} 在目标镜像内有 {len(hits)} 处命中，无法唯一确定', file=sys.stderr)
                return 2
        # 3) 全缓存唯一命中
        if vmaddr is None:
            hits = []
            for path in files:
                segment = blobs[path]
                pos = segment.find(original)
                while pos >= 0:
                    value = to_vm(path, pos)
                    if value is not None:
                        hits.append(value)
                    pos = segment.find(original, pos + 1)
            if len(hits) != 1:
                print(f'[x] 站点 {index} 全缓存命中 {len(hits)} 处，无法唯一确定', file=sys.stderr)
                return 2
            vmaddr = hits[0]
        sites.append((vmaddr - unslid, original, patched, len(patched)))
        print(f'  site {index:2d}: rel 0x{vmaddr - unslid:X} size {len(original)}')

    rels = [rel for rel, _, _, _ in sites]
    if len(set(rels)) != len(rels):
        print('[x] 站点相对偏移重复', file=sys.stderr)
        return 2

    name = args.constant
    out = []
    out.append('// 由 tools/gen_vp9_sites.py 从参考实现的数据集生成；本文件只保留偏移与字节约束。')
    out.append('// 数据来源见 docs/vp9-hw-decode-analysis.md 的致谢一节。')
    out.append('#pragma once')
    out.append('#include <IOKit/IOTypes.h>')
    out.append('')
    out.append(f'namespace {name}')
    out.append('{')
    out.append('    struct Site')
    out.append('    {')
    out.append('        UInt64       offset;')
    out.append('        UInt32       size;')
    out.append('        UInt32       patchSize;')
    out.append('        const UInt8* original;')
    out.append('        const UInt8* patched;')
    out.append('    };')
    out.append('')
    out.append('    inline constexpr UInt8 UUID[] = {')
    out.append('        ' + ', '.join(f'0x{b:02X}' for b in uuid))
    out.append('    };')
    out.append('')
    for i, (rel, original, patched, _) in enumerate(sites):
        for kind, body in (('Original', original), ('Patched', patched)):
            out.append(f'    inline constexpr UInt8 Site{i}{kind}[] = {{')
            for j in range(0, len(body), 16):
                out.append('        ' + ', '.join(f'0x{b:02X}' for b in body[j:j + 16]) + ',')
            out.append('    };')
    out.append('')
    out.append('    inline constexpr Site Sites[] = {')
    for i, (rel, original, patched, patch_size) in enumerate(sites):
        out.append(f'        {{0x{rel:X}ULL, sizeof(Site{i}Original), {patch_size}U, Site{i}Original, Site{i}Patched}},')
    out.append('    };')
    out.append('    inline constexpr UInt32 SiteCount = sizeof(Sites) / sizeof(Sites[0]);')
    out.append('    inline constexpr UInt32 LargestSite = %d;' % max(len(o) for _, o, _, _ in sites))
    out.append('    inline constexpr UInt32 LargestPatch = %d;' % max(pl for _, _, _, pl in sites))
    out.append('}    // namespace ' + name)
    out.append('')

    path = args.out
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    open(path, 'w').write('\n'.join(out))
    print(f'[ok] {len(sites)} sites -> {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
