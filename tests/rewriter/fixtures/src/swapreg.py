#!/usr/bin/env python3
"""swapreg.py <N> < in.s > out.s: xN / wN and x18 / w18 change places in compiler-made aarch64 assembly (both are caller-saved temporaries, so it is the code another
register allocation would have given): Switch compilers use x18 freely, Linux ones avoid it"""
import re, sys
n = sys.argv[1]
def sw(m):
    r = m.group(2)
    return m.group(1) + ('18' if r == n else n)
pat = re.compile(r'\b([xw])(%s|18)\b' % n)
for line in sys.stdin:
    if line.lstrip().startswith(('.', '//', '#')): sys.stdout.write(line); continue
    code, sep, rest = line.partition('//')
    sys.stdout.write(pat.sub(sw, code) + sep + rest)
