#!/usr/bin/env python3
# Debugging aid: logs every statement at one indent level of a function to the engine log.
# usage: autolog.py file.c 'function signature prefix' [indent tabs]
import re, sys
p, sig = sys.argv[1], sys.argv[2]
ind = int(sys.argv[3]) if len(sys.argv) > 3 else 1
s = open(p).read()
a = s.index(sig)
b = s.index('\n}\n', a)
out, n = [], 0
tab = '\t' * ind
for line in s[a:b].split('\n'):
    out.append(line)
    if re.match('^' + tab + r'[A-Za-z_][^ ]*.*\);\s*(/\*.*)?$', line) and not line.strip().startswith(('return', 'else')):
        n += 1
        out.append(tab + 'qrx_log("%s %d: %s");' % (sig.split('(')[0].split()[-1][:16], n, re.sub(r'["\\]', '', line.strip())[:40]))
s = s[:a] + 'extern void qrx_log(const char*);\n' + '\n'.join(out) + s[b:]
open(p, 'w').write(s)
print(n, 'log points')
