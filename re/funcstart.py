import sys,bisect
lines=open('text.asm').readlines(); idx=[l[:8] for l in lines]
def fstart(a):
    i=bisect.bisect_left(idx,'%08x'%a)
    while i>0:
        if lines[i-1].split()[1]=='int3' and lines[i].split()[1]!='int3': return int(lines[i][:8],16)
        i-=1
def callers(t):
    s='call 0x%x\n'%t; j='jmp 0x%x\n'%t
    return [l[:8] for l in lines if l.endswith(s) or l.endswith(j)]
if __name__=='__main__':
    for a in sys.argv[1:]:
        f=fstart(int(a,16)); c=callers(f)
        print(a,'func',hex(f),'callers',c[:20], 'nfuncs', [hex(fstart(int(x,16))) for x in c[:20]])
