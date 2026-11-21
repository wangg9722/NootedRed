#!/usr/bin/env python3
"""解析 macOS .ips 崩溃报告（兼容单行 body 与多行 pretty body），打印头部信息与 faultingThread 栈。"""
import json, sys, os, glob

def load(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    lines = raw.split("\n")
    head = json.loads(lines[0])
    body = None
    for i in (1, 0):
        try:
            body = json.loads("\n".join(lines[i:]))
            break
        except Exception:
            continue
    return head, body

def plan():
    for p in sys.argv[1:]:
        if os.path.isdir(p):
            fs = sorted(glob.glob(os.path.join(p, "*.ips")), key=os.path.getmtime, reverse=True)
        else:
            fs = [p]
        for f in fs:
            head, body = load(f)
            ex = (body or {}).get("exception", {})
            print("==", os.path.basename(f))
            print("   proc=%s time=%s decl=%s" % (head.get("app_name"), head.get("timestamp"), ex.get("type")))
            print("   signal=%s codes=%s" % (ex.get("signal"), str(ex.get("codes"))[:80]))
            ths = (body or {}).get("threads", [])
            ft = (body or {}).get("faultingThread", 0)
            if ft < len(ths):
                imgs = (body or {}).get("usedImages", [])
                for fr in ths[ft]["frames"][:10]:
                    img = imgs[fr["imageIndex"]] if fr["imageIndex"] < len(imgs) else {}
                    print("     %-26s %-50s +%s" % (str(img.get("name"))[:26], str(fr.get("symbol"))[:50], fr.get("imageOffset")))

if __name__ == "__main__":
    plan()
