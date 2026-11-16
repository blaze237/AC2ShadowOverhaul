# find lines with pattern A, then report lines within N following lines matching pattern B
import sys,re
pa=re.compile(sys.argv[1]); pb=re.compile(sys.argv[2]); N=int(sys.argv[3])
lines=open('text.asm').readlines()
i=0
while i<len(lines):
    if pa.search(lines[i]):
        hits=[l.rstrip() for l in lines[i+1:i+1+N] if pb.search(l)]
        if hits:
            print('==', lines[i].rstrip()); print('   '+'\n   '.join(hits))
    i+=1
