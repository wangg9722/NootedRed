#!/usr/bin/env python3
"""通用 Mach-O 能力分支探针（不假设缓存镜像，可扫磁盘上的 .bundle 可执行文件）

用途：在 GL/MT 驱动里定位「blend / dual-quad / caps bit」这类能力判定分支，
      输出候选地址（文件偏移 + vmaddr + 所在函数近似），供后续打补丁或比对。
用法：
  python3 macho_branch_probe.py <mach-o 文件> [--symbols 关键字] [--bytes 关键字]
"""
import sys, struct, re

def slices(data):
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic in (0xfeedfacf, 0xfeedface):
        yield data, 0
    elif magic in (0xcafebabe, 0xbebafeca):
        n = struct.unpack_from(">I", data, 4)[0]
        off = 8
        for _ in range(n):
            cputype, cpusub, foff, fsize, align = struct.unpack_from(">iiIII", data, off)
            yield data[foff:foff+fsize], foff
            off += 20
    else:
        raise SystemExit("not mach-o")

def parse_slice(sl):
    is64 = struct.unpack_from("<I", sl, 0)[0] == 0xfeedfacf
    ncmds = struct.unpack_from("<I", sl, 16)[0]
    off = 32 if is64 else 28
    segs, syms, strtab = [], None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", sl, off)
        if cmd in (0x19, 0x1):  # LC_SEGMENT_64 / LC_SEGMENT
            name = sl[off+8:off+24].rstrip(b"\0").decode()
            if is64:
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", sl, off+24)
            else:
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<IIII", sl, off+24)
            segs.append((name, vmaddr, vmsize, fileoff, filesize, off))
        elif cmd == 0x2:  # LC_SYMTAB
            syms = struct.unpack_from("<IIII", sl, off+8)
        elif cmd == 0xb:  # LC_DYSYMTAB
            pass
        off += size
    return segs, syms

def segment_of(segs, vm):
    for s in segs:
        if s[1] <= vm < s[1]+s[2]:
            return s
    return None

def main():
    path = sys.argv[1]
    symkw = ""
    bytekw = ""
    for i, a in enumerate(sys.argv):
        if a == "--symbols": symkw = sys.argv[i+1]
        if a == "--bytes": bytekw = sys.argv[i+1]
    raw = open(path, "rb").read()
    for sl, fbase in slices(raw):
        segs, symtab = parse_slice(sl)
        print("slice@%d segs=%s" % (fbase, [(s[0], hex(s[1]), hex(s[4])) for s in segs if s[0] in ("__TEXT","__text","__DATA","__LINKEDIT")]))
        # 符号表
        if symtab and symkw:
            symoff, nsyms, stroff, strsize = symtab
            is64 = struct.unpack_from("<I", sl, 0)[0] == 0xfeedfacf
            ent = 16 if is64 else 12
            hits = 0
            for i in range(nsyms):
                e = symoff + i*ent
                if is64:
                    strx, typ, sect, desc, value = struct.unpack_from("<IBBHQ", sl, e)
                else:
                    strx, value = struct.unpack_from("<II", sl, e)
                name = sl[stroff+strx: stroff+strsize].split(b"\0")[0].decode("utf8", "ignore")
                if symkw.lower() in name.lower() and hits < 20:
                    print("  sym %-60s vm=0x%x" % (name, value)); hits += 1
            print("  symbols matching '%s': %d" % (symkw, hits))
        # 指令模式扫描：test r/m8, imm8 (F6 /0 ib) 且 imm=2/4，紧邻 je/jne(74/75)
        text = None
        for s in segs:
            if s[0] in ("__TEXT",) :
                text = s
                break
        if text and bytekw:
            pass
    # 僵直扫描 __text 中 test byte [x], imm 后跟短跳转的模式
    for sl, fbase in slices(raw):
        segs, symtab = parse_slice(sl)
        txt = next((s for s in segs if s[0] == "__TEXT"), None)
        if not txt: continue
        vmaddr, vmsize, fileoff, filesize = txt[1], txt[2], txt[3], txt[4]
        data = sl[fileoff:fileoff+filesize]
        found = 0
        # F6 4x imm : test byte ptr [reg+disp], imm8
        for m in re.finditer(rb"\xf6[\x40-\x7f]\x02", data):
            a = m.start()
            ctx = data[max(0,a-6):a+14].hex()
            print("  cand test-byte[,,2] off=0x%x vm=0x%x ctx=%s" % (fileoff+a, vmaddr+a, ctx))
            found += 1
            if found > 40: break
        if found == 0:
            for m in re.finditer(rb"\x83[\xe0-\xe7]\xfd", data):  # and eax, 0xFFFFFFFD
                a = m.start()
                print("  cand and-reg~2 off=0x%x ctx=%s" % (fileoff+a, data[max(0,a-6):a+12].hex()))
                found += 1
                if found > 40: break

if __name__ == "__main__":
    main()
