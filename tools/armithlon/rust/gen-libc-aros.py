#!/usr/bin/env python3
"""Checks the libc crate's AROS module (src/unix/aros/mod.rs) against the AROS 64 SDK headers and fixes it.

Every `pub type`, `pub const` and `s! { pub struct }` of the module is measured with the m68k64 clang
against the headers the C library is built from: type sizes and signedness, the macro values of the
constants, struct sizes and field offsets. Types and constants are rewritten to AROS's values; a
constant AROS does not define is removed. Struct mismatches are reported (structs are fixed by hand).

usage: gen-libc-aros.py <aros/mod.rs> [--write]
"""
import os, re, subprocess, sys, tempfile

CLANG = os.environ.get('QRX_CLANG', '/opt/qrx-os-one/aros64/tc/bin/clang')
SDK = os.environ.get('QRX_SDK', '/opt/qrx-os-one/aros64/build-qrx/bin/native-m68k64/AROS/Development')
HEADERS = ['sys/types.h', 'stdint.h', 'stddef.h', 'stdio.h', 'stdlib.h', 'string.h', 'errno.h', 'fcntl.h',
           'unistd.h', 'signal.h', 'time.h', 'sys/time.h', 'sys/stat.h', 'sys/statvfs.h', 'dirent.h', 'pthread.h',
           'sched.h', 'semaphore.h', 'limits.h', 'locale.h', 'poll.h', 'termios.h', 'sys/select.h', 'sys/socket.h',
           'netinet/in.h', 'netinet/tcp.h', 'arpa/inet.h', 'netdb.h', 'sys/uio.h', 'sys/mman.h', 'sys/resource.h',
           'sys/utsname.h', 'sys/wait.h', 'pwd.h', 'grp.h', 'sys/un.h', 'sys/ioctl.h', 'net/if.h']

RUST_INT = {(1, True): 'i8', (1, False): 'u8', (2, True): 'i16', (2, False): 'u16',
            (4, True): 'i32', (4, False): 'u32', (8, True): 'i64', (8, False): 'u64'}


def available_headers():
    have = []
    for h in HEADERS:
        r = subprocess.run([CLANG, '--sysroot', SDK, '-fsyntax-only', '-x', 'c', '-'], input=f'#include <{h}>\n',
                           capture_output=True, text=True)
        if r.returncode == 0:
            have.append(h)
    return have


def probe(head, items):
    """items: list of (key, C expression); returns key -> int, for those that compile (round by round)."""
    items = list(items)
    for _ in range(40):
        src = head + ''.join(f'const long long qrx_v{i} = (long long)({e});\n' for i, (_, e) in enumerate(items))
        with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
            f.write(src)
        asm = f.name + '.s'
        r = subprocess.run([CLANG, '--sysroot', SDK, '-S', f.name, '-o', asm], capture_output=True, text=True)
        os.unlink(f.name)
        if r.returncode == 0:
            break
        nhead = head.count('\n')
        bad = {int(n) - nhead - 1 for n in re.findall(r'\.c:(\d+):\d+: error', r.stderr)}
        if not bad:
            raise SystemExit(r.stderr[-3000:])
        items = [it for i, it in enumerate(items) if i not in bad]
    out, cur = {}, None
    for line in open(asm).read().splitlines():
        m = re.match(r'qrx_v(\d+):', line.strip())
        if m:
            cur = int(m.group(1))
            continue
        m = re.match(r'\.(quad|long)\s+(-?\d+)', line.strip())
        if cur is not None and m:
            out[items[cur][0]] = int(m.group(2))
            cur = None
    os.unlink(asm)
    return out


def main():
    path = sys.argv[1]
    write = '--write' in sys.argv
    src = open(path).read()
    head = ''.join(f'#include <{h}>\n' for h in available_headers())
    types = re.findall(r'^pub type (\w+) = ([^;]+);', src, re.M)
    consts = re.findall(r'^pub const (\w+): ([\w:]+) = [^;]+;', src, re.M)
    structs = re.findall(r'pub struct (\w+) \{(.*?)\n    \}', src, re.S)
    items = []
    for name, _ in types:
        items += [(('tsize', name), f'sizeof({name})'), (('tsigned', name), f'(({name})-1) < 0'),
                  (('tptr', name), f'__builtin_classify_type(*({name} *)0) == 5'),
                  (('talign', name), f'_Alignof({name})')]
    for name, _ in consts:
        items.append((('const', name), name))
    for name, body in structs:
        items.append((('ssize', name), f'sizeof(struct {name})'))
        for field in re.findall(r'pub (\w+):', body):
            items.append((('off', name, field), f'__builtin_offsetof(struct {name}, {field})'))
    v = probe(head, items)
    changed, removed, missing_types = [], [], []
    for name, old in types:
        if ('tsize', name) not in v:
            missing_types.append(name)
            continue
        if v.get(('tptr', name)):
            new = '*mut c_void'
        elif ('tsigned', name) not in v:
            # a struct or union in AROS (e.g. the pthread types): an array of its size and alignment
            size, align = v[('tsize', name)], v.get(('talign', name), 1)
            elem = {8: 'u64', 4: 'u32', 2: 'u16'}.get(align, 'u8')
            new = f'[{elem}; {size // max(1, {"u64": 8, "u32": 4, "u16": 2}.get(elem, 1))}]'
        else:
            new = RUST_INT.get((v[('tsize', name)], bool(v[('tsigned', name)])), old)
        if new != old.strip():
            changed.append(f'type {name}: {old} -> {new}')
            src = re.sub(rf'^pub type {name} = [^;]+;', f'pub type {name} = {new};', src, flags=re.M)
    for name, ty in consts:
        if ('const', name) not in v:
            removed.append(name)
            src = re.sub(rf'^pub const {name}: [^;]+;\n', '', src, flags=re.M)
            continue
        val = v[('const', name)]
        src, n = re.subn(rf'^pub const {name}: ([\w:]+) = [^;]+;', lambda m: f'pub const {name}: {m.group(1)} = {val};', src,
                         flags=re.M)
    report = []
    for name, body in structs:
        if ('ssize', name) not in v:
            report.append(f'struct {name}: not in AROS')
            continue
        offs = {f: v.get(('off', name, f)) for f in re.findall(r'pub (\w+):', body)}
        miss = [f for f, o in offs.items() if o is None]
        report.append(f'struct {name}: size {v[("ssize", name)]}, offsets ' +
                      ', '.join(f'{f}={o}' for f, o in offs.items() if o is not None) +
                      (f'; not in AROS: {", ".join(miss)}' if miss else ''))
    print(f'{len(changed)} types changed, {len(missing_types)} types not in AROS, '
          f'{len(removed)} constants removed, {len(consts) - len(removed)} constants set')
    for c in changed:
        print('  ' + c)
    if missing_types:
        print('  types not in AROS: ' + ', '.join(missing_types))
    if removed:
        print('  constants not in AROS: ' + ', '.join(removed))
    for r in report:
        print('  ' + r)
    if write:
        open(path, 'w').write(src)




