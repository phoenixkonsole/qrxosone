#!/usr/bin/env python3
"""Reads the GL prototypes from GLEW's glew.h and keeps those the Mac's OpenGL.framework exports.

    extract.py path/to/glew.h > gl_functions.json

The result is checked in, so the engine and the guest library build without Blender's source.
Run it again only to take functions from a newer glew.h."""
import json, re, subprocess, sys, tempfile
from pathlib import Path

text = Path(sys.argv[1]).read_text()
protos = {}
# GL 1.1, declared directly: programs link these by name.
for m in re.finditer(r'^GLAPI (.+?) GLAPIENTRY (gl\w+) \((.*?)\);', text, re.M):
    protos[m.group(2)] = (m.group(1).strip(), m.group(3))
gl11 = set(protos)
# Everything newer: a function pointer type plus a define naming the function.
pfn = {m.group(2): (m.group(1).strip(), m.group(3))
       for m in re.finditer(r'^typedef (.+?) \(GLAPIENTRY \* (PFNGL\w+PROC)\) \((.*?)\);', text, re.M)}
var = {m.group(2): m.group(1) for m in re.finditer(r'^GLEW_FUN_EXPORT (PFNGL\w+PROC) (__glew\w+);', text, re.M)}
for m in re.finditer(r'^#define (gl\w+) GLEW_GET_FUN\((__glew\w+)\)', text, re.M):
    t = var.get(m.group(2))
    if t in pfn and m.group(1) not in protos:
        protos[m.group(1)] = pfn[t]


def params(s):
    s = s.strip()
    if s in ('', 'void'):
        return []
    out = []
    for i, p in enumerate(x.strip() for x in s.split(',')):
        m = re.match(r'(.*?)(\w+)\s*(\[\d*\])?$', p)
        typ, name = m.group(1).strip(), m.group(2)
        if m.group(3):
            typ += ' *'
        if not typ or typ in ('const',):  # unnamed parameter
            typ, name = p, f'a{i}'
        out.append([re.sub(r'\s+', ' ', typ.replace('*', ' * ')).replace('* *', '**').strip(), name])
    return out


names = sorted(protos)
with tempfile.TemporaryDirectory() as d:
    c = Path(d) / 'have.c'
    c.write_text('#include <dlfcn.h>\n#include <stdio.h>\nint main(void){static const char *n[]={'
                 + ','.join(f'"{n}"' for n in names)
                 + '};for(unsigned i=0;i<sizeof n/sizeof*n;i++)'
                 'if(dlsym(RTLD_DEFAULT,n[i]))puts(n[i]);}\n')
    subprocess.check_call(['cc', '-o', str(Path(d) / 'have'), str(c), '-framework', 'OpenGL'])
    have = set(subprocess.check_output([str(Path(d) / 'have')], text=True).split())
out = [{'name': n, 'ret': re.sub(r'\s+', ' ', protos[n][0]), 'params': params(protos[n][1]), 'gl11': n in gl11} for n in names if n in have]
json.dump(out, sys.stdout, indent=0)
print(file=sys.stderr, end=f'{len(out)} of {len(names)} functions on this Mac\n')
