#!/usr/bin/env python3
"""The rewriter's patch tables, from nxnative's patch files to the text file nxmac carries (src/rewriter/patches.tsv).

nxnative keeps what it knows about each build in Python files: dm/tools/sdk_patches.py and engine_patches.py, and
dm/games/<id>/patches.py. This script loads them exactly as dm/tools/patch_nso.py does (the same files in the same
order, a build ID claimed twice stops it, a derived profile's rules give way to the repository's) and writes their tables
out, each value as the file has it. What the files hold as code (FRAME_STEP and STUBS functions, the stub and find of a
frame step placed by function match) cannot be exported: the build is listed, and its code is ported by hand in
src/rewriter/games. The rows are described in src/rewriter/include/nxmac/rewriter/patches.h.

    tools/export_patches.py                         nxnative beside nxmac -> src/rewriter/patches.tsv
    tools/export_patches.py --nxnative <dir> -o <file> [--games-dir <dir>]...

--games-dir adds profiles outside the repository (nxnative's DM_GAMES_DIR). Run it again whenever nxnative's patch
files change, and build: the tables are compiled into the rewriter. It needs only Python 3.12 and nxnative's sources."""
import argparse, glob, importlib.util, os, subprocess, sys

SHARED = ('sdk_patches.py', 'engine_patches.py')   # rules for every game's module of a kind: Nintendo's SDK, engine code
HERE = os.path.dirname(os.path.abspath(__file__))


def fail(why):
    sys.exit(f'export_patches: {why}')


def field(text, what):
    """A value as one field of a row: no tab or line break may be in it"""
    text = str(text)
    if '\t' in text or '\n' in text or '\r' in text: fail(f'{what} holds a tab or a line break: {text!r}')
    return text


def hex_bytes(text, what):
    """Bytes as the tables write them: lower-case hex digits and nothing else, so that the rewriter's files repeat them as nxnative's do"""
    try: same = bytes.fromhex(text).hex() == text
    except (ValueError, TypeError): same = False
    if not same or not text: fail(f'{what}: bytes not written as lower-case hex digits: {text!r}')
    return text


