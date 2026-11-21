#!/usr/bin/env python3
"""离线核对 VP9 站点数据与真实 dyld 共享缓存是否逐字节一致。

用法：
  ./tools/verify_vp9_sites.py --data <数据集目录> --major 23|24|25 --dyld <dyld 目录>

数据集目录里应有 UserVP9DecodeData.hpp（Darwin 25）或对应版本的 *Data.hpp。
dyld 目录里应有 dyld_shared_cache_x86_64h 与 .01/.02... 子缓存。
脚本只读，不修改任何缓存文件。
"""
import argparse
import os
import re
import sys

ARRAY_RE = re.compile(r'Original(\d+)\[\]\s*=\s*\{(.*?)\};', re.S)
PATCH_RE = re.compile(r'\{(0x[0-9A-Fa-f]+)ULL,\s*Original(\d+),\s*Patched\2,\s*sizeof\(Original\2\)\}')
UUID_RE = re.compile(r'CacheUUID\[\]\s*=\s*\{(.*?)\};', re.S)
IMAGE_RE = re.compile(r'ImageReference ResolverImages\[\]\s*=\s*(.*?)\};', re.S)


def parse_array(body):
    return bytes(int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', body))


def load_data(path):
    text = open(path).read()
    arrays = {int(i): parse_array(body) for i, body in ARRAY_RE.findall(text)}
    patches = [(int(off, 16), int(idx)) for off, idx in PATCH_RE.findall(text)]
    uuid = bytes(int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', UUID_RE.search(text).group(1)))
    images = re.findall(r'"([^"]+)"', IMAGE_RE.search(text).group(1))
    return arrays, patches, uuid, images


def cache_prefix(uuid):
    return uuid.hex().upper()


def read_uuid(prefix):
    with open(prefix, 'rb') as f:
        head = f.read(0x68)
    return head[0x58:0x68]


def iter_cache_files(prefix):
    names = [prefix] + sorted(
        os.path.join(os.path.dirname(prefix), n)
        for n in os.listdir(os.path.dirname(prefix))
        if re.fullmatch(os.path.basename(prefix) + r'\.\d\d', n))
    return [n for n in names if os.path.exists(n)]


def find_bytes(files, needle, limit=3):
    hits = []
    total_base = 0
    for path in files:
        data = open(path, 'rb').read()
        start = 0
        while len(hits) < limit:
            idx = data.find(needle, start)
            if idx < 0:
                break
            hits.append((os.path.basename(path), total_base + idx))
            start = idx + 1
        total_base += len(data)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True)
    ap.add_argument('--major', required=True, type=int)
    ap.add_argument('--dyld', required=True)
    args = ap.parse_args()

    data_file = {25: 'UserVP9DecodeData.hpp', 24: 'UserVP9DecodeSequoiaData.hpp',
                 23: 'UserVP9DecodeSonomaData.hpp', 22: 'UserVP9DecodeVenturaData.hpp'}[args.major]
    arrays, patches, uuid, images = load_data(os.path.join(args.data, data_file))

    prefix = os.path.join(args.dyld, 'dyld_shared_cache_x86_64h')
    actual = read_uuid(prefix)
    print(f'dataset {data_file}: {len(patches)} sites, {len(images)} images')
    print(f'uuid dataset={uuid.hex().upper()}  cache={actual.hex().upper()}  match={uuid == actual}')
    if uuid != actual:
        print('UUID 不匹配：该数据集不是为这台机器的这个 OS build 生成的，禁止套用')
        return 2

    files = iter_cache_files(prefix)
    print(f'cache files: {len(files)}')
    ok = bad = 0
    for off, idx in patches:
        needle = arrays[idx]
        hits = find_bytes(files, needle)
        status = 'unique' if len(hits) == 1 else ('absent' if not hits else f'{len(hits)} hits')
        if len(hits) == 1:
            ok += 1
        else:
            bad += 1
            print(f'  site 0x{off:X} idx={idx} size={len(needle)} -> {status} {hits[:3]}')
    print(f'originals found uniquely: {ok}/{len(patches)}, problems: {bad}')
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
