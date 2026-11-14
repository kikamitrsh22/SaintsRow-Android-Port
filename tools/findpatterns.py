import sys
d=open(sys.argv[1],'rb').read()
BASE=0x82000000
pats={
 'restgprlr_14':'e9c1ff68','savegprlr_14':'f9c1ff68',
 'restfpr_14':'c9ccff70','savefpr_14':'d9ccff70',
 'restvmx_14':'3960fee07dcb60ce','savevmx_14':'3960fee07dcb61ce',
 'restvmx_64':'3960fc00100b60cb','savevmx_64':'3960fc00100b61cb',
}
text=(0xB0000,0xB0000+0x6D9544)
for n,p in pats.items():
    b=bytes.fromhex(p); hits=[]; i=d.find(b,text[0])
    while i!=-1 and i<text[1]:
        if i%4==0: hits.append(hex(BASE+i))
        i=d.find(b,i+1)
    print(f'{n:14s}',hits[:5])
# RtlUnwind import thunk lookup helper: find longjmp candidates via 'mtctr'-less pattern skipped; print .text end
