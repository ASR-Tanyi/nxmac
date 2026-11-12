#!/usr/bin/env python3
"""fake_build.py <nxnative tree> <profile> <build ID> <out.s>: assembly of a stand-in for a registered build: the bytes its patch tables assert (cuts, observation gates,
frame-step and stub sites) at their offsets in a text of zeros (udf #0), so that the per-build code of nxnative's patches.py and nxmac's port can be compared on the
same image without the game. Nothing of the game is in it but those few asserted instruction words."""
import importlib.util, os, struct, sys, types
tree, profile, bid, out = sys.argv[1:5]
tools = os.path.join(tree, 'dm', 'tools'); sys.path.insert(0, tools)
from nso_asm import *
def load(path):
    spec = importlib.util.spec_from_file_location('p', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
mods = [load(os.path.join(tools, 'sdk_patches.py')), load(os.path.join(tools, f'../games/{profile}/patches.py'))]
sites = {}
def put(off, data):
    for i, b in enumerate(data):
        assert sites.get(off + i, b) == b, hex(off + i)
        sites[off + i] = b
for m in mods:
    for off, ob, nb, why in getattr(m, 'CUTS', {}).get(bid, []): put(off, bytes.fromhex(ob))
    for off, oid, ow, why in getattr(m, 'OBS', {}).get(bid, []): put(off, struct.pack('<I', int(ow, 16)))
    for kind in ('FRAME_STEP', 'STUBS'):
        fn = getattr(m, kind, {}).get(bid)
        if not fn: continue
        pos = [0x2000000]
        def address(off):   # an adrp + add pair is planted where the stub reads an address
            put(off, struct.pack('<2I', ADRP(8, off, 0x1f00000 + (off & 0xff0)), ADDI(8, 8, off & 0xff0))); return 0x1f00000 + 2 * (off & 0xff0)
        ctx = types.SimpleNamespace(here=lambda: pos[0], emit=lambda w: pos.__setitem__(0, pos[0] + 4), site=lambda off, orig: put(off, bytes.fromhex(orig)),
                                    put=lambda off, words, why: None, state=0x3000000, render_scale=0, base=0, others={}, found=None, capabilities=set(), address=address)
        fn(ctx)
end = (max(sites) + 0x1000) & ~0xfff
with open(out, 'w') as f:
    f.write('    .section .crt0, "ax"\n    .globl _start\n_start:\n    .cfi_startproc\n    b 1f\n    .word __mod0 - _start\n1:  ret\n    .cfi_endproc\n')
    f.write('    .globl fake_fn\n    .type fake_fn, %function\nfake_fn:\n    .cfi_startproc\n')
    at = 12
    for off in sorted(sites):
        if off != at: f.write(f'    .org {off:#x}\n')
        f.write(f'    .byte {sites[off]:#x}\n'); at = off + 1
    f.write(f'    .org {end:#x}\n    ret\n    .cfi_endproc\n')
    f.write('    .section .mod0, "a"\n    .balign 4\n__mod0:\n    .ascii "MOD0"\n    .word __dynamic_start - __mod0\n    .word __bss_start - __mod0\n    .word __bss_end - __mod0\n'
            '    .word __eh_frame_hdr_start - __mod0\n    .word __eh_frame_hdr_end - __mod0\n    .word 0\n    .data\n    .quad 1\n')
print(f'{out}: {len(sites)} asserted bytes, text {end:#x}')
