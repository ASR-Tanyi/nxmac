#!/usr/bin/env python3
"""gen.py [--low] <out dir> [<nxnative checkout>]: made-up games for the shader split: RomFS folders, mains and the arguments to split them with,
in every form the split reads (src/shader/include/nxmac/shader/split.h), no game data. Each case: <out>/<case>/case.json {"game": id,
"versions": [[RomFS, main], ...], "dlc": [...]} (paths relative to the case). The bytes around the magics and fields are random, or with --low
(the committed fixtures) a few random bytes then a ramp of values below 0x40, which holds no magic and compresses well in git.
nxnative's dm/tools/romfs_image.py packs the image case."""
import gzip, json, os, random, shutil, struct, subprocess, sys, zlib

CTL, GFX, CS = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
GLSLC = bytes.fromhex('91688619')
BAD = [CTL, GFX, CS, GLSLC, b'BNSH', b'BAHS', b'BTCD', b'MGFX', b'SARC', b'Yaz0', b'RFRM', b'NVNsh', b'HNSH', b'rgxd', b'SLCT',
       b'Ninty', b'#ver', b'NDSR', b'NDVE', b'MSHD', b'grsc', b'DAT\0', b'VIPR', b'RYHP', b'tfxo', b'Unit', b'PDAT', b'CRA.', b'PK\x03\x04']
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH = {'vs': 1, 'hs': 2, 'ds': 3, 'gs': 4, 'ps': 5}
R = random.Random(1)
LOW = False
NXNATIVE = '/home/claude/nxnative'


def rnd(n):
    if LOW:
        head = bytes(b & 0x3f for b in R.randbytes(min(n, 16)))
        step, start = R.randrange(1, 8), R.randrange(64)
        return head + bytes((start + step * i) & 0x3f for i in range(n - len(head)))
    while True:
        b = R.randbytes(n)
        if not any(x in b for x in BAD): return b


def pad(b, a):
    return b + bytes(-len(b) % a)


def ctl(code_len, length=0x880, own=None, cs=False, tg=(32, 1, 1), shared=0, nvn2=False, stage=None, tail=b''):
    """A control section: a 0x880-byte NVN one (own length at +0x20, the code length at +0x704) or an NVN2 one"""
    if nvn2:
        c = bytearray(rnd(max(length, 0xd8)))
        c[0:4] = CTL; c[4:8] = b'\xc9\0\0\0'
        struct.pack_into('<I', c, 0x30, len(c)); struct.pack_into('<I', c, 0x44, code_len)
        c[0x56] = {'cs': 5, 'ts': 6, 'ms': 7}.get(stage, 0)
        if stage == 'cs': struct.pack_into('<3I', c, 0x58, *tg); struct.pack_into('<I', c, 0x64, shared)
        return bytes(c)
    c = bytearray(rnd(length))
    c[0:4] = CTL; c[4:8] = struct.pack('<I', 0x1d)
    struct.pack_into('<I', c, 0x20, own if own is not None else length)
    struct.pack_into('<I', c, 0x704, code_len)
    if cs: struct.pack_into('<3I', c, 0x748, *tg); struct.pack_into('<I', c, 0x754, shared)
    else: c[0x748:0x754] = bytes(12)
    return bytes(c) + tail


def code(stage, n=None, inputs=None, headerless=False, nvn2=False):
    n = n or R.randrange(0x10, 0x60) * 16
    b = bytearray(rnd(n))
    if nvn2:   # no code header: the SPH at +0 (its stage bits read through nvn2_stage)
        struct.pack_into('<I', b, 0, 0x61 | SPH.get(stage, 1) << 10)
        return bytes(b)
    if stage == 'cs':
        if headerless: b[0:4] = b'\x10\x20\x30\x40'
        else: b[0:4] = CS
        return bytes(b)
    b[0:4] = GFX
    struct.pack_into('<I', b, 0x30, 0x61 | SPH[stage] << 10)
    if stage == 'ps':
        ins = inputs if inputs is not None else R.sample(range(32), R.randrange(1, 8))
        for i in range(32): b[0x48 + i] = (R.randrange(1, 255) if i in ins else 0)
    return bytes(b)


def shader(stage, **kw):
    """(control, code) of a fresh shader of stage"""
    nvn2 = kw.pop('nvn2', False)
    k = code(stage, nvn2=nvn2, **{x: kw.pop(x) for x in ('n', 'inputs', 'headerless') if x in kw})
    if nvn2: return ctl(len(k), nvn2=True, stage=stage, **kw), k
    return ctl(len(k), cs=stage == 'cs', **kw), k


