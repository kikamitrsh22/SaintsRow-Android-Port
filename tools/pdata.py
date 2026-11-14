import struct,sys,bisect
d=open(sys.argv[1],'rb').read(); B=0x82000000
off,size=0x8AC00,0x1FBB8
ents=[]
for i in range(0,size,8):
    beg,info=struct.unpack('>II',d[off+i:off+i+8])
    if beg==0: continue
    ents.append((beg,((info>>8)&0x3FFFFF)*4))
ents.sort()
starts=[e[0] for e in ents]
def find(a):
    i=bisect.bisect_right(starts,a)-1
    return ents[i] if i>=0 else None
if __name__=='__main__':
    print(len(ents),'pdata entries')
    for a in sys.argv[2:]:
        a=int(a,16); i=bisect.bisect_right(starts,a)-1
        for e in ents[max(i-1,0):i+3]: print(hex(a),'->',hex(e[0]),hex(e[1]),hex(e[0]+e[1]))
        print()
