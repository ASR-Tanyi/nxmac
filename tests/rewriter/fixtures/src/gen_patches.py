#!/usr/bin/env python3
"""gen_patches.py <nxnative tree> <profile> <registered module A .nso> : a test profile in the tree: build rows, registered CUTS / OBS / CAPABILITIES of A's build, and
MATCH_CUTS made from A's functions with nso_match's own recipe (one function, several functions, a rule that matches nothing, and two implementations of one patch)"""
import os, struct, sys, pprint
tree, profile, a_path = sys.argv[1:4]
sys.path.insert(0, os.path.join(tree, 'dm/tools'))
import nso, nso_match
from capstone import *
m = nso.read(a_path); img = bytes(m.image); bid = m.build_id[:16].hex()
im = nso_match.Image(m.image, m.text_end)
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
NOP = '1f2003d5'
def site(start, end):
    """the first word of the function that is plain arithmetic without x18 or pc: (offset, old)"""
    for off in range(start + 8, end - 8, 4):
        w = img[off:off + 4]
        ins = next(md.disasm(w, off), None)
        if ins and ins.mnemonic in ('mov', 'add', 'sub', 'orr', 'and') and '18' not in ins.op_str and 'sp' not in ins.op_str and 'x29' not in ins.op_str: return off, w.hex()
    return None
fns = [(s, e) for s, e in im.functions.items() if 64 <= e - s <= 4096]
good = []
for s, e in fns:
    try: im.fingerprint(s)
    except nso_match.MatchError: continue
    st = site(s, e)
    if st and sum(1 for s2, e2 in fns if e2 - s2 == e - s) < 40: good.append((s, e, st))
    if len(good) >= 60: break
print(len(good), 'functions with a cut site')
rules = []
unique, ambiguous = [], []
for k, (s, e, (off, old)) in enumerate(good):
    try: im.recipe(off, old, NOP, 'x'); unique.append(k)
    except nso_match.MatchError: ambiguous.append(k)
print(len(unique), 'unique,', len(ambiguous), 'ambiguous')
def recipe(k, name):
    s, e, (off, old) = good[k]
    return im.recipe(off, old, NOP, name)
u = unique
rules.append(recipe(u[3], 'one function'))
rules.append(im.patch_recipe([(good[k][2][0], good[k][2][1], NOP, f'part {k}') for k in (u[10], u[11], u[12])], 'three functions'))
bad = recipe(u[20], 'no such function'); bad['fingerprints'] = ['0' * 64]; rules.append(bad)
alt1 = recipe(u[25], 'two implementations'); alt0 = dict(alt1, fingerprints=['1' * 64], size=alt1['size'] + 4); rules += [alt0, alt1]
part = im.patch_recipe([(good[k][2][0], good[k][2][1], NOP, f'half {k}') for k in (u[26], u[27])], "half of it: the patch's second function is another"); part['functions'][1]['fingerprints'] = ['2' * 64]; rules.append(part)
if ambiguous:
    s, e, (off, old) = good[ambiguous[0]]
    rules.append(dict(name='several alike', size=e - s, fingerprints=[im.fingerprint(s)], cuts=[(off - s, old, NOP, 'ambiguous')]))
cuts = [(good[k][2][0], good[k][2][1], NOP, f'registered cut {k}: its instruction → nop') for k in (u[5], u[6], u[7])]
kobs = [good[k][2][0] for k in (u[30], u[31])]   # plain arithmetic again: a gate goes on no rewrite site
obs = [(o, 30 + i, f'{struct.unpack_from("<I", img, o)[0]:08x}', f'test gate {i}') for i, o in enumerate(kobs)]
d = os.path.join(tree, 'dm/games', profile); os.makedirs(d, exist_ok=True)
with open(os.path.join(d, 'game.tsv'), 'w') as f:
    f.write(f'name\tTest\nbuild\tv0\t1.0.0\tmain={bid}\tsdk=00000000000000000000000000000000\nbuild\tv65536\t1.0.1\tmain=ffffffffffffffffffffffffffffffff\nmodules\tmain\n')
with open(os.path.join(d, 'patches.py'), 'w') as f:
    f.write('# a test profile (nxmac tests/rewriter/fixtures/src/gen_patches.py)\n')
    f.write(f'CUTS = {{{bid!r}: {pprint.pformat(cuts, width=200)}}}\n')
    f.write(f'OBS = {{{bid!r}: {pprint.pformat(obs, width=200)}}}\n')
    f.write(f'CAPABILITIES = {{{bid!r}: ("TEST_CAPABILITY", "ANOTHER_ONE")}}\n')
    f.write(f'MATCH_CUTS = {{"main": {pprint.pformat(rules, width=200)}}}\n')
print('profile', d, 'build', bid)
