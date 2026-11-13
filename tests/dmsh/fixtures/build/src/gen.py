#!/usr/bin/env python3
# A made-up game for the shader build: Maxwell (NVN 1) vertex, pixel and compute programs written with a small assembler, packed as shader_split.py
# packs them (records BTCDBTCD + key, the code area, the control section), with a profile folder that names them (game.tsv and the shader tables).
# Usage: gen.py <out folder>: <out>/pkg/*.bin and <out>/profile/0100000000FA4E00/
import hashlib, json, os, struct, sys

PT, RZ = 7, 255
SCHED = 0x001f8000fc0007e0
NOP = 0x50b0000000070f00

def opc(pattern):
    p = pattern.replace('x', '0'); return int(p.ljust(64, '0'), 2)

def f(v, lo, width=64): return (v & ((1 << width) - 1)) << lo

def base(pattern, pred=PT): return opc(pattern) | f(pred, 16, 3)

def ald(rd, attr, n=4, ra=RZ): return base('1110111111011') | f(rd, 0, 8) | f(ra, 8, 8) | f(attr, 20, 11) | f(RZ, 39, 8) | f(n - 1, 47, 2)
def ast(rs, attr, n=4, ra=RZ): return base('1110111111110') | f(rs, 0, 8) | f(ra, 8, 8) | f(attr, 20, 11) | f(RZ, 39, 8) | f(n - 1, 47, 2)
def ipa(rd, attr, mul=None): return base('11100000') | f(rd, 0, 8) | f(RZ, 8, 8) | f(mul if mul is not None else RZ, 20, 8) | f(attr, 28, 10) | f(RZ, 39, 8) | f(PT, 47, 3) | f(1 if mul is not None else 0, 54, 2)
def mufu(rd, ra, op): return base('0101000010000') | f(rd, 0, 8) | f(ra, 8, 8) | f(op, 20, 4)
def texs(rd, rd2, ra, rb, handle, target=1, wmask=4): return base('1101100') | f(rd, 0, 8) | f(ra, 8, 8) | f(rb, 20, 8) | f(rd2, 28, 8) | f(handle, 36, 13) | f(wmask, 50, 3) | f(target, 53, 4)
def exit_(): return base('111000110000') | 0xf
def kil(pred=PT): return base('111000110011', pred) | 0xf
def mov32i(rd, v): return base('000000010000') | f(rd, 0, 8) | f(0xf, 12, 4) | f(v, 20, 32)
def movc(rd, slot, off): return base('0100110010011') | f(rd, 0, 8) | f(off >> 2, 20, 14) | f(slot, 34, 5) | f(0xf, 39, 4)
def ffmar(rd, ra, rb, rc): return base('010110011') | f(rd, 0, 8) | f(ra, 8, 8) | f(rb, 20, 8) | f(rc, 39, 8)
def ffmac(rd, ra, slot, off, rc): return base('010010011') | f(rd, 0, 8) | f(ra, 8, 8) | f(off >> 2, 20, 14) | f(slot, 34, 5) | f(rc, 39, 8)
def fmulc(rd, ra, slot, off): return base('0100110001101') | f(rd, 0, 8) | f(ra, 8, 8) | f(off >> 2, 20, 14) | f(slot, 34, 5)
def fmulr(rd, ra, rb): return base('0101110001101') | f(rd, 0, 8) | f(ra, 8, 8) | f(rb, 20, 8)
def faddr(rd, ra, rb): return base('0101110001011') | f(rd, 0, 8) | f(ra, 8, 8) | f(rb, 20, 8)
def s2r(rd, sreg): return base('1111000011001') | f(rd, 0, 8) | f(sreg, 20, 8)
def sts(rs, ra, off, size=4): return base('1110111101011') | f(rs, 0, 8) | f(ra, 8, 8) | f(off, 20, 24) | f(size, 48, 3)
def lds(rd, ra, off, size=4): return base('1110111101001') | f(rd, 0, 8) | f(ra, 8, 8) | f(off, 20, 24) | f(size, 48, 3)
def stg(rs, ra, off=0, size=4): return base('1110111011011') | f(rs, 0, 8) | f(ra, 8, 8) | f(off, 20, 24) | f(1, 45, 1) | f(size, 48, 3)
def ldg(rd, ra, off=0, size=4): return base('1110111011010') | f(rd, 0, 8) | f(ra, 8, 8) | f(off, 20, 24) | f(1, 45, 1) | f(size, 48, 3)
def ldc(rd, ra, slot, off, size=4): return base('1110111110010') | f(rd, 0, 8) | f(ra, 8, 8) | f(off, 20, 16) | f(slot, 36, 5) | f(size, 48, 3)
def bar(): return base('1111000010101') | f(0xb, 32, 4)   # BAR.SYNC 0
def iadd32i(rd, ra, v): return base('0001110') | f(rd, 0, 8) | f(ra, 8, 8) | f(v, 20, 32)
def i2fr(rd, rb, signed=False): return base('0101110010111') | f(rd, 0, 8) | f(rb, 20, 8) | f(2, 8, 2) | f(2, 10, 2) | f(int(signed), 13, 1)   # I2F.F32.U32/S32
def shli(rd, ra, sh): return opc('0011100001001') | f(PT, 16, 3) | f(rd, 0, 8) | f(ra, 8, 8) | f(sh, 20, 19)

