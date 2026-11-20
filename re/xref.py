import sys; sys.path.insert(0,'.')
from img import *
import struct
def xrefs(va, start=TS, end=TE):
    return find_all(struct.pack('<I', va), start, end)
if __name__=='__main__':
    for a in sys.argv[1:]:
        v=int(a,16)
        r=xrefs(v)
        print(a, cstr(v), [hex(x) for x in r][:30])
