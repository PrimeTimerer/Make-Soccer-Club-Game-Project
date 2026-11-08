"""Vita fake-signed SELF -> ELF: copies the ELF header and program headers and puts every segment (zlib-inflated when
compressed) at its p_offset. Usage: unself.py in.suprx out.elf"""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
assert data[:4] == b"SCE\0", "not a SELF"
elf_off, phdr_off, seg_off = struct.unpack_from("<QQ", data, 0x40) + (struct.unpack_from("<Q", data, 0x58)[0],)
elf_size = struct.unpack_from("<Q", data, 0x18)[0]
ehdr = data[elf_off:elf_off + 0x34]
e_phoff, = struct.unpack_from("<I", ehdr, 0x1C)
e_phentsize, e_phnum = struct.unpack_from("<HH", ehdr, 0x2A)
out = bytearray(elf_size)
out[0:0x34] = ehdr
phdrs = data[phdr_off:phdr_off + e_phentsize * e_phnum]
out[e_phoff:e_phoff + len(phdrs)] = phdrs
for i in range(e_phnum):
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<8I", phdrs, i * e_phentsize)
    s_offset, s_length, s_comp, s_enc = struct.unpack_from("<QQQQ", data, seg_off + i * 0x20)
    blob = data[s_offset:s_offset + s_length]
    if s_comp == 2:
        blob = zlib.decompress(blob)
    print("seg %d type %08X off %06X vaddr %08X filesz %06X memsz %06X flags %X | self off %X len %X comp %d enc %d -> %X bytes" % (
        i, p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags, s_offset, s_length, s_comp, s_enc, len(blob)))
    out[p_offset:p_offset + len(blob)] = blob
open(sys.argv[2], "wb").write(out)
print("wrote", sys.argv[2], len(out))
