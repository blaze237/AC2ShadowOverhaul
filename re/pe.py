import pefile, sys, struct
pe = pefile.PE(sys.argv[1], fast_load=True)
print(hex(pe.OPTIONAL_HEADER.ImageBase))
for s in pe.sections:
    print(s.Name.rstrip(b'\0'), hex(s.VirtualAddress+pe.OPTIONAL_HEADER.ImageBase), hex(s.Misc_VirtualSize), hex(s.PointerToRawData), hex(s.SizeOfRawData))
