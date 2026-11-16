import sys; sys.path.insert(0,'.')
from img import *
md2 = Cs(CS_ARCH_X86, CS_MODE_32); md2.skipdata=True
code = rd(TS, TE-TS)
with open('text.asm','w') as f:
    for (addr,size,mn,op) in md2.disasm_lite(code, TS):
        f.write(f'{addr:08x} {mn} {op}\n')
