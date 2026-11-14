# Finds `bctr` jump tables XenonAnalyse missed and emits [[switch]] entries for them.
#
# Handled shapes (walking back from `mtctr rX; [nop]; bctr`):
#   A) word table:      lis/addi rT ; rlwinm rI2, rIDX, 2,0,29 ; lwzx rX, rT, rI2
#   B) byte offsets:    lis/addi rT ; lbzx r0, rT, rIDX ; rlwinm r0,r0,2,0,29 ; lis/addi rB ; add rX, rB, r0
#   C) half offsets:    lis/addi rT ; rlwinm r0, rIDX, 1,0,30 ; lhzx r0, rT, r0 ; lis/addi rB ; add rX, rB, r0
# The entry count comes from a preceding `cmplwi rIDX, N` when there is one; otherwise entries are
# collected while they stay inside the containing function.
#
# Usage: python find_inline_switches.py <decrypted image> <existing switch toml> > new_switches.toml
import re
import struct
import sys

sys.path.insert(0, 'tools')
img = open(sys.argv[1], 'rb').read()
known = {int(x, 16) for x in re.findall(r'base = (0x[0-9A-Fa-f]+)', open(sys.argv[2]).read())}
B = 0x82000000
TEXT = (0x820B0000, 0x820B0000 + 0x6D9544)


def w(a):
    return struct.unpack('>I', img[a - B:a - B + 4])[0]


def h(a):
    return struct.unpack('>H', img[a - B:a - B + 2])[0]


def simm(x):
    return x - 0x10000 if x & 0x8000 else x


# .pdata function bounds
pdata = []
for i in range(0, 0x1FBB8, 8):
    beg, info = struct.unpack('>II', img[0x8AC00 + i:0x8AC08 + i])
    if beg:
        pdata.append((beg, beg + ((info >> 8) & 0x3FFFFF) * 4))
pdata.sort()
starts = [p[0] for p in pdata]


def func_bounds(a):
    import bisect
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and pdata[i][0] <= a < pdata[i][1]:
        return pdata[i]
    lo = pdata[i][1] if i >= 0 else TEXT[0]
    hi = pdata[i + 1][0] if i + 1 < len(pdata) else TEXT[1]
    return (lo, hi)


def fields(v):
    return v >> 26, (v >> 21) & 31, (v >> 16) & 31, (v >> 11) & 31


def is_x(v, xo):
    return (v >> 26) == 31 and ((v >> 1) & 0x3FF) == xo


def find_const(a, reg, limit=6):
    """Walk back from a for `addi reg,reg,LO` preceded (not necessarily adjacent) by `lis reg,HI`."""
    lo = None
    for i in range(1, limit + 1):
        v = w(a - 4 * i)
        op, rd, ra, _ = fields(v)
        if lo is None and op == 14 and rd == reg and ra == reg:
            lo = simm(v & 0xFFFF)
        elif op == 15 and rd == reg:
            if lo is None:
                return (simm(v & 0xFFFF) << 16) & 0xFFFFFFFF
            return ((simm(v & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
    return None


def find_bound(a, reg, limit=12):
    for i in range(1, limit + 1):
        v = w(a - 4 * i)
        op, rd, ra, _ = fields(v)
        if op == 10 and ra == reg:  # cmplwi crN, reg, N
            return (v & 0xFFFF) + 1
    return None


BCTR = 0x4E800420
found, unknown = [], []
for a in range(TEXT[0] + 40, TEXT[1], 4):
    if w(a) != BCTR or a in known:
        continue
    p = a - 4
    if w(p) == 0x60000000:  # nop
        p -= 4
    mt = w(p)
    if (mt & 0xFC1FFFFF) != 0x7C0903A6:
        unknown.append(a)
        continue
    rx = (mt >> 21) & 31
    prev = w(p - 4)
    lo_f, hi_f = func_bounds(a)
    kind = None

    if is_x(prev, 23) and fields(prev)[1] == rx:  # A: lwzx rX, rT, rI2
        _, _, rt, ri2 = fields(prev)
        rl = w(p - 8)
        if (rl >> 26) == 21 and fields(rl)[2] == ri2 and ((rl >> 11) & 31) == 2 and ((rl >> 1) & 31) == 29:
            idx = fields(rl)[1]
            table = find_const(p - 8, rt)
            if table:
                kind = 'word'
                bound = find_bound(p - 8, idx)

                def entry(i, table=table):
                    return w(table + 4 * i)

    elif is_x(prev, 266) and fields(prev)[1] == rx:  # B/C: add rX, rB, r0
        _, _, rb, roff = fields(prev)
        if roff == rb:
            roff, rb = fields(prev)[2], fields(prev)[3]
        base_addr = find_const(p - 4, rb, 3)
        # skip the lis/addi of rB
        q = p - 4
        while q > p - 24 and (w(q - 4) >> 26) in (14, 15) and fields(w(q - 4))[1] == rb:
            q -= 4
        v1, v2 = w(q - 4), w(q - 8)
        if base_addr and (v1 >> 26) == 21 and is_x(v2, 87):  # rlwinm r0,r0,2 ; lbzx r0,rT,rIDX
            _, _, rt, idx = fields(v2)
            table = find_const(q - 8, rt)
            if table:
                kind = 'byte'
                bound = find_bound(q - 8, idx)

                def entry(i, table=table, base_addr=base_addr):
                    return base_addr + img[table - B + i] * 4
        elif base_addr and is_x(v1, 279) and (v2 >> 26) == 21:  # lhzx r0,rT,r0 ; rlwinm r0,rIDX,1
            _, _, rt, _ = fields(v1)
            idx = fields(v2)[1]
            table = find_const(q - 8, rt)
            if table:
                kind = 'half'
                bound = find_bound(q - 8, idx)

                def entry(i, table=table, base_addr=base_addr):
                    return base_addr + h(table + 2 * i)

    if not kind:
        unknown.append(a)
        continue

    labels = []
    limit = bound if bound else 512
    for i in range(limit):
        v = entry(i)
        if v % 4 or not (lo_f <= v < hi_f):
            break
        labels.append(v)
    if not labels or (bound and len(labels) != bound):
        unknown.append(a)
        print(f'#   rejected {kind} switch at 0x{a:08X}: {len(labels)} labels, bound {bound}', file=sys.stderr)
        continue
    found.append((a, idx, labels, kind, bound))

print('# Generated by tools/find_inline_switches.py: jump tables XenonAnalyse missed')
for a, idx, labels, kind, bound in found:
    print(f'\n# {kind} table{"" if bound else ", unbounded"}')
    print('[[switch]]')
    print(f'base = 0x{a:08X}')
    print(f'r = {idx}')
    print('default = 0x0')
    print('labels = [')
    for l in labels:
        print(f'    0x{l:08X},')
    print(']')

kinds = {}
for f in found:
    kinds[f[3]] = kinds.get(f[3], 0) + 1
print(f'# {len(found)} switches added {kinds}, {len(unknown)} other bctr left as indirect jumps', file=sys.stderr)