def code(*insts):
    insts = list(insts) + [exit_()]
    out = b''
    for k in range(0, len(insts), 3):
        g = (insts[k:k + 3] + [NOP, NOP])[:3]
        out += struct.pack('<4Q', SCHED, *g)
    return out + struct.pack('<4Q', SCHED, 0xe2400fffff87000f, NOP, NOP)   # BRA to itself after the end, as GLSLC leaves it

def sph(stage, omap=0, imap=(), kills=False, depth=False):
    w = [0] * 20
    w[0] = 1 | (1 << 5) | (stage << 10) | (int(kills) << 15) | (1 << 17)
    for i, t in enumerate(imap):   # generic attribute i: four components of interpolation type t (2 perspective, 1 constant)
        w[6 + (i >> 2)] |= (t | t << 2 | t << 4 | t << 6) << ((i & 3) * 8)
    w[18] = omap; w[19] = 2 if depth else 0
    return struct.pack('<20I', *w)

def program(stage, insts, c1=b'', **kw):   # the code area of a graphics program: a 0x30 header, the SPH, the instructions, then c1
    head = bytes.fromhex('78563412') + b'\0' * 0x2c
    body = head + sph({'vs': 1, 'ps': 5}[stage], **kw) + code(*insts)
    return body, len(body), len(body) + len(c1), body + c1

def compute(insts, c1=b''):
    head = bytes.fromhex('79563412') + b'\0' * 0xfc
    body = head + code(*insts)
    return body, len(body), len(body) + len(c1), body + c1

def control(c1o, c1t, local=(0, 0, 0), shared=0, early=False, tf=None):
    c = bytearray(0x800)
    c[0:4] = bytes.fromhex('34127698')
    struct.pack_into('<2I', c, 0x700, c1o, c1t)
    struct.pack_into('<3I', c, 0x748, *local); struct.pack_into('<I', c, 0x754, shared)
    if early: c[0x718] = 1
    for b, outs in enumerate(tf or ()):   # transform feedback buffer b: (output attribute byte, component count) each
        if not outs: continue
        struct.pack_into('<H', c, 0x6dc + 2 * b, 4 * sum(n for _, n in outs)); c[0x6e8 + b] = len(outs)
        for i, (o, n) in enumerate(outs): c[0xdc + 0x80 * b + i] = o; c[0x4dc + 0x80 * b + i] = n
    return bytes(c)

def tex_handle(slot): return (0x20 + 8 * slot) // 4   # c2 word of combined texture slot r

POS, GEN = 0x70, 0x80
shaders = []   # (stage, name, code area, control, facts)
def add(stage, name, prog, ctl_kw=None, facts=None):
    body, c1o, c1t, full = prog
    shaders.append((stage, name, full, control(c1o, c1t, **(ctl_kw or {})), facts))

