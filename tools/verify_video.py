import sys, re; sys.path.insert(0,".")
import metal_cache
from pathlib import Path
metal_cache.CACHE = Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld")
from metal_cache import CacheReader
BASE=0x7FF800000000
r=CacheReader()
text=Path("../NootedRed/UserVideoDecodeData.hpp").read_text()
m=re.search(r"inline constexpr Patch Patches\[\] = \{(.*?)\};", text, re.S)
body=m.group(1)
entries=re.findall(r"\{(0x[0-9A-F]+)ULL, (Original\d+), (Patched\d+), sizeof", body)
def arr(name):
    mm=re.search(r"inline constexpr UInt8 "+name+r"\[\] = \{(.*?)\};", text, re.S)
    return bytes(int(x,16) for x in re.findall(r"0x([0-9A-F]{2})", mm.group(1)))
ok=True
for off, orig, patched in entries:
    o=int(off,16)
    ob=arr(orig)
    live=r.read(BASE+o, len(ob))
    match = live==ob
    if not match: ok=False
    print(f"{orig:11s} off {o:#x} len {len(ob):3d} " + ("OK" if match else "MISMATCH live="+live.hex()[:40]))
print("ALL OK" if ok else "FAILED")