def bnsh(variations, mesh=False):
    """A BNSH file: variations = [ {stage: (control, code)} or None ]"""
    out = bytearray(0x100)
    out[0:8] = b'BNSH\0\0\0\0'; out[8:0x60] = rnd(0x58); out[0x60:0x64] = b'grsc'
    out[0x64:0x7c] = bytes(0x18)
    pvar = 0x100
    struct.pack_into('<IQ', out, 0x7c, len(variations), pvar)
    out[0x88:0x100] = bytes(0x78)
    out += bytes(0x40 * len(variations))
    for v, var in enumerate(variations):
        if var is None: continue
        prog = len(out); out += bytes(0x50)
        struct.pack_into('<Q', out, pvar + 0x40 * v + 0x10, prog)
        for st, (c, k) in var.items():
            desc = len(out); out += bytes(0x20)
            slot = prog + (0x40 if st == 'ts' else 0x48 if st == 'ms' else 8 + 8 * STAGES.index(st))
            struct.pack_into('<Q', out, slot, desc)
            out = bytearray(pad(bytes(out), 0x10)); pc = len(out); out += c
            out = bytearray(pad(bytes(out), 0x100)); pk = len(out); out += k
            struct.pack_into('<QQII', out, desc + 8, pc, pk, len(k), len(c))
    out = bytearray(pad(bytes(out), 0x10))
    struct.pack_into('<I', out, 0x1c, len(out))
    return bytes(out)


def bahs(programs, loose=()):
    """An agl BAHS file: programs = [[(vs, ps), ...] variations]; loose = [(kind, (control, code))] binaries of no program (compute: kind 3)"""
    bins = []   # (kind, control, code)
    bases = []
    for prog in programs:
        bases.append(len(bins))
        for vs, ps in prog:
            if vs: bins.append((0, *vs))
            if ps: bins.append((1, *ps))
    bases_end = len(bins)
    for kind, (c, k) in loose: bins.append((kind, c, k))
    arr = 0x40
    acnt = len(bins); asz = 8 + 0x20 * acnt
    names = [pad(f'prog{i}'.encode() + b'\0', 4) for i in range(len(programs))]
    psz = 8 + sum(16 + len(n) for n in names)
    ctl_at = (arr + asz + psz + 0xf) & ~0xf
    ctls, at = [], ctl_at
    for _, c, _ in bins: ctls.append(at); at += (len(c) + 0xf) & ~0xf
    region = (at + 0xff) & ~0xff
    codes, off = [], 0
    for _, _, k in bins: codes.append(off); off = (off + len(k) + 0xff) & ~0xff
    d = bytearray(region + off)
    d[0:4] = b'BAHS'; d[4:0x18] = bytes(0x14)
    struct.pack_into('<II', d, 0x18, off, region)
    struct.pack_into('<II', d, arr, asz, acnt)
    for i, (kind, c, k) in enumerate(bins):
        r = arr + 8 + 0x20 * i; a = r + 0x10
        struct.pack_into('<IIII', d, r, 0x20, kind, 0, 0)
        struct.pack_into('<IIIi', d, a, codes[i], 0, len(c), ctls[i] - a)
        d[ctls[i]:ctls[i] + len(c)] = c
        d[region + codes[i]:region + codes[i] + len(k)] = k
    p = arr + asz
    struct.pack_into('<II', d, p, psz, len(programs)); q = p + 8
    for i, n in enumerate(names):
        struct.pack_into('<IIII', d, q, 16 + len(n), len(n), 0x11, bases[i]); d[q + 16:q + 16 + len(n)] = n; q += 16 + len(n)
    return bytes(d)


