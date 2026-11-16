# dump function from text.asm starting at addr until a ret followed by int3 padding
import sys,re
start=sys.argv[1].lower().zfill(8); maxl=int(sys.argv[2]) if len(sys.argv)>2 else 3000
sys.path.insert(0,'.')
from img import f32,u32,BASE,mem,cstr
lines=open('text.asm').readlines() if 'LINES' not in globals() else LINES
import bisect
idx=[l[:8] for l in lines]
i=bisect.bisect_left(idx,start)
out=0
while out<maxl:
    l=lines[i].rstrip(); extra=''
    for m in re.findall(r'\[(0x[0-9a-f]{6,8})\]', l):
        v=int(m,16)
        if BASE<=v<BASE+len(mem)-4: extra+=f' ; {m}=%g'%f32(v)
    print(l+extra); out+=1
    if (' ret' in l) and lines[i+1].split()[1]=='int3': break
    i+=1
