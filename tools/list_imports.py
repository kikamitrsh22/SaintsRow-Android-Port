# List XEX import slots (type-0 records in .rdata) with ordinal, export name and kind.
# Usage: python list_imports.py <default.xex> <decrypted image from xexdump>
# Note: the decrypted image has its import records byte-swapped in place by XenonUtils,
# and code thunks already replaced with nop/blr, so we read the IAT slots only.
import re
import struct
import sys

xex = open(sys.argv[1], 'rb').read()
img = open(sys.argv[2], 'rb').read()
BASE = 0x82000000
TEXT = (0x820B0000, 0x827A0000)


def table(path):
    m = {}
    for o, n, k in re.findall(r'XE_EXPORT\([^,]+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)', open(path).read()):
        m[int(o, 16)] = (n, k)
    return m


names = {
    'xboxkrnl.exe': table('tools/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc'),
    'xam.xex': table('tools/XenonRecomp/XenonUtils/xbox/xam_table.inc'),
}

nhdr = struct.unpack('>I', xex[20:24])[0]
off = None
for i in range(nhdr):
    k, v = struct.unpack('>II', xex[24 + i * 8:32 + i * 8])
    if k >> 8 == 0x103:
        off = v
_, sts, n = struct.unpack('>III', xex[off:off + 12])
libs = []
p = off + 12
for _ in range(n):
    e = xex.index(b'\0', p)
    libs.append(xex[p:e].decode())
    p += ((e - p + 1) + 3) & ~3

p = off + 12 + sts
for lib in libs:
    size = struct.unpack('>I', xex[p:p + 4])[0]
    nimp = struct.unpack('>H', xex[p + 0x26:p + 0x28])[0]
    for j in range(nimp):
        slot = struct.unpack('>I', xex[p + 0x28 + 4 * j:p + 0x2C + 4 * j])[0]
        if TEXT[0] <= slot < TEXT[1]:
            continue  # code thunk
        ordinal = struct.unpack('<H', img[slot - BASE:slot - BASE + 2])[0]
        name, kind = names.get(lib, {}).get(ordinal, ('?', '?'))
        print(f'{lib}\t{kind}\t0x{ordinal:03X}\t{name}\t0x{slot:08X}')
    p += size