def sarc(members):
    """A SARC: members = [(name or None, bytes)]"""
    hl, n = 0x14, len(members)
    names, attrs = b'', []
    for nm, _ in members:
        if nm is None: attrs.append(0)
        else: attrs.append(0x01000000 | len(names) // 4); names += pad(nm.encode() + b'\0', 4)
    names_at = hl + 12 + 16 * n + 8
    doff = (names_at + len(names) + 0xff) & ~0xff
    data, ranges = b'', []
    for _, m in members:
        data = pad(data, 0x80); ranges.append((len(data), len(data) + len(m))); data += m
    d = bytearray(doff) + data
    struct.pack_into('<4sHHIIHH', d, 0, b'SARC', hl, 0xFEFF, len(d), doff, 0x100, 0)
    struct.pack_into('<4sHHI', d, hl, b'SFAT', 0xc, n, 0x65)
    for i, ((s, e), a) in enumerate(zip(ranges, attrs)):
        struct.pack_into('<IIII', d, hl + 12 + 16 * i, R.getrandbits(32), a, s, e)
    struct.pack_into('<4sHH', d, names_at - 8, b'SFNT', 8, 0)
    d[names_at:names_at + len(names)] = names
    return bytes(d)


def yaz0(data):
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8))
    i, n, table = 0, len(data), {}
    def add(p):
        if p + 3 <= n: table.setdefault(data[p:p + 3], []).append(p)
    while i < n:
        fp = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            best_len, best_dist = 0, 0
            for p in reversed(table.get(data[i:i + 3], [])[-24:]):
                dist = i - p
                if dist > 0x1000: break
                ln = 0
                while ln < 0x111 and i + ln < n and data[p + ln] == data[i + ln]: ln += 1
                if ln > best_len: best_len, best_dist = ln, dist
            if best_len >= 3:
                dd = best_dist - 1
                if best_len >= 0x12: out += bytes([dd >> 8, dd & 0xff, best_len - 0x12])
                else: out += bytes([(best_len - 2) << 4 | dd >> 8, dd & 0xff])
                for k in range(best_len): add(i + k)
                i += best_len
            else:
                flags |= 0x80 >> bit; out.append(data[i]); add(i); i += 1
        out[fp] = flags
    return bytes(out)


def glslc_output(stages):
    """A GLSLC compiler output of one stage (control section then code area) or several (by its section headers)"""
    if len(stages) == 1:
        c, k = stages[0]
        h = bytearray(0x200)
        h[0:4] = GLSLC
        size = 0x200 + len(c) + len(k)
        struct.pack_into('<I', h, 0x44, size); struct.pack_into('<I', h, 0x4c, 0)
        return bytes(h) + c + k
    n = len(stages)
    first, stride = 0x90, 0x90
    head = first + stride * n
    h = bytearray((head + 0xff) & ~0xff)
    h[0:4] = GLSLC
    body, secs = b'', []
    for c, k in stages:
        body = pad(body, 0x100); soff = len(h) + len(body)
        sec = pad(c, 0x10) + k
        secs.append((len(sec), soff, 0, len(pad(c, 0x10)), len(k), len(c)))
        body += sec
    size = len(h) + len(body)
    struct.pack_into('<I', h, 0x44, size); struct.pack_into('<I', h, 0x4c, n)
    for i, (ssz, soff, typ, ko, kn, cn) in enumerate(secs):
        s = first + stride * i
        struct.pack_into('<3I', h, s, ssz, soff, typ); struct.pack_into('<4I', h, s + 0x30, 0, ko, kn, cn)
    return bytes(h) + body


class Img:
    """A module image under construction: a page with MOD0 and the dynamic section, then the blobs"""
    def __init__(self):
        self.d = bytearray(0x1000)
        struct.pack_into('<I', self.d, 4, 0x10)
        self.d[0x10:0x14] = b'MOD0'
        self.dyn, self.rela, self.relr = 0x100, [], []
        struct.pack_into('<i', self.d, 0x14, self.dyn - 0x10)
        self.syms = []   # (name, defined)
        self.code = []   # instruction words appended
    def put(self, blob, align=0x100):
        self.d = bytearray(pad(bytes(self.d), align)); at = len(self.d); self.d += blob; return at
    def nso(self, path, extra_dyn=()):
        tags = []
        if self.rela:
            t = self.put(b''.join(struct.pack('<QQq', s, 0x403, v) for s, v in self.rela), 8)
            tags += [(7, t), (8, 24 * len(self.rela)), (9, 24)]
        if self.relr:
            t = self.put(b''.join(struct.pack('<Q', e) for e in self.relr), 8)
            tags += [(36, t), (35, 8 * len(self.relr))]
        if self.syms:
            strtab = b'\0' + b''.join(n.encode() + b'\0' for n, _ in self.syms)
            st = self.put(strtab, 8)
            sym = bytes(24); o = 1
            for n, defined in self.syms:
                sym += struct.pack('<IBBHQQ', o, 0x12, 0, 1 if defined else 0, 0, 0); o += len(n) + 1
            sy = self.put(sym, 8)
            tags += [(5, st), (6, sy), (10, len(strtab)), (11, 24)]
        tags += list(extra_dyn)
        for i, (t, v) in enumerate(tags + [(0, 0)]): struct.pack_into('<qQ', self.d, self.dyn + 16 * i, t, v)
        text = pad(bytes(self.d), 0x1000)
        segs = [(0, text), (len(text), bytes(0x1000)), (len(text) + 0x1000, bytes(0x1000))]
        hdr = bytearray(0x100); hdr[:4] = b'NSO0'
        at = 0x100; body = b''
        for k, (va, data) in enumerate(segs):
            struct.pack_into('<III', hdr, 0x10 + 0x10 * k, at, va, len(data)); struct.pack_into('<I', hdr, 0x60 + 4 * k, len(data))
            body += data; at += len(data)
        hdr[0x40:0x60] = R.randbytes(0x20)
        write(path, bytes(hdr) + body)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f: f.write(data)


