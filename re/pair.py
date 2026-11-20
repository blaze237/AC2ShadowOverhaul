import sys,re
o1,o2,N=sys.argv[1],sys.argv[2],int(sys.argv[3])
lines=open('text.asm').readlines()
r1=re.compile(r'^(\S+) (movss|fld|fmul|fadd|fcomp|fcom|fsub|fdiv|comiss|mulss|addss|subss|divss|minss|maxss|cvtss2sd|ucomiss|mov) [^\[]*dword ptr \[(e..) \+ '+o1+r'\]')
for i,l in enumerate(lines):
    m=r1.match(l)
    if m:
        reg=m.group(3)
        r2=re.compile(r'dword ptr \['+reg+r' \+ '+o2+r'\]')
        for j in range(max(0,i-N),min(len(lines),i+N)):
            if r2.search(lines[j]) and re.search(r' (movss|fld|fmul|fadd|comiss|mulss|addss|subss|divss|minss|maxss|cvtss2sd|ucomiss|fcomp|fcom|fsub|fdiv) ',lines[j]):
                print(l.strip(),'  |  ',lines[j].strip()); break
