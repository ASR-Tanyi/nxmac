#!/usr/bin/env python3
"""expect.py <fixtures dir> <nxnative checkout>: nxnative's shader_split.py on every case of <fixtures>/cases (gen.py made them), one file at a time
(J=1). Writes cases.tsv (what each case splits), expected.tsv (the SHA-256 of every file it writes) and lines.tsv (the lines it prints, but its progress)."""
import hashlib, json, os, re, subprocess, sys, tempfile

TABLES = ['shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv']
# Where nxmac does not do what shader_split.py does, by design: another engine's file (zstd, Unity) is left out with its own red line, and
# a zip archive, which shader_split.py takes apart (or scans whole when it is not one), stops the split
OWN = {'unported': ('own-lines', '-'), 'unported_zip': ('stop', 'a zip archive: its reader is not ported to nxmac yet')}


def main():
    fixtures, nxnative = map(os.path.abspath, sys.argv[1:3])
    cases = os.path.join(fixtures, 'cases')
    commit = subprocess.run(['git', '-C', nxnative, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown commit'
    rows, expected, lines = [], [], []
    for name in sorted(os.listdir(cases)):
        c = json.load(open(os.path.join(cases, name, 'case.json')))
        want, why = OWN.get(name, ('ok', '-'))
        rows.append('\t'.join([name, c['game'], ';'.join(f'{r}|{m}' for r, m in c['versions']), ';'.join(c['dlc']) or '-', want, why]))
        base = os.path.join(cases, name)
        with tempfile.TemporaryDirectory() as out:
            args = [sys.executable, os.path.join(nxnative, 'dm/gfx/tools/shader_split.py'), ','.join(r for r, _ in c['versions']),
                    ','.join(m for _, m in c['versions']), os.path.join(out, 'pkg')] + [os.path.join(out, t) for t in TABLES]
            if c['dlc']: args.append(','.join(c['dlc']))
            p = subprocess.run(args, cwd=base, capture_output=True, text=True, env=dict(os.environ, DM_GAME=c['game'], J='1'))
            if p.returncode:   # a stop: its red line is what nxmac's must hold
                red = p.stderr.strip().splitlines()[-1] if p.stderr.strip() else ''
                if name in OWN or not red.startswith('[RED] '): sys.exit(f'{name}: shader_split.py stopped:\n{p.stdout}{p.stderr}')
                rows[-1] = '\t'.join(rows[-1].split('\t')[:4] + ['stop', red.removeprefix('[RED] ')])
                lines += [f'{name}\t{l}' for l in p.stdout.splitlines() if not re.fullmatch(r'progress: \d+/\d+', l)]
                continue
            files = [(t, os.path.join(out, t)) for t in TABLES] + [(f'pkg/{f}', os.path.join(out, 'pkg', f)) for f in sorted(os.listdir(os.path.join(out, 'pkg')))]
            for f, path in files: expected.append(f'{name}\t{f}\t{hashlib.sha256(open(path, "rb").read()).hexdigest()}')
            lines += [f'{name}\t{l}' for l in p.stdout.splitlines() if not re.fullmatch(r'progress: \d+/\d+', l)]
    head = f'# Written by src/make_fixtures.sh from nxnative\'s shader_split.py ({commit}): do not edit.'
    with open(os.path.join(fixtures, 'cases.tsv'), 'w') as f:
        f.write('# The golden cases of tests/shader/golden_test.cpp: case, profile, versions (RomFS|main;...), DLC (;-separated, - for none), then ok, own-lines\n'
                '# (nxmac prints its own red lines: another engine\'s files) or stop (nxmac stops where shader_split.py goes on), and the text the stop holds.\n'
                + head + '\n' + '\n'.join(rows) + '\n')
    with open(os.path.join(fixtures, 'expected.tsv'), 'w') as f:
        f.write('# SHA-256 of each file shader_split.py writes for the cases of cases.tsv: case, file (pkg/ the package), hash.\n' + head + '\n' + '\n'.join(expected) + '\n')
    with open(os.path.join(fixtures, 'lines.tsv'), 'w', encoding='utf-8') as f:
        f.write('# The lines shader_split.py prints for each case, in its order (one file at a time), its progress left out: case, line.\n' + head + '\n' + '\n'.join(lines) + '\n')
    # the profiles the cases split with, as tools/export_shader_profiles.py writes them: the test does not follow the built-in table's changes
    games = sorted({r.split('\t')[1] for r in rows})
    exporter = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools', 'export_shader_profiles.py')
    with tempfile.TemporaryDirectory() as t:
        subprocess.run([sys.executable, exporter, '--nxnative', nxnative, '-o', os.path.join(t, 'p.tsv')], check=True, capture_output=True)
        keep = [l for l in open(os.path.join(t, 'p.tsv'), encoding='utf-8').read().splitlines()
                if l.startswith('#') or l.startswith('source\t') or (len(l.split('\t')) > 1 and l.split('\t')[1] in games)]
    with open(os.path.join(fixtures, 'profiles_fixture.tsv'), 'w', encoding='utf-8') as f: f.write('\n'.join(keep) + '\n')
    print(f'{len(rows)} cases, {len(expected)} files, {len(games)} profiles')


if __name__ == '__main__':
    main()
