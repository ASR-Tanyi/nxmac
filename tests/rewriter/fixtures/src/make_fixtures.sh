#!/bin/bash
# The fixtures of the rewriter's golden tests, and what nxnative's rewriter makes of them.
#
#   src/make_fixtures.sh <nxnative checkout> <python> [<googletest source folder>]
#
# <python> is a Python 3.12+ with nxnative's pinned capstone and lz4 (pip install -r nxnative/.github/requirements/tools.txt).
# It needs clang with the aarch64 target and ld.lld, and (for the compiled fixture) aarch64 libc and libstdc++ headers
# (Debian / Ubuntu: libc6-dev-arm64-cross libstdc++-13-dev-arm64-cross) and GoogleTest's sources (default: the ones
# nxmac's debug build fetched).
#
# The fixtures are small modules laid out as a Switch NSO is (crt0.s, module.ld, elf2nso.py):
#   sys, excl, jump, spin, region, data, fail   hand-written assembly, one kind of rewrite each (the .s files say which)
#   far       all of them in one module with 128 MB of bss: the patch area before the base
#   nro       the same code as an NRO
#   g, g2     GoogleTest compiled for aarch64, x9 and x18 changing places (swapreg.py: Linux compilers avoid x18, Switch
#             ones use it freely); g2 is g linked behind other code, as an update of a game moves its functions
#   botw_sites   zeros with the instruction words Breath of the Wild 1.9.0's patches assert at their offsets
#             (fake_build.py): what the frame-step code of src/rewriter/games is checked on, without the game
# A test profile (0100000000009000, gen_patches.py) registers g's build with cuts, gates and capabilities, and holds
# function patches made from g's functions, which g2 is matched against. patches_fixture.tsv is nxnative's tables with
# that profile's, as tools/export_patches.py writes them. Then nxnative's patch_nso.py rewrites every case of cases.tsv
# and expected.tsv gets the SHA-256 of each file it writes: the golden test holds nxmac's rewriter to those.
set -eo pipefail
here=$(cd "$(dirname "$0")" && pwd); fixtures=$(dirname "$here"); repo=$(cd "$fixtures/../../.." && pwd)
nxnative=${1:?the nxnative checkout}; py=${2:?a python with capstone 5.0.9 and lz4}
gtest=${3:-$repo/build/debug/_deps/googletest-src/googletest}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
T="--target=aarch64-linux-gnu -march=armv8-a"
LD="ld.lld -shared -Bsymbolic --no-relax --eh-frame-hdr --hash-style=sysv -z notext --no-rosegment -T $here/module.ld"

link() {   # link <name> <crt0> <objects...>: $work/<name>.elf
    local name=$1 crt0=$2; shift 2
    clang $T -c -o "$work/$name.crt0.o" "$here/$crt0"
    $LD -o "$work/$name.elf" "$work/$name.crt0.o" "$@"
}
asm() {   # asm <source name>...: their objects, in order
    local out=()
    for s in "$@"; do clang $T -c -o "$work/$s.o" "$here/$s.s"; out+=("$work/$s.o"); done
    echo "${out[@]}"
}
for m in sys excl jump spin region fail; do
    link $m crt0.s $(asm $m); "$py" "$here/elf2nso.py" "$work/$m.elf" "$fixtures/$m.nso"
done
link data crt0.s $(asm sys data_block excl); "$py" "$here/elf2nso.py" "$work/data.elf" "$fixtures/data.nso"
link far crt0.s $(asm sys excl jump spin region); "$py" "$here/elf2nso.py" "$work/far.elf" "$fixtures/far.nso" --bss 0x8000000
link nro crt0_nro.s $(asm sys excl spin); "$py" "$here/elf2nro.py" "$work/nro.elf" "$fixtures/nro.nro"

# GoogleTest, compiled to assembly, x9 <-> x18, assembled
clang++ -std=c++17 $T -mno-outline-atomics -O2 -fPIC -fno-stack-protector -S -I"$gtest" -I"$gtest/include" -o "$work/gtest.s" "$gtest/src/gtest-all.cc"
"$py" "$here/swapreg.py" 9 < "$work/gtest.s" > "$work/gtest.sw.s"
clang $T -c -o "$work/gtest.o" "$work/gtest.sw.s"
link g crt0.s "$work/gtest.o"; "$py" "$here/elf2nso.py" "$work/g.elf" "$fixtures/g.nso"
link g2 crt0.s $(asm spin) "$work/gtest.o"; "$py" "$here/elf2nso.py" "$work/g2.elf" "$fixtures/g2.nso"

# nxnative's tools and patch files with the test profile beside its own
tree=$work/nxnative; mkdir -p "$tree/dm"; cp -r "$nxnative/dm/tools" "$tree/dm/tools"
(cd "$nxnative" && find dm/games -maxdepth 2 \( -name patches.py -o -name game.tsv \)) | while read -r f; do
    mkdir -p "$tree/$(dirname "$f")"; cp "$nxnative/$f" "$tree/$f"
done
"$py" "$here/gen_patches.py" "$tree" 0100000000009000 "$fixtures/g.nso"
"$py" "$repo/tools/export_patches.py" --nxnative "$tree" -o "$fixtures/patches_fixture.tsv"
commit=$(git -C "$nxnative" rev-parse HEAD)
sed -i.bak "s/^source\t.*/source\tnxnative $commit and the test profile 0100000000009000/" "$fixtures/patches_fixture.tsv"; rm -f "$fixtures/patches_fixture.tsv.bak"

# Breath of the Wild 1.9.0's main, as far as its patches assert it
botw=cd57b23fa4bbad65803d9788c01821ee
"$py" "$here/fake_build.py" "$tree" 01007EF00011E000 $botw "$work/botw.s"
clang $T -c -o "$work/botw.o" "$work/botw.s"
$LD -o "$work/botw.elf" "$work/botw.o"
"$py" "$here/elf2nso.py" "$work/botw.elf" "$fixtures/botw_sites.nso" --build-id $botw

# What nxnative's rewriter writes for each case
{
    echo "# SHA-256 of each file nxnative's patch_nso.py ($commit) writes for the cases of cases.tsv (- when it writes none): case, file, hash."
    echo "# Written by src/make_fixtures.sh: do not edit."
    grep -v '^#' "$fixtures/cases.tsv" | while IFS=$'\t' read -r case file name game want; do
        out=$work/out/$case; mkdir -p "$out"
        env=(); [ "$game" != "-" ] && env=(DM_GAME="$game")
        if env "${env[@]}" "$py" "$tree/dm/tools/patch_nso.py" "$fixtures/$file" "$out" "$name" > "$out/log.txt" 2>&1; then
            [ "$want" = ok ] || { echo "$case: patch_nso.py rewrote it, but the case expects a failure" >&2; exit 1; }
            for ext in patched.bin fixups.tsv gates.tsv sites.tsv cuts.tsv changed.tsv obs.tsv match.tsv; do
                if [ -f "$out/$name.$ext" ]; then h=$(sha256sum "$out/$name.$ext" | cut -d' ' -f1); else h=-; fi
                printf '%s\t%s\t%s\n' "$case" "$ext" "$h"
            done
        else
            [ "$want" != ok ] && grep -q -- "$want" "$out/log.txt" || { tail -3 "$out/log.txt" >&2; echo "$case: patch_nso.py failed" >&2; exit 1; }
        fi
    done
} > "$fixtures/expected.tsv"
echo "fixtures and expected.tsv written to $fixtures"