# ---- struct layouts from clang's record dump (gen-libc-aros.py <mod.rs> --structs NAME... [--write])
CMAP = {'char': 'c_char', 'signed char': 'c_schar', 'unsigned char': 'c_uchar', 'short': 'c_short',
        'unsigned short': 'c_ushort', 'int': 'c_int', 'unsigned int': 'c_uint', 'long': 'c_long',
        'unsigned long': 'c_ulong', 'long long': 'c_longlong', 'unsigned long long': 'c_ulonglong',
        'void *': '*mut c_void', 'char *': '*mut c_char', 'const char *': '*const c_char', 'double': 'f64',
        'float': 'f32'}


def rust_type(ct, defined):
    ct = ct.strip()
    m = re.match(r'(.*)\[(\d+)\]$', ct)
    if m:
        return f'[{rust_type(m.group(1), defined)}; {m.group(2)}]'
    ct = re.sub(r'\b(__)?(\w+_t)\b', lambda mm: mm.group(2), ct)        # __off_t -> off_t
    if ct in CMAP:
        return CMAP[ct]
    if ct.endswith('*'):
        base = ct[:-1].strip()
        const = base.startswith('const ')
        base = base.replace('const ', '')
        inner = rust_type(base, defined) if base not in ('void',) else 'c_void'
        return f"*{'const' if const else 'mut'} {inner}"
    if ct.startswith('struct ') or ct.startswith('union '):
        return ct.split()[1]
    if ct.startswith('void (') or '(*)' in ct:
        return 'Option<unsafe extern "C" fn()>'     # adjusted by hand where it matters
    return ct if ct in defined else ct


def dump_layouts(names, head):
    src = head + ''.join(f'struct {n} qrx_s{i};\n' for i, n in enumerate(names))
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(src)
    r = subprocess.run([CLANG, '--sysroot', SDK, '-c', '-o', '/dev/null', '-Xclang', '-fdump-record-layouts', f.name],
                       capture_output=True, text=True)
    os.unlink(f.name)
    out = {}
    for block in r.stdout.split('*** Dumping AST Record Layout'):
        m = re.search(r'^\s*0 \| struct (\w+)\n(.*?)\[sizeof=(\d+), align=(\d+)', block, re.S | re.M)
        if not m:
            continue
        fields = re.findall(r'^\s*(\d+) \|   (?! )(.+?) (\w+)$', m.group(2), re.M)
        out[m.group(1)] = ([(int(o), t, n) for o, t, n in fields], int(m.group(3)), int(m.group(4)))
    return out


def structs_main(path, names, write):
    src = open(path).read()
    head = ''.join(f'#include <{h}>\n' for h in available_headers())
    defined = set(re.findall(r'^pub type (\w+)', src, re.M))
    lay = dump_layouts(names, head)
    for n in names:
        if n not in lay:
            print(f'struct {n}: no layout (not a struct in AROS?)')
            continue
        fields, size, align = lay[n]
        body = ''.join(f'        pub {fn}: {rust_type(ft, defined)},\n' for _, ft, fn in fields)
        new = f'    pub struct {n} {{\n{body}    }}'
        print(f'// size {size}, align {align}\n{new}')
        src, k = re.subn(rf'    pub struct {n} \{{.*?\n    \}}', lambda m: new, src, count=1, flags=re.S)
        if not k:
            print(f'  (struct {n} not in the module: printed only)')
    if write:
        open(path, 'w').write(src)


if __name__ == '__main__':
    if '--structs' in sys.argv:
        i = sys.argv.index('--structs')
        structs_main(sys.argv[1], [a for a in sys.argv[i + 1:] if not a.startswith('--')], '--write' in sys.argv)
    else:
        main()
