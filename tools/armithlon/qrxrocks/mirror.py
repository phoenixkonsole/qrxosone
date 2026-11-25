#!/usr/bin/env python3
"""Mirrors https://qrx.rocks/ (the page, its CSS/JS/translations and the images they name) into a
directory for the AROS guest's browser test, and writes SHA256SUMS. usage: mirror.py <dir>"""
import os, re, subprocess, sys
base = 'https://qrx.rocks/'
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
os.chdir(out)
subprocess.run(['curl', '-sS', '-L', '-f', '-o', 'index.html', base], check=True)
html = open('index.html', encoding='utf-8').read()
todo = set(re.findall(r'(?:src|href|content)="((?:assets|locales)/[^"#?]+)"', html))
done = set()
while todo:
    p = todo.pop()
    if p in done or '%' in p:
        continue
    done.add(p)
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    r = subprocess.run(['curl', '-sS', '-L', '-f', '-o', p, base + p], capture_output=True, text=True)
    print(('OK   ' if r.returncode == 0 else 'FAIL ') + p)
    if r.returncode == 0 and p.endswith(('.css', '.js')):
        t = open(p, encoding='utf-8', errors='replace').read()
        for u in re.findall(r'url\(\s*[\'"]?([^\'")]+)[\'"]?\s*\)', t) + \
                re.findall(r'[\'"]((?:\.\./|\./)?(?:assets|screenshots|img|fonts|locales)/[^\'"]+\.(?:png|jpg|svg|webp|woff2?|ttf|js|json))[\'"]', t):
            if u.startswith(('data:', 'http', '#')):
                continue
            q = os.path.normpath(os.path.join(os.path.dirname(p), u)) if not u.startswith('/') else u.lstrip('/')
            if q not in done:
                todo.add(q)
files = sorted(os.path.relpath(os.path.join(d, f)) for d, _, fs in os.walk('.') for f in fs if f != 'SHA256SUMS')
with open('SHA256SUMS', 'w') as s:
    subprocess.run(['sha256sum'] + files, stdout=s, check=True)
print(len(files), 'files')
