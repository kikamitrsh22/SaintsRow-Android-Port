# Generate explicit function boundaries for switches whose labels fall outside the analysed function.
import re,struct,sys,bisect
sys.path.insert(0,'tools'); import pdata as P
d=P.d; B=P.B
TEXT=(0x820B0000,0x820B0000+0x6D9544)
def word(a): return struct.unpack('>I',d[a-B:a-B+4])[0]
# bl targets
bl=set()
for a in range(TEXT[0],TEXT[1],4):
    w=word(a)
    if w>>26==18 and (w&3)==1:
        li=w&0x03FFFFFC
        if li&0x02000000: li-=0x04000000
        bl.add((a+li)&0xFFFFFFFF)
bl=sorted(bl)
pends=sorted((s,s+n) for s,n in P.ents)
pstarts=[p[0] for p in pends]
# switch tables
tbl=open('config/SaintsRow_switch_tables.toml').read()
sw={}
for m in re.finditer(r'base = (0x[0-9A-F]+).*?labels = \[(.*?)\]',tbl,re.S):
    sw[int(m.group(1),16)]=[int(x,16) for x in re.findall(r'0x[0-9A-F]+',m.group(2))]
swb=sorted(sw)
bad=set()
for x in re.findall(r'Switch case at ([0-9A-F]+)',open('work/recomp.log').read()):
    a=int(x,16); i=bisect.bisect_right(swb,a)-1
    bad.add(swb[i])
bad=sorted(bad)
funcs={}
for base in bad:
    i=bisect.bisect_right(pstarts,base)-1
    inp = i>=0 and base < pends[i][1]
    gs = pends[i][1] if i>=0 else TEXT[0]
    if inp: gs=pends[i][0]
    ge = pstarts[i+1] if i+1<len(pstarts) else TEXT[1]
    while gs<ge and word(gs)==0: gs+=4
    j=bisect.bisect_right(bl,base)-1
    start = bl[j] if j>=0 and bl[j]>=gs else gs
    hi=max(sw[base]+[base])
    k=bisect.bisect_right(bl,hi)
    end = bl[k] if k<len(bl) and bl[k]<ge else ge
    while end>start and word(end-4)==0: end-=4
    funcs.setdefault(start,set()).add(end)
    print(f'{base:08X} start={start:08X} end={end:08X} maxlabel={hi:08X} {"in_pdata" if inp else "gap"} gap={gs:08X}-{ge:08X}',file=sys.stderr)
out=['functions = [']
for s in sorted(funcs):
    e=max(funcs[s]); out.append(f'    {{ address = 0x{s:08X}, size = 0x{e-s:X} }},')
out.append(']')
print('\n'.join(out))
