#!/bin/bash
# The fixtures of the shader split's golden tests, and what nxnative's shader_split.py makes of them.
#
#   src/make_fixtures.sh <nxnative checkout> <python>
#
# <python>: Python 3.12+ (shader_split.py needs nothing else for these cases).
#
# gen.py makes up a small game per case in cases/ (no game data: shaders of random bytes around the fields the readers
# check), each in the forms the split reads:
#   botw, botw_image  Breath of the Wild's: Yaz0 BNSH files, packs (SARC) of Yaz0 SARCs with agl's BAHS packs and BNSH,
#                     effects holding several BNSH, layouts, BNSH in main; the second as a DMROMFS1 image
#   versions          a base game and an update (the union; the update's names prefixed with its folder), and DLC
#   damaged           files that do not read (a cut Yaz0 stream, a SARC member out of range, a BNSH whose variations
#                     run off its end, a key with two different control sections, ...): each left out with a red line
#   formats           gzip, Gust's zlib chunks, loose GLSLC outputs, MSHD, NDSR, NVN2 BNSH with a mesh shader, sead's
#                     primitive drawer, PlatinumGames' DAT archive, Needle, Sumo, Star Fox, MonoGame, Phyre; in main GLSLC
#                     outputs, YEBIS's zlib stream and loose control sections (a data record, adrp + add, none)
#   keyskip           a profile with shader_key_skip; single: single-stage programs and the pixel input cover;
#   glsl, capture     main's GLSL sources with glslcCompile (a red line), captured GLSLC outputs beside the RomFS
#   unported, unported_zip   other engines' files: left out with nxmac's own red line, and a zip that stops the split
# expect.py then runs shader_split.py on each and writes cases.tsv, expected.tsv (the SHA-256 of every file it writes) and
# lines.tsv (what it prints): the golden test holds nxmac's split to those.
set -eo pipefail
here=$(cd "$(dirname "$0")" && pwd); fixtures=$(dirname "$here")
nxnative=${1:?the nxnative checkout}; py=${2:?a python 3.12+}
rm -rf "$fixtures/cases"
"$py" "$here/gen.py" --low "$fixtures/cases" "$nxnative"
"$py" "$here/expect.py" "$fixtures" "$nxnative"
