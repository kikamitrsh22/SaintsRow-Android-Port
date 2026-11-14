# Finds functions that are only referenced through data (vtables, callback tables) and were not
# recompiled as separate functions. Prints `{ address, size }` entries for config/SaintsRow.toml.
#
# A data word is taken as a function pointer when it points into .text, is not already a function
# start, is not a switch label, and the previous instruction ends a function (blr, b, or padding)
# or the target starts with a typical prologue.
#
# Usage: python find_data_functions.py <decrypted image> <ppc_func_mapping.cpp> <switch toml>
import bisect
import re
import struct
import sys

img = open(sys.argv[1], 'rb').read()
mapped = sorted(int(x, 16) for x in re.findall(r'\{ (0x[0-9A-F]+), ', open(sys.argv[2]).read()))
labels = {int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{8}', open(sys.argv[3]).read())}
B = 0x82000000
TEXT = (0x820B0000, 0x820B0000 + 0x6D9544)
DATA = [(0x82000400, 0x8208AABC), (0x827A0000, 0x827A0000 + 0x191C70C)]


def w(a):
    return struct.unpack('>I', img[a - B:a - B + 4])[0]


mset = set(mapped)
cands = set()
for lo, hi in DATA:
    for a in range(lo, hi - 3, 4):
        v = w(a)
        if TEXT[0] <= v < TEXT[1] and v % 4 == 0 and v not in mset and v not in labels:
            prev = w(v - 4)
            first = w(v)
            ends = prev in (0x4E800020, 0x00000000) or (prev >> 26) == 18 and not (prev & 1)
            prologue = first in (0x7D8802A6, 0x7C0802A6) or (first >> 16) == 0x9421 or (first & 0xFFFF0000) == 0xFBE10000
            if ends or prologue:
                cands.add(v)

allstarts = sorted(set(mapped) | cands)
for v in sorted(cands):
    i = bisect.bisect_right(allstarts, v)
    end = allstarts[i] if i < len(allstarts) else TEXT[1]
    # trim trailing padding
    while end > v + 4 and w(end - 4) == 0:
        end -= 4
    print(f'    {{ address = 0x{v:08X}, size = 0x{end - v:X} }},')
print(f'# {len(cands)} data-referenced functions', file=sys.stderr)