def load(tools, games, extra):
    """patch_nso.py's load_patches: (tables, where each build's table came from, the patch files)"""
    files = [os.path.join(tools, f) for f in SHARED] + sorted(glob.glob(os.path.join(glob.escape(games), '*', 'patches.py')))
    files += [p for d in extra for p in sorted(glob.glob(os.path.join(glob.escape(d), '*', 'patches.py')))]
    seen = {}
    for path in files: seen.setdefault(os.path.realpath(path), path)   # repeated roots and symlinks name the same file
    files = list(seen.values())
    tables = {k: {} for k in ('CUTS', 'OBS', 'FRAME_STEP', 'STUBS', 'MATCH_CUTS', 'MATCH_FRAME_STEP', 'SDK_MATCH_CUTS', 'CAPABILITIES')}
    where = {}
    for n, path in enumerate(files):
        spec = importlib.util.spec_from_file_location(f'patches_{n}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        shared = os.path.basename(path) in SHARED
        for kind, table in tables.items():
            if kind == 'SDK_MATCH_CUTS': continue
            if kind == 'MATCH_CUTS' and shared:
                for mod, rules in getattr(m, kind, {}).items(): tables['SDK_MATCH_CUTS'].setdefault(mod, []).extend(rules)
                continue
            if kind in ('MATCH_CUTS', 'MATCH_FRAME_STEP'):
                if shared: continue
                profile = os.path.basename(os.path.dirname(path))
                profile_dir = next((os.path.join(root, profile) for root in [games] + extra if os.path.isdir(os.path.join(root, profile))), None)
                if profile_dir and os.path.realpath(os.path.dirname(path)) != os.path.realpath(profile_dir): continue
                if getattr(m, kind, {}): table[profile] = getattr(m, kind)
                continue
            for bid, v in getattr(m, kind, {}).items():
                if bid in table: fail(f'{kind} of build {bid} in both {where[kind, bid]} and {path}')
                table[bid], where[kind, bid] = v, path
    return tables, where, files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--nxnative', default=os.path.join(HERE, '..', '..', 'nxnative'), help='the nxnative checkout (default: beside nxmac)')
    ap.add_argument('--games-dir', action='append', default=[], help='a folder of profiles outside the repository (DM_GAMES_DIR)')
    ap.add_argument('-o', '--out', default=os.path.join(HERE, '..', 'src', 'rewriter', 'patches.tsv'))
    args = ap.parse_args()
    tools, games = os.path.join(args.nxnative, 'dm', 'tools'), os.path.join(args.nxnative, 'dm', 'games')
    if not os.path.isfile(os.path.join(tools, 'patch_nso.py')): fail(f'no nxnative at {args.nxnative} (--nxnative <dir>)')
    sys.path.insert(0, tools)   # the patch files import nso_asm; the rules are checked with nso_match's own validate
    sys.dont_write_bytecode = True
    import nso_match
    tables, where, files = load(tools, games, args.games_dir)
    try: commit = subprocess.run(['git', '-C', args.nxnative, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError): commit = 'unknown commit'
    rows = ['# nxmac patch tables (src/rewriter/include/nxmac/rewriter/patches.h describes the rows). Generated by tools/export_patches.py from',
            '# nxnative\'s patch files: do not edit; run it again when they change.',
            f'source\tnxnative {commit}, {len(files)} patch files']

    def build_id(bid, kind):
        if len(bid) != 32 or any(c not in '0123456789abcdef' for c in bid): fail(f'{kind}: {bid!r} is no build ID')
        return bid

    def profile_of(kind, bid):
        path = where[kind, bid]
        return os.path.basename(path) if os.path.basename(path) in SHARED else os.path.basename(os.path.dirname(path))

    for bid, cuts in tables['CUTS'].items():
        rows.append(f'cuts\t{build_id(bid, "CUTS")}')
        for off, ob, nb, why in cuts:
            what = f'CUTS {bid} {off:#x}'
            if len(ob) != len(nb): fail(f'{what}: original and new bytes differ in length')
            rows.append(f'cut\t{bid}\t{off:#x}\t{hex_bytes(ob, what)}\t{hex_bytes(nb, what)}\t{field(why, what)}')
    for bid, gates in tables['OBS'].items():
        for off, number, word, why in gates:
            what = f'OBS {bid} {off:#x}'
            if len(word) != 8: fail(f'{what}: the instruction word is 8 hex digits: {word!r}')
            rows.append(f'obs\t{build_id(bid, "OBS")}\t{off:#x}\t{int(number)}\t{hex_bytes(word, what)}\t{field(why, what)}')
    for bid, names in tables['CAPABILITIES'].items():
        rows += [f'capability\t{build_id(bid, "CAPABILITIES")}\t{field(name, "capability")}' for name in names]
    for kind, row in (('FRAME_STEP', 'frame_step'), ('STUBS', 'stubs')):
        rows += [f'{row}\t{build_id(bid, kind)}\t{field(profile_of(kind, bid), kind)}' for bid in tables[kind]]

    def match_rows(scope, by_module):
        for module, rules in by_module.items():
            for rule in rules:
                nso_match.validate(rule)
                name = field(rule['name'], 'a function patch\'s name')
                functions = rule['functions'] if 'functions' in rule else [rule]
                rows.append(f'match\t{field(scope, "profile")}\t{field(module, "module")}\t{"several" if "functions" in rule else "one"}\t{name}')
                for fn in functions:
                    rows.append(f'function\t{fn["size"]}\t{",".join(fn["fingerprints"])}')
                    for delta, ob, nb, why in fn['cuts']:
                        what = f'function patch {name!r} +{delta:#x}'
                        rows.append(f'fcut\t{delta:#x}\t{hex_bytes(ob, what)}\t{hex_bytes(nb, what)}\t{field(why, what)}')
    for profile, by_module in tables['MATCH_CUTS'].items(): match_rows(profile, by_module)
    match_rows('*', tables['SDK_MATCH_CUTS'])
    for profile, by_module in tables['MATCH_FRAME_STEP'].items():
        for module, rules in by_module.items():
            for rule in rules: nso_match.validate_stub(rule)
            for name in dict.fromkeys(rule['name'] for rule in rules):
                rows.append(f'match_frame_step\t{field(profile, "profile")}\t{field(module, "module")}\t{field(name, "frame step")}')
    # The builds each repository profile was brought up on (its game.tsv build rows): patch_nso.py reads them to tell an
    # unregistered build what the registered ones had cut
    for path in sorted(glob.glob(os.path.join(glob.escape(games), '*', 'game.tsv'))):
        with open(path, encoding='utf-8') as f: lines = f.read().splitlines()
        for line in lines:
            c = [x.strip() for x in line.split('\t')]
            if c[0] == 'build' and len(c) > 3:
                rows.append('\t'.join(['build', os.path.basename(os.path.dirname(path)), field(c[1], 'build'), field(c[2], 'build')] + [x for x in c[3:] if '=' in x]))
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f: f.write('\n'.join(rows) + '\n')
    counts = {k: len(v) for k, v in tables.items()}
    print(f'{args.out}: {len(rows)} rows from {len(files)} patch files of nxnative {commit[:12]}: ' + ', '.join(f'{k} {n}' for k, n in counts.items()))
    code = sorted({(profile_of(k, b), b) for k in ('FRAME_STEP', 'STUBS') for b in tables[k]})
    print(f'{len(code)} builds have frame-step or stub code (src/rewriter/games holds the ported ones): ' + ', '.join(f'{p} {b[:8]}' for p, b in code))


if __name__ == '__main__':
    main()