def case(out, name, game, versions, dlc=()):
    write(os.path.join(out, name, 'case.json'), json.dumps({'game': game, 'versions': versions, 'dlc': list(dlc)}).encode())


# ---------------------------------------------------------------------------------------------------------------- cases

def variations(n, stages=('vs', 'ps'), share=None, **kw):
    out = []
    for _ in range(n):
        var = {st: shader(st, **kw) for st in stages}
        if share: var.update(share)
        out.append(var)
    return out


def botw(out, name='botw', image=False):
    """Breath of the Wild's forms: Yaz0 BNSH files, packs (SARC) with Yaz0 SARCs holding agl's BAHS and BNSH, effects with several BNSH, layouts, main with BNSH"""
    root = os.path.join(out, name)
    rom = os.path.join(root, 'rom')
    shared_vs = shader('vs')
    tvars = variations(3, share={'vs': shared_vs})
    write(f'{rom}/Shader/terrain.Tex1.sbfsha', yaz0(bnsh(tvars)))
    write(f'{rom}/Shader/model.sbfsha', yaz0(bnsh(variations(4) + [None] + variations(2, stages=('vs', 'gs', 'ps')))))
    write(f'{rom}/Shader/copy.Product.sbfsha', yaz0(bnsh(tvars[:1])))   # the same shaders again: keys found twice
    cs = [shader('cs', tg=(R.choice([8, 16, 32, 64]), R.choice([1, 2, 4]), 1), shared=R.choice([0, 100, 1024, 4097])) for _ in range(3)]
    write(f'{rom}/Shader/compute.sbfsha', yaz0(bnsh([{'cs': c} for c in cs])))
    effect = bytearray(rnd(0x2000))
    for v in range(3):
        effect = bytearray(pad(bytes(effect), 0x1000)); effect += bnsh(variations(R.randrange(1, 4))); effect += rnd(R.randrange(0x10, 0x400))
    effect += b'BNSH\0\0\0\0' + rnd(0x10)   # a BNSH magic near the end, no header
    write(f'{rom}/Effect/AncientBall.sesetlist', bytes(effect))
    write(f'{rom}/Effect/Plain.sesetlist', rnd(0x3000))
    agl = bahs([[(shader('vs'), shader('ps')), (shader('vs'), shader('ps'))], [(shader('vs'), shader('ps'))], [(shader('vs'), None)]],
               loose=[(3, shader('cs', tg=(16, 16, 1), shared=0x200))])
    gsys = bahs([[(shader('vs'), shader('ps')) for _ in range(3)]])
    lay = sarc([('Layout/Common.bnsh', bnsh(variations(2))), ('timg/a.bflim', rnd(0x800))])
    inner = sarc([('System/agl_resource.sharcb', agl), ('System/gsys_resource.sharcb', yaz0(gsys)), (None, rnd(0x100))])
    write(f'{rom}/Pack/Bootup_Graphics.pack', sarc([('System/Resource.ssarc', yaz0(inner)), ('Layout/Common.sblarc', yaz0(lay)),
                                                    ('Actor/x.bxml', rnd(0x200)), ('Terrain/MaterialAlb.sbfsha', yaz0(bnsh(variations(2))))]))
    write(f'{rom}/Pack/TitleBG.pack', sarc([('Effect/Frog.sesetlist', bytes(pad(rnd(0x1800), 0x1000)) + bnsh(variations(2))),
                                            ('Terrain/System/x.sbfsha', yaz0(bnsh(tvars)))]))
    write(f'{rom}/Layout/Common.sblarc', yaz0(sarc([('blyt/a.bflyt', rnd(0x300)), ('bnsh/ui.bnsh', bnsh(variations(1)))])))
    write(f'{rom}/Font/Noto Sans.bffnt', rnd(0x1000))   # a space in the name
    write(f'{rom}/Movie/empty.mp4', b'')
    write(f'{rom}/Map/MainField/A-1.smubin', yaz0(rnd(0x4000) * 2))
    m = Img()
    m.put(rnd(0x3000))
    for _ in range(2): m.put(bnsh(variations(3)), 0x1000); m.put(rnd(0x200))
    m.put(bnsh(tvars[:1]), 0x1000)   # a key main shares with the RomFS
    m.nso(f'{root}/main')
    if image:
        subprocess.run([sys.executable, f'{NXNATIVE}/dm/tools/romfs_image.py', rom, f'{root}/RomFS.img'], check=True, capture_output=True)
        shutil.rmtree(rom)
        case(out, name, '01007EF00011E000', [['RomFS.img', 'main']])
    else:
        case(out, name, '01007EF00011E000', [['rom', 'main']])
    return tvars


