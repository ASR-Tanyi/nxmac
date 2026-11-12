#!/usr/bin/env python3
"""elf2nso.py <in.elf> <out.nso> [--bss <bytes>] [--plain] [--build-id <32 hex digits>]: an aarch64 ELF linked with module.ld as an NSO (LZ4 segments with their
hashes; --plain stores them as they are, unchecked). The build ID is the hash of the segments unless one is given (a stand-in for a registered build)."""
import hashlib, struct, sys
import lz4.block

def main():
    args = sys.argv[1:]
    plain = '--plain' in args
    if plain: args.remove('--plain')
    bss_extra = 0
    if '--bss' in args:
        i = args.index('--bss'); bss_extra = int(args[i + 1], 0); del args[i:i + 2]
    given = None
    if '--build-id' in args:
        i = args.index('--build-id'); given = bytes.fromhex(args[i + 1]); del args[i:i + 2]
    src, dst = args
    d = open(src, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 2
    phoff, = struct.unpack_from('<Q', d, 0x20); phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads = []
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if p_type == 1: loads.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    assert [f for f, *_ in loads] == [5, 4, 6], loads
    segs, bss = [], 0
    for k, (flags, off, va, fsz, msz) in enumerate(loads):
        assert va % 0x1000 == 0, hex(va)
        data = d[off:off + fsz]
        if k == 2: bss = (msz - fsz + 0xFFF) & ~0xFFF
        segs.append((va, data))
    build_id = hashlib.sha256(b''.join(s for _, s in segs)).digest()
    if given: build_id = given + build_id[len(given):]
    hdr = bytearray(0x100); hdr[:4] = b'NSO0'
    flags = 0
    out = b''; at = 0x100
    for k, (va, data) in enumerate(segs):
        stored = data
        if not plain:
            flags |= 8 << k   # its hash is checked
            packed = lz4.block.compress(data, store_size=False)
            if len(packed) < len(data): stored = packed; flags |= 1 << k   # a segment that does not shrink is stored as it is
        struct.pack_into('<III', hdr, 0x10 + 0x10 * k, at, va, len(data))
        struct.pack_into('<I', hdr, 0x60 + 4 * k, len(stored))
        hdr[0xA0 + 0x20 * k:0xC0 + 0x20 * k] = hashlib.sha256(data).digest()
        out += stored; at += len(stored)
    struct.pack_into('<I', hdr, 0xC, flags)
    struct.pack_into('<I', hdr, 0x3C, bss + bss_extra)
    hdr[0x40:0x60] = build_id
    open(dst, 'wb').write(bytes(hdr) + out)
    print(f'{dst}: text {len(segs[0][1]):#x} ro {segs[1][0]:#x}+{len(segs[1][1]):#x} data {segs[2][0]:#x}+{len(segs[2][1]):#x} bss {bss + bss_extra:#x} build {build_id[:16].hex()}')
main()
