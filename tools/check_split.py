"""Check whether a function can be split at an address: prints the instruction before the split and
any branches that cross the split point. Usage: check_split.py image.bin start split end ..."""
import struct, sys
BASE = 0x82000000
img = open(sys.argv[1], 'rb').read()
def w(a): return struct.unpack('>I', img[a - BASE:a - BASE + 4])[0]
def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v
def target(a):
    i = w(a); op = i >> 26
    if op == 18 and not (i & 2): return a + sx(i & 0x03FFFFFC, 26), bool(i & 1), 'b'
    if op == 16 and not (i & 2): return a + sx(i & 0xFFFC, 16), bool(i & 1), 'bc'
    return None
args = [int(x, 16) for x in sys.argv[2:]]
for k in range(0, len(args), 3):
    start, split, end = args[k:k + 3]
    prev = w(split - 4)
    kind = 'blr' if prev == 0x4E800020 else 'bctr' if prev == 0x4E800420 else ('b' if prev >> 26 == 18 and not prev & 1 else '%08X' % prev)
    cross = []
    for a in range(start, end, 4):
        t = target(a)
        if not t or t[1]: continue
        if start <= t[0] < end and ((a < split) != (t[0] < split)):
            cross.append('%08X->%08X' % (a, t[0]))
    print('%08X split %08X end %08X: prev=%s first=%08X cross=%s' % (start, split, end, kind, w(split), cross or 'none'))
