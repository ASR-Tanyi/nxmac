#!/usr/bin/env python3
"""The shader split's profile values, from nxnative's game profiles to the text file nxmac carries (src/shader/profiles.tsv).

nxnative's dm/gfx/tools/shader_split.py takes a few values from the game profile it runs for (dm/games/<id>/game.tsv):
shader_groups (which package each source goes to), shader_note (the shader map's header), shader_key_skip (control bytes
the keys hash as zeros) and modules (where a glslcCompile might be), and merges the profile's shader_facts_manual.tsv
over the facts it generates. This script reads them as dm/games/game.py and shader_split.py do (the same files, the
same search order: dm/games first, then each --games-dir, the first profile of a title ID wins) and writes them out;
the rows are described in src/shader/include/nxmac/shader/profiles.h.

    tools/export_shader_profiles.py                         nxnative beside nxmac -> src/shader/profiles.tsv
    tools/export_shader_profiles.py --nxnative <dir> -o <file> [--games-dir <dir>]...

--games-dir adds profiles outside the repository (nxnative's DM_GAMES_DIR). Run it again whenever a profile's shader
values or its shader_facts_manual.tsv change, and build: the table is compiled into nxmac. It needs only Python 3.12
and nxnative's sources."""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def fail(why):
    sys.exit(f'export_shader_profiles: {why}')


def field(text, what):
    """A value as one field of a row: no tab or line break may be in it"""
    text = str(text)
    if '\t' in text or '\n' in text or '\r' in text: fail(f'{what} holds a tab or a line break: {text!r}')
    return text


def json_text(value):
    """A JSON value as shader_split.py writes it into shader_facts.tsv (json.dumps with sorted keys and no spaces)"""
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def profile_rows(game_module, g):
    """The rows of one profile, or None when shader_split.py could not run for it (no shader_groups or shader_note:
    it stops on the missing key), which leaves the profile out"""
    gid = g.id
    try: groups, note = g.pairs('shader_groups'), g.get('shader_note')
    except KeyError: return None
    rows = [f'profile\t{field(gid, "title ID")}']
    for pair in groups:
        if len(pair) != 2: fail(f'{gid}: shader_groups word {pair[0]!r} has no "=": shader_split.py cannot use it')
    rows.append('\t'.join(['groups', gid] + [field(f'{k}={v}', f'{gid} shader_groups') for k, v in groups]))
    # get() joins a key's values with tabs: the note is the rest of the row, tabs and all
    if '\n' in note or '\r' in note: fail(f'{gid}: shader_note holds a line break')
    rows.append(f'note\t{gid}\t{note}')
    try: skip = tuple(int(x, 0) for x in g.get('shader_key_skip', '0+0').split('+'))
    except ValueError: fail(f'{gid}: shader_key_skip is not <offset>+<length>')
    if len(skip) != 2 or min(skip) < 0: fail(f'{gid}: shader_key_skip is not <offset>+<length>')
    rows.append(f'key_skip\t{gid}\t{skip[0]}\t{skip[1]}')
    rows.append('\t'.join(['modules', gid] + [field(m, f'{gid} modules') for m in g.words('modules', 'main')]))
    manual = g.table('shader_facts_manual.tsv')
    if os.path.exists(manual):
        facts = {}
        with open(manual) as f:   # as shader_split.py reads it
            for ln in f:
                if ln.strip() and not ln.startswith('#'):
                    k, v = ln.rstrip('\n').split('\t', 1)
                    value = json.loads(v)
                    if not isinstance(value, dict): fail(f'{manual}: the facts of {k} are not a JSON object')
                    facts[k] = {**facts.get(k, {}), **value}
        for k, members in facts.items():
            for name, value in members.items():
                # the generated facts' names (local, shared) are compared with these by their bytes: ASCII names keep that the order of json.dumps(sort_keys=True)
                if not name.isascii() or json.dumps(name) != f'"{name}"': fail(f'{manual}: fact name {name!r} of {k} is not plain ASCII')
                rows.append(f'fact\t{gid}\t{field(k, f"{manual} key")}\t{name}\t{json_text(value)}')
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--nxnative', default=os.path.join(HERE, '..', '..', 'nxnative'), help='nxnative\'s checkout (default: beside nxmac)')
    ap.add_argument('--games-dir', action='append', default=[], help='a folder of profiles outside the repository (DM_GAMES_DIR)')
    ap.add_argument('-o', '--output', default=os.path.normpath(os.path.join(HERE, '..', 'src', 'shader', 'profiles.tsv')), help='the table to write')
    args = ap.parse_args()
    games = os.path.join(os.path.abspath(args.nxnative), 'dm', 'games')
    if not os.path.isfile(os.path.join(games, 'game.py')): fail(f'no nxnative checkout at {args.nxnative} (dm/games/game.py)')
    sys.path.insert(0, games)
    os.environ['DM_GAMES_DIR'] = ':'.join(os.path.abspath(d) for d in args.games_dir)
    os.environ.pop('DM_GAMES_FIRST', None)
    import game
    profiles = game.all_games()   # dm/games first, then DM_GAMES_DIR; a title ID's first profile wins
    try: commit = subprocess.run(['git', '-C', args.nxnative, 'rev-parse', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError): commit = 'unknown commit'
    rows, used = [], 0
    for g in profiles:
        r = profile_rows(game, g)
        if r is None: continue
        rows += r; used += 1
    out = ['# nxmac shader split profiles (src/shader/include/nxmac/shader/profiles.h describes the rows). Generated by tools/export_shader_profiles.py',
           '# from nxnative\'s game profiles: do not edit; run it again when they change.',
           f'source\tnxnative {commit}, {used} profiles'] + rows
    with open(args.output, 'w', encoding='utf-8', newline='\n') as f: f.write('\n'.join(out) + '\n')
    print(f'{args.output}: {used} profiles of {len(profiles)} ({len(rows)} rows)')


if __name__ == '__main__':
    main()
