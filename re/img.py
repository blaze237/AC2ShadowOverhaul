import pefile, struct, sys, re, os, pickle
from capstone import *
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'AssassinsCreedIIGame.exe')
pe = pefile.PE(EXE, fast_load=True)
BASE = pe.OPTIONAL_HEADER.ImageBase
mem = bytearray(pe.get_memory_mapped_image())
TEXT = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
TS, TE = BASE+TEXT.VirtualAddress, BASE+TEXT.VirtualAddress+TEXT.Misc_VirtualSize
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = False
def rd(va, n): return bytes(mem[va-BASE:va-BASE+n])
def u32(va): return struct.unpack('<I', rd(va,4))[0]
def f32(va): return struct.unpack('<f', rd(va,4))[0]
def dis(va, n=40, stop_ret=False):
    out=[]
    for i in md.disasm(rd(va, n*15), va):
        out.append(i)
        if len(out)>=n or (stop_ret and i.mnemonic in ('ret','retn')): break
    return out
def pd(va, n=40, stop_ret=False):
    for i in dis(va,n,stop_ret):
        extra=''
        for m in re.findall(r'0x[0-9a-f]{6,8}', i.op_str):
            v=int(m,16)
            if BASE<=v<BASE+len(mem)-4:
                fv=f32(v)
                if 'dword ptr [0x' in i.op_str or 'movss' in i.mnemonic: extra+=f' ; [{m}]=%g/0x%x'%(fv,u32(v))
                s=cstr(v)
                if s: extra+=f' ; "{s}"'
        print(f'{i.address:08x}: {i.bytes.hex():<20} {i.mnemonic} {i.op_str}{extra}')
def cstr(va, mx=80):
    b=rd(va,mx); e=b.find(b'\0')
    if e>=3 and all(32<=c<127 for c in b[:e]): return b[:e].decode()
    return None
def find_all(pat, start=BASE, end=None):
    end = end or BASE+len(mem)
    s=bytes(mem[start-BASE:end-BASE]); res=[]; i=s.find(pat)
    while i!=-1: res.append(start+i); i=s.find(pat,i+1)
    return res