# vertex: position from attribute 0, varying 0 from attribute 1 scaled by uniform slot 0 (c[3]), varying 1 = attribute 2
add('vs', 'basic', program('vs', [ald(0, GEN, 4), ald(4, GEN + 16, 4), ald(8, GEN + 32, 4),
                                   fmulc(4, 4, 3, 0), fmulc(5, 5, 3, 4),
                                   ast(0, POS, 4), ast(4, GEN, 4), ast(8, GEN + 16, 4)]))
# vertex: a full-screen pass (position straight from attribute 0, uv straight from attribute 1)
add('vs', 'fullscreen', program('vs', [ald(0, GEN, 4), ald(4, GEN + 16, 2), mov32i(6, 0), mov32i(7, 0x3f800000), ast(0, POS, 4), ast(4, GEN, 4)]),
    ctl_kw={'tf': [[(0x20, 4)], [], [(0x20, 2), (0x21, 1)]]})   # transform feedback: varying 0 into buffer 0, parts of it into buffer 2
# vertex: an integer attribute used as an index (bone), the base instance word c0 + 0x644, a c1 read
add('vs', 'skinned', program('vs', [ald(0, GEN, 4), ald(4, GEN + 16, 1), shli(5, 4, 4), iadd32i(5, 5, 0x100), ldc(8, 5, 3, 0),
                                     movc(9, 0, 0x644), i2fr(9, 9), movc(10, 1, 0x0),
                                     faddr(0, 0, 8), faddr(1, 1, 9), faddr(2, 2, 10), ast(0, POS, 4), ast(8, GEN, 4)], c1=struct.pack('<4f', 1, 2, 3, 4)))
# pixel: one texture sampled at varying 0's uv (a readable-tile candidate), into colour 0
w = 0
add('ps', 'textured', program('ps', [ipa(0, POS + 0xc), mufu(0, 0, 4), ipa(1, GEN, 0), ipa(2, GEN + 4, 0), texs(4, 6, 1, 2, tex_handle(0)),
                                     mov32i(0, 0), mov32i(1, 0), mov32i(2, 0), mov32i(3, 0), faddr(0, 4, RZ), faddr(1, 5, RZ), faddr(2, 6, RZ), faddr(3, 7, RZ)],
                              omap=0xf, imap=[2, 2]))
# pixel: two colour outputs, constant colours, a uniform-scaled one (alpha test / logic op candidates), early fragment tests
add('ps', 'two_targets', program('ps', [ipa(0, POS + 0xc), mufu(0, 0, 4), ipa(4, GEN + 16, 0), fmulc(4, 4, 3, 0x10),
                                        mov32i(0, 0x3f800000), mov32i(1, 0), mov32i(2, 0), mov32i(3, 0x3f000000), faddr(5, 4, 4), mov32i(6, 0), mov32i(7, 0x3f800000)],
                                 omap=0xff, imap=[2, 2]), ctl_kw={'early': True})
# pixel: integer colour (bits written as floats), kills pixels
add('ps', 'integer', program('ps', [mov32i(0, 7), mov32i(1, 1), mov32i(2, 0), mov32i(3, 255)], omap=0xf, kills=True))
# pixel: two textures, the second sampled at a computed coordinate
add('ps', 'blend', program('ps', [ipa(0, POS + 0xc), mufu(0, 0, 4), ipa(1, GEN, 0), ipa(2, GEN + 4, 0), texs(4, 6, 1, 2, tex_handle(0)),
                                  fmulr(8, 1, 1), texs(10, 12, 8, 2, tex_handle(1)), faddr(0, 4, 10), faddr(1, 5, 11), faddr(2, 6, 12), faddr(3, 7, 13)],
                           omap=0xf, imap=[2, 2]))
# compute: thread index into shared memory, a barrier, a global store through storage buffer 0 (c0 + 0x310: address, size)
add('cs', 'shared', compute([s2r(0, 0x21), shli(1, 0, 2), sts(0, 1, 0), bar(), lds(2, 1, 4), ldc(4, RZ, 0, 0x310, 5), iadd32i(4, 4, 0), stg(2, 4, 0)]),
    ctl_kw={'local': (32, 1, 1), 'shared': 0x100}, facts={'local': [32, 1, 1], 'shared': 0x100})