def versions(out):
    """An update beside the base game, and DLC: the union, later versions' names prefixed"""
    root = os.path.join(out, 'versions')
    base = variations(2)
    for v, extra in (('v0', 1), ('v327680', 2)):
        write(f'{root}/{v}/RomFS/Shader/a.sbfsha', yaz0(bnsh(base + variations(extra))))
        write(f'{root}/{v}/RomFS/Effect/b.sesetlist', bnsh(variations(1)))
        m = Img(); m.put(bnsh(variations(1)), 0x1000); m.nso(f'{root}/{v}/ExeFS/main')
    write(f'{root}/dlc/0100/RomFS/Shader/dlc.sbfsha', yaz0(bnsh(variations(2))))
    write(f'{root}/dlc2/Effect/e.sesetlist', bnsh(base))
    case(out, 'versions', '01007EF00011E000', [['v0/RomFS', 'v0/ExeFS/main'], ['v327680/RomFS/', 'v327680/ExeFS/main']],
         dlc=['dlc/0100/RomFS', 'dlc2/'])


def damaged(out):
    """Files that do not read: left out with a red line each, and the split goes on"""
    root = os.path.join(out, 'damaged')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/Shader/good.sbfsha', bnsh(variations(2)))
    y = yaz0(bnsh(variations(1)))
    write(f'{rom}/Shader/cut.sbfsha', y[:len(y) // 2])   # a Yaz0 stream that ends early
    s = bytearray(sarc([('a', bnsh(variations(1))), ('b', rnd(0x100))]))
    struct.pack_into('<I', s, 0x14 + 12 + 16 + 12, 0x7fffffff)   # member 1 ends past the data
    write(f'{rom}/Pack/bad.pack', bytes(s))
    b = bytearray(bnsh(variations(2)))
    struct.pack_into('<I', b, 0x7c, 50)   # more variations than the table holds
    write(f'{rom}/Effect/bad.sesetlist', bytes(b))
    vs, ps = shader('vs'), shader('ps')
    mismatch = bytearray(vs[0]); struct.pack_into('<I', mismatch, 0x704, len(vs[1]) + 16)
    write(f'{rom}/Effect/mismatch.sesetlist', bnsh([{'vs': (bytes(mismatch), vs[1]), 'ps': ps}]))
    # one key, two different control sections (their own length past 0x880: the bytes after the key differ)
    k = code('vs'); c1 = ctl(len(k), own=0x900); c2 = c1 + b'\x01' * 0x10
    write(f'{rom}/Effect/conflict.sesetlist', bnsh([{'vs': (c1, k)}, {'vs': (c2, k)}]))
    sa = bytearray(sarc([('x', bnsh(variations(1)))]))
    write(f'{rom}/Pack/deep.pack', yaz0(sarc([('n', bytes(sa))])))
    write(f'{rom}/Effect/short.bin', b'BNSH\0\0\0\0' + bytes(0x18) + b'grsc')
    m = Img(); m.put(bnsh(variations(1)), 0x1000); m.nso(f'{root}/main')
    case(out, 'damaged', '01007EF00011E000', [['rom', 'main']])


def formats(out):
    """The other forms the generic scan reads: gzip, Gust's zlib chunks, loose GLSLC outputs, MSHD, NDSR / NDVE, sead's primitive drawer, NVN2 BNSH with mesh shaders,
    PlatinumGames' objects and DAT archives, Needle, Sumo, Star Fox, MonoGame, Phyre; and in main: GLSLC outputs, YEBIS's zlib stream, loose control sections"""
    root = os.path.join(out, 'formats')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/data/shaders.arc.gz', gzip.compress(bnsh(variations(2)), mtime=0))
    write(f'{rom}/data/two.gz', gzip.compress(bnsh(variations(1)), mtime=0) + bytes(16) + gzip.compress(bnsh(variations(1)), mtime=0))
    raw = bnsh(variations(1))
    chunks = b''
    for i in range(0, len(raw), 0x4000): z = zlib.compress(raw[i:i + 0x4000]); chunks += struct.pack('<I', len(z)) + z
    write(f'{rom}/data/pe_pack.elixir.gz', chunks + bytes(4))
    outs = [glslc_output([shader(st)]) for st in ('vs', 'ps', 'cs')] + [glslc_output([shader('vs'), shader('ps')])]
    write(f'{rom}/data/shaders.tec', b''.join(pad(o, 0x100) for o in outs))
    # MSHD: control section, a little gap, code area; in pairs
    mshd = bytearray(b'MSHD' + rnd(0x3c))
    for _ in range(2):
        for st in ('vs', 'ps'):
            c, k = shader(st)
            mshd += c + bytes(0x40) + k
    write(f'{rom}/system/shd/a.bshdat', bytes(mshd))
    # NDSR: records {control, length << 4 | type, code, ...}
    nd = bytearray(b'NDSR' + bytes(0xc)); recs = []
    body = bytearray()
    for st in ('vs', 'ps', 'cs'):
        k = code(st if st != 'cs' else 'vs', nvn2=True)
        c = ctl(len(k), nvn2=True, stage=st, tg=(8, 8, 1))
        recs.append((c, k))
    base = 0x10 + 16 * (len(recs) + 1)
    blob = bytearray(); table = []
    for c, k in recs:
        co = base + len(blob); blob += c; blob = bytearray(pad(bytes(blob), 0x100))
        ko = base + len(blob); blob += k; blob = bytearray(pad(bytes(blob), 0x100))
        table.append(struct.pack('<4I', co, len(c) << 4, ko, len(k) // 16 << 8))
    nd += b''.join(table) + bytes(16) + blob
    write(f'{rom}/Lib/agl/agl_ndsr.pck', bytes(nd))
    # NVN2 BNSH: vertex, pixel, and a mesh shader
    mk = code('ms', nvn2=True); mc = ctl(len(mk), nvn2=True, stage='ms')
    pk_ = code('ps', nvn2=True); pc = ctl(len(pk_), nvn2=True)
    write(f'{rom}/Shader/Foliage.bfsha', bnsh([{'ps': (pc, pk_), 'ms': (mc, mk)}, {'vs': shader('vs', nvn2=True), 'ps': shader('ps', nvn2=True)}]))
    # sead's primitive drawer
    vk, pk2 = code('vs', nvn2=True), code('ps', nvn2=True)
    vc, pc2 = ctl(len(vk), nvn2=True), ctl(len(pk2), nvn2=True)
    sp = bytearray(0x10); o = 0x10; parts = []
    for blob_ in (vc, pc2, vk, pk2): parts.append(o); sp += blob_; o += len(blob_)
    struct.pack_into('<4I', sp, 0, *parts)
    write(f'{rom}/Lib/sead/primitive_drawer_nvn2_shader.bin', bytes(sp))
    # PlatinumGames: a DAT with shader objects
    def pgso(st):
        c, k = shader(st)
        return pad({'vs': b'VSO\0', 'ps': b'PSO\0', 'cs': b'CSO\0'}[st] + bytes([0, 0, 1, 15]) + struct.pack('<4I', len(c), 0x18, len(k), 0x18 + len(c)), 1) + c + k
    objs = [pgso('vs'), pgso('ps'), pgso('cs')]
    n = len(objs); ot, et, nt = 0x20, 0x20 + 4 * n, 0x20 + 8 * n
    w = 8; st_ = nt + 4 + w * n
    head = bytearray(st_ + 4 * n); data_at = (len(head) + 0xff) & ~0xff
    head += bytes(data_at - len(head)); offs = []
    for o_ in objs: offs.append(len(head)); head += pad(o_, 0x10)
    struct.pack_into('<4s5I', head, 0, b'DAT\0', n, ot, et, nt, st_)
    for i, o_ in enumerate(objs):
        struct.pack_into('<I', head, ot + 4 * i, offs[i]); head[et + 4 * i:et + 4 * i + 4] = b'pso\0'
        struct.pack_into('<I', head, st_ + 4 * i, len(o_))
    struct.pack_into('<I', head, nt, w)
    for i in range(n): head[nt + 4 + w * i:nt + 4 + w * (i + 1)] = pad(f's{i}.pso'.encode(), w)[:w]
    write(f'{rom}/shader.dat', bytes(head))
    # Needle: HNSHV002, its length big-endian, NVN sections with 0x90-byte descriptors
    ne = bytearray(b'HNSHV002' + bytes(8))
    for st in ('vs', 'ps'):
        c, k = shader(st, length=0x850, own=0x850)
        s = len(ne); desc = bytearray(0x90); struct.pack_into('<4I', desc, 0x30, 0x90, 0x90 + len(c), len(k), len(c))
        ne += desc + c + k
    struct.pack_into('>I', ne, 8, len(ne))
    write(f'{rom}/data/post.gsc', bytes(rnd(0x40)) + bytes(ne) + rnd(0x20))
    # Sumo NVNsh records
    sm = bytearray(rnd(0x20))
    for stn, st in ((0, 'vs'), (4, 'ps')):
        c, k = shader(st)
        h = len(sm); rec = bytearray(0x40); rec[0:6] = b'NVNsh\0'; struct.pack_into('<H', rec, 6, 1)
        size = 0x40 + len(c) + len(k); struct.pack_into('<I', rec, 8, size); struct.pack_into('<I', rec, 28, stn)
        struct.pack_into('<4I', rec, 32, 0x40, 0x40 + len(c), len(k), len(c))
        sm += rec + c + k
    write(f'{rom}/shaders/sumo.bin', bytes(sm))
    # Star Fox's shader binary: GLSLC magic, code length, control length, 0, hash, code, NVN2 control
    k = code('ps', nvn2=True); c = ctl(len(k), nvn2=True)
    write(f'{rom}/data/vs.bin', GLSLC + struct.pack('<IIIQ', len(k), len(c), 0, 0) + k + c)
    # MonoGame effect, version 8, in an uncompressed XNB
    def mgfx():
        e = bytearray(b'MGFX' + bytes([8, 0]) + struct.pack('<I', 0x1234))
        e += bytes([0])   # no constant buffers
        sh = []
        for vs in (1, 0):
            c, k = shader('vs' if vs else 'ps')
            bc = struct.pack('<I', len(c)) + c + struct.pack('<I', len(k)) + k
            s_ = bytes([vs]) + struct.pack('<I', len(bc)) + bc + bytes([0]) + bytes([0]) + bytes([0])
            sh.append(s_)
        e += bytes([len(sh)]) + b''.join(sh)
        e += bytes([0])   # parameters
        e += bytes([1]) + bytes([1]) + b'T' + bytes([0]) + bytes([1]) + bytes([1]) + b'P' + bytes([0]) + bytes([0, 1]) + bytes([0, 0, 0])
        return bytes(e)
    write(f'{rom}/Content/Effects/x.xnb', b'XNBw\x05\x00' + struct.pack('<I', 0) + b'EffectReader' + mgfx())
    # Phyre: code area right before its control section
    c, k = shader('ps')
    write(f'{rom}/shaders/a.ps.bin', rnd(0x10) + k + c)
    # a .data with its .dict beside: read through the index, not scanned
    write(f'{rom}/levels/a.data', bnsh(variations(1)))
    write(f'{rom}/levels/a.dict', rnd(0x40))
    # 'PAK' that is no Heaps archive: scanned as a file
    write(f'{rom}/a.pak', b'PAK\x05' + rnd(0x40) + bnsh(variations(1)))
    m = Img()
    m.put(rnd(0x800))
    for o_ in outs[:2]: m.put(o_)
    cfx = [shader(st, length=0x850, own=0x850) for st in ('vs', 'ps', 'cs')]
    tf = b'.tfxo file\0' + b'1234 3 hlsl TexUtil_NVN_NX tech_a,vs_a,ps_a\0' + b''.join(c_ + k_ for c_, k_ in cfx)
    m.put(rnd(0x10) + zlib.compress(tf, 9) + rnd(0x10))
    # loose control sections: one with a data record, one the code builds the address of, one with no code area
    c1, k1 = shader('ps'); at_k1 = m.put(k1); at_c1 = m.put(c1)
    rec = m.put(bytes(0x20), 0x10); struct.pack_into('<I', m.d, rec + 16, len(k1)); m.rela += [(rec, at_k1), (rec + 8, at_c1)]
    # the code before the shader (shader_split.py reads adrp's page offset as unsigned: only a forward one counts)
    code_at = m.put(bytes(0x10), 0x1000)
    c2, k2 = shader('vs'); at_c2 = m.put(c2, 0x1000)
    at_k2 = m.put(k2, 0x10)
    assert at_k2 == (at_c2 + len(c2) + 15) & ~15
    insns = b''
    for i, t in enumerate((at_c2, at_k2)):
        imm = ((t & ~0xfff) - ((code_at + len(insns)) & ~0xfff)) >> 12
        insns += struct.pack('<I', 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | 3)   # adrp x3
        insns += struct.pack('<I', 0x91000000 | (t & 0xfff) << 10 | 3 << 5 | 3)   # add x3, x3, #lo
    m.d[code_at:code_at + len(insns)] = insns
    c3, _ = shader('ps'); m.put(c3)
    m.nso(f'{root}/main')
    case(out, 'formats', '01007EF00011E000', [['rom', 'main']])


def unported(out):
    """Files of other engines: a zstd file and a Unity file are left out (red); a zip archive stops the split"""
    root = os.path.join(out, 'unported')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/Shader/a.sbfsha', bnsh(variations(1)))
    write(f'{rom}/Shader/b.zs', bytes.fromhex('28b52ffd') + rnd(0x40))
    write(f'{rom}/Data/data.unity3d', b'UnityFS\0' + rnd(0x40))
    m = Img(); m.nso(f'{root}/main')
    case(out, 'unported', '01007EF00011E000', [['rom', 'main']])
    root = os.path.join(out, 'unported_zip')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/Shader/a.sbfsha', bnsh(variations(1)))
    write(f'{rom}/game.zip', b'PK\x03\x04' + rnd(0x40))
    m = Img(); m.nso(f'{root}/main')
    case(out, 'unported_zip', '01007EF00011E000', [['rom', 'main']])


def keyskip(out):
    """A profile with shader_key_skip (Batman: Arkham Knight, 0x770+0x78): sections that differ only there are one key"""
    root = os.path.join(out, 'keyskip')
    rom = os.path.join(root, 'rom')
    k = code('ps'); c = bytearray(ctl(len(k)))
    c2 = bytearray(c); c2[0x780:0x790] = rnd(0x10)
    write(f'{rom}/a.bin', bnsh([{'ps': (bytes(c), k), 'vs': shader('vs')}]))
    write(f'{rom}/b.bin', bnsh([{'ps': (bytes(c2), k), 'vs': shader('vs')}]))
    m = Img(); m.nso(f'{root}/main')
    case(out, 'keyskip', '0100ACD0163D0000', [['rom', 'main']])


def single(out):
    """Single-stage programs (GLSLC outputs in main): every vertex shader paired with a cover of the pixel shaders' inputs"""
    root = os.path.join(out, 'single')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/x.bin', bnsh(variations(1)))
    m = Img()
    for st, ins in (('vs', None), ('vs', None), ('ps', [0, 1, 2]), ('ps', [2, 3]), ('ps', [5]), ('ps', [0, 5])):
        m.put(glslc_output([shader(st, inputs=ins) if st == 'ps' else shader(st)]))
    m.nso(f'{root}/main')
    case(out, 'single', '01007EF00011E000', [['rom', 'main']])


def glsl_check(out):
    """main holds GLSL sources and imports glslcCompile, which sdk defines, and no capture: a red line; a capture folder beside the RomFS is read"""
    root = os.path.join(out, 'glsl')
    rom = os.path.join(root, 'rom')
    write(f'{rom}/x.bin', bnsh(variations(1)))
    m = Img(); m.put(b'#version 450\nlayout(location=0) out vec4 o;\nvoid  main ( ) { o = vec4(1); }\0'); m.syms = [('glslcCompile', False), ('nnMain', True)]
    m.nso(f'{root}/ExeFS/main')
    s = Img(); s.syms = [('glslcCompile', True)]; s.nso(f'{root}/ExeFS/sdk')
    case(out, 'glsl', '01007EF00011E000', [['rom', 'ExeFS/main']])
    root = os.path.join(out, 'capture')
    write(f'{root}/rom/x.bin', bnsh(variations(1)))
    write(f'{root}/glslc_capture/0001.glslc', glslc_output([shader('vs')]))
    write(f'{root}/glslc_capture/0002.glslc', glslc_output([shader('ps')]))
    write(f'{root}/glslc_capture/._0002.glslc', b'')
    m = Img(); m.nso(f'{root}/main')
    case(out, 'capture', '01007EF00011E000', [['rom', 'main']])


def conflict(out):
    """One key in two files with different control sections (their own length past 0x880, the bytes after the key's differ): the split stops"""
    root = os.path.join(out, 'conflict')
    k = code('vs'); c1 = ctl(len(k), own=0x900); c2 = c1 + b'\x01' * 0x10
    write(f'{root}/rom/a.bin', bnsh([{'vs': (c1, k)}]))
    write(f'{root}/rom/b.bin', bnsh([{'vs': (c2, k)}]))
    m = Img(); m.nso(f'{root}/main')
    case(out, 'conflict', '01007EF00011E000', [['rom', 'main']])


def main():
    global LOW, NXNATIVE
    args = sys.argv[1:]
    if args and args[0] == '--low': LOW = True; args = args[1:]
    out = args[0]
    if len(args) > 1: NXNATIVE = args[1]
    shutil.rmtree(out, ignore_errors=True)
    botw(out)
    botw(out, 'botw_image', image=True)
    versions(out)
    damaged(out)
    formats(out)
    unported(out)
    keyskip(out)
    single(out)
    glsl_check(out)
    conflict(out)


if __name__ == '__main__':
    main()
