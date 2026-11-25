#!/usr/bin/env python3
"""Recomputes .cargo-checksum.json of vendored crates after a patch changed them (cargo checks every
listed file). Usage: fix-checksums.py <crate dir>..."""
import hashlib, json, os, sys
for d in sys.argv[1:]:
    p = os.path.join(d, '.cargo-checksum.json')
    j = json.load(open(p))
    files = {}
    for root, _, names in os.walk(d):
        for n in names:
            full = os.path.join(root, n)
            rel = os.path.relpath(full, d)
            if rel == '.cargo-checksum.json' or rel.endswith('.qrxorig') or rel.endswith('.rej') or rel.endswith('.orig'):
                continue
            files[rel] = hashlib.sha256(open(full, 'rb').read()).hexdigest()
    j['files'] = dict(sorted(files.items()))
    json.dump(j, open(p, 'w'), separators=(',', ':'))
    print(d, len(files), 'files')
