#!/usr/bin/env python3
# A translator that records: runs nxnative's dmsh_xlate (REAL_XLATE) on the jobs, then keeps each job's outputs (<key>.spv, <key>.res.json) and what it
# printed under the job's digest in RECORD_DIR, for replay_xlate.py. --source-sha is passed through.
import hashlib, json, os, shutil, subprocess, sys
REAL, OUT = os.environ['REAL_XLATE'], os.environ['RECORD_DIR']
if sys.argv[1:] == ['--source-sha']: os.execv(REAL, [REAL, '--source-sha'])
def digest(job):
    j = dict(job); code = open(j.pop('code'), 'rb').read()
    return hashlib.sha256(json.dumps(j, sort_keys=True).encode() + b'\0' + code).hexdigest()[:24]
jobs_path, outdir = sys.argv[1:3]
jobs = json.load(open(jobs_path))
r = subprocess.run([REAL, jobs_path, outdir], capture_output=True, text=True)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
os.makedirs(OUT, exist_ok=True)
for job in jobs:
    d = os.path.join(OUT, digest(job)); os.makedirs(d, exist_ok=True)
    for ext in ('.spv', '.res.json'):
        p = os.path.join(outdir, job['key'] + ext)
        if os.path.exists(p): shutil.copyfile(p, os.path.join(d, 'out' + ext))
    open(os.path.join(d, 'stderr'), 'w').write(''.join(l + '\n' for l in r.stderr.splitlines() if l.startswith(f'[RED] {job["key"]}:')))
sys.exit(r.returncode)
