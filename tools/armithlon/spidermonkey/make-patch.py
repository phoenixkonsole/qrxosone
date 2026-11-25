#!/usr/bin/env python3
"""Writes spidermonkey-153.8.0-aros.patch from a work tree: every file with a .qrxorig copy (kept by
qorig.sh before its first change) against that copy, and every new file listed in qrx-new-files.txt.
Usage: make-patch.py <firefox-153.8.0 tree>"""
import difflib, os, sys
here = os.path.dirname(os.path.abspath(__file__))
top = os.path.abspath(sys.argv[1])
out = []
pairs = []
for d, _, files in os.walk(top):
    for f in files:
        # .cargo-checksum.json is not in the patch: fetch.sh recomputes it (fix-checksums.py)
        if f.endswith('.qrxorig') and f != '.cargo-checksum.json.qrxorig':
            pairs.append(os.path.relpath(os.path.join(d, f[:-8]), top))
new = []
nf = os.path.join(top, 'qrx-new-files.txt')
if os.path.exists(nf):
    new = [l.strip() for l in open(nf) if l.strip()]
for rel in sorted(pairs) + sorted(new):
    cur = os.path.join(top, rel)
    orig = cur + '.qrxorig'
    a = open(orig, encoding='utf8').read().splitlines(True) if os.path.exists(orig) else []
    b = open(cur, encoding='utf8').read().splitlines(True)
    out += difflib.unified_diff(a, b, 'a/' + rel if a else '/dev/null', 'b/' + rel)
open(os.path.join(here, 'spidermonkey-153.8.0-aros.patch'), 'w', encoding='utf8').writelines(out)
print(f'{len(pairs)} changed, {len(new)} new files')
