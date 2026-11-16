import sys,re; sys.path.insert(0,'.')
from img import *
def scan(p):
    toks=p.split(); rx=b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in toks)
    data=rd(TS,TE-TS)
    return [TS+m.start() for m in re.finditer(rx,data,re.S)]
sigs={
 'splits':"F3 0F 10 8E 1C 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D AC 74 0A F3 0F 10 8E 20 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D A8 74 0A F3 0F 10 8E 24 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D A4 74 08 F3 0F 10 86 28 05 00 00 85 F6 F3 0F 11 45 A0 74 10 D9 86 2C 05 00 00 D9 E0 DC 0D ?? ?? ?? ?? EB 06 DD 05 ?? ?? ?? ?? 85 F6 D9 5D CC 74 0A F3 0F 10 86 2C 05 00 00 EB 08 F3 0F 10 05 ?? ?? ?? ?? 8B 73 14 8B 43 10",
 'fade':"8B CE E8 ?? ?? ?? ?? D9 86 30 05 00 00 D9 5D F4 F3 0F 10 05 ?? ?? ?? ?? D9 86 34 05 00 00",
 'bias':"D9 86 2C 05 00 00 DC 0D ?? ?? ?? ?? DC 3D ?? ?? ?? ?? D9 5D F4",
}
for k,v in sigs.items():
    r=scan(v); print(k,[hex(x) for x in r], 'len',len(v.split()))
