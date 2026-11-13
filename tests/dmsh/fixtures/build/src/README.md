How the made-up game of `tests/dmsh/fixtures/build` is made (in a checkout of nxnative with its translator and a venv for its tools):

```sh
F=tests/dmsh/fixtures/build
python3 $F/src/gen.py /tmp/g                     # /tmp/g/pkg and /tmp/g/profile/0100000000FA4E00
# the translator's outputs for each job, recorded: dmsh_build.py run with rec_xlate.py as its translator, the stand-in Metal tools first on PATH
PATH=$F/bin:$PATH REAL_XLATE=../nxnative/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate RECORD_DIR=$F/xlate XLATE=$F/src/rec_xlate.py \
  DM_GAME=/tmp/g/profile/0100000000FA4E00 ../venv/bin/python3 ../nxnative/dm/gfx/tools/dmsh_build.py /tmp/g/pkg /tmp/g/out
cp -r /tmp/g/pkg $F/pkg; cp -r /tmp/g/profile $F/profile
cp /tmp/g/out/b-7c609af36b0f.img /tmp/g/profile/0100000000FA4E00/c1_full.tsv /tmp/g/profile/0100000000FA4E00/cb_extent.tsv $F/expected/
```

dmsh_build.py checks its tools against nxnative's lock file first: a translator built elsewhere and the stand-in Metal tools differ from it, so
that check is answered with no complaint for this run (`tool_lock.check_tools = lambda: []` before running the script with `runpy`). Running
dmsh_build.py again with `XLATE=$F/bin/dmsh_xlate` (the replay) gives the same library.
