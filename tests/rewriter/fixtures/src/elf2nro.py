#!/usr/bin/env python3
"""elf2nro.py <in.elf> <out.nro>: an aarch64 ELF linked with module.ld and crt0_nro.s as an NRO: the file is the image, its header at +0x10"""
import hashlib, struct, sys
src, dst = sys.argv[1:3]
d = open(src, 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20); phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
loads = []
for i in range(phnum):
    p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if p_type == 1: loads.append((p_offset, p_vaddr, p_filesz, p_memsz))
assert len(loads) == 3
page = lambda n: (n + 0xFFF) & ~0xFFF
end = page(loads[2][1] + loads[2][2])
img = bytearray(end)
for off, va, fsz, msz in loads: img[va:va + fsz] = d[off:off + fsz]
segs = [(loads[0][1], loads[1][1] - loads[0][1]), (loads[1][1], loads[2][1] - loads[1][1]), (loads[2][1], end - loads[2][1])]
bss = page(loads[2][1] + loads[2][3]) - end
img[0x10:0x14] = b'NRO0'
struct.pack_into('<II', img, 0x18, end, 0)
for i, (o, s) in enumerate(segs): struct.pack_into('<II', img, 0x20 + 8 * i, o, s)
struct.pack_into('<I', img, 0x38, bss)
img[0x40:0x60] = hashlib.sha256(bytes(img)).digest()
open(dst, 'wb').write(img)
print(f'{dst}: {[(hex(o), hex(s)) for o, s in segs]} bss {bss:#x} build {bytes(img[0x40:0x50]).hex()}')