# compute: shared memory past Metal's 32 KB (48 KB, as Mario Kart World's), stored at its end
add('cs', 'big_shared', compute([s2r(0, 0x21), shli(1, 0, 2), sts(0, 1, 0xbf00), bar(), lds(2, 1, 0xbf04), ldc(4, RZ, 0, 0x310, 5), stg(2, 4, 0)]),
    ctl_kw={'local': (64, 1, 1), 'shared': 0xc000}, facts={'local': [64, 1, 1], 'shared': 0xc000})
# vertex: a storage buffer write (c0 + 0x110: storage buffer 0 of the vertex stage), so it needs a rasterizer-discard entry point
add('vs', 'writer', program('vs', [ald(0, GEN, 4), ldc(4, RZ, 0, 0x110, 5), stg(0, 4, 0), ast(0, POS, 4)]))
# compute: a uniform read and a global load and store
add('cs', 'copy', compute([s2r(0, 0x21), ldc(4, RZ, 0, 0x310, 5), ldg(6, 4, 0), movc(7, 3, 0), faddr(6, 6, 7), stg(6, 4, 4)]),
    ctl_kw={'local': (64, 1, 1)}, facts={'local': [64, 1, 1], 'shared': 0})

out = sys.argv[1]
pkg = os.path.join(out, 'pkg'); game = '0100000000FA4E00'; prof = os.path.join(out, 'profile', game)
os.makedirs(pkg, exist_ok=True); os.makedirs(prof, exist_ok=True)
packages, rows, facts, keys = {}, [], [], {}
for stage, name, area, ctl, fc in shaders:
    key = hashlib.sha256(ctl + area).hexdigest()[:16]
    keys[name] = key
    rec = b'BTCDBTCD' + bytes.fromhex(key) + b'\0' * 0x28
    rec = rec[:0x28] + struct.pack('<2I', len(ctl), len(area)) + rec[0x30:] + area + ctl
    pname = f'{stage}_test.bin'
    rows.append(f'{pname}\t{len([r for r in rows if r.startswith(pname)])}\t{stage}\t{key}\t/Shader/{name}.bnsh#0')
    packages[pname] = packages.get(pname, b'') + rec
    if fc is not None: facts.append(f'{key}\t' + json.dumps(fc, sort_keys=True, separators=(',', ':')))
for n, d in packages.items(): open(os.path.join(pkg, n), 'wb').write(d)
open(os.path.join(prof, 'game.tsv'), 'w').write(f'title_id\t{game}\nname\tA made-up game\nshader_versions\tv1114112\nshader_groups\t/Shader/=shader\nshader_note\ta made-up game\n')
open(os.path.join(prof, 'shader_map.tsv'), 'w').write('# kind: evidence  a made-up game\n# column 5 records where each key first appears\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(r + '\n' for r in rows))
open(os.path.join(prof, 'shader_map.tsv.conflicts.tsv'), 'w').write('# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n')
open(os.path.join(prof, 'shader_facts.tsv'), 'w').write('# Answer table\n' + ''.join(sorted(r + '\n' for r in facts)))
pairs = [(keys['basic'], keys['textured']), (keys['basic'], keys['blend']), (keys['fullscreen'], keys['two_targets']), (keys['skinned'], keys['integer'])]
open(os.path.join(prof, 'shader_pairs.tsv'), 'w').write('# vertex\tpixel\n' + ''.join(f'{v}\t{p}\n' for v, p in pairs[:3]))
open(os.path.join(prof, 'pso_pairs.tsv'), 'w').write('# vertex\tpixel\n' + ''.join(f'{v}\t{p}\n' for v, p in pairs[3:]))
open(os.path.join(prof, 'bf_want.tsv'), 'w').write(f'# generated\n{keys["two_targets"]}\n')
open(os.path.join(prof, 'dual_want.tsv'), 'w').write(f'{keys["two_targets"]}\n')
print(len(shaders), 'shaders:', ' '.join(f'{n}={k}' for n, k in keys.items()))
