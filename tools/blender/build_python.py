#!/usr/bin/env python3
"""Builds CPython 3.5.10 statically for QRX OS One, into .build/blender-2.79b/py.

Python's Makefile runs freshly built programs, which a cross build cannot. The generated files it wants
(grammar, frozen importlib) ship in the source package, so this compiles from the Makefile's own lists.
configure must have run in py/build; pyconfig.h is corrected here for what it cannot find out.
"""
import concurrent.futures as cf, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W = Path(os.environ.get('QRX_BLENDER_WORK', '/Users/pascalpapara/Documents/GitHub/qrxosone/.build/blender-2.79b'))
SRC = W / 'py/Python-3.5.10'
B = W / 'py/build'
CC = str(HERE / 'aros_cc.py')
AR = '/opt/homebrew/bin/m68k-elf-ar'

# name in config.c, sources under Modules
MODULES = {
    'posix': ['posixmodule.c'], 'errno': ['errnomodule.c'], '_sre': ['_sre.c'], '_codecs': ['_codecsmodule.c'],
    '_weakref': ['_weakref.c'], '_functools': ['_functoolsmodule.c'], '_operator': ['_operator.c'],
    '_collections': ['_collectionsmodule.c'], 'itertools': ['itertoolsmodule.c'], 'atexit': ['atexitmodule.c'],
    '_signal': ['signalmodule.c'], '_stat': ['_stat.c'], 'time': ['timemodule.c'], '_thread': ['_threadmodule.c'],
    '_locale': ['_localemodule.c'],
    '_io': ['_io/_iomodule.c', '_io/iobase.c', '_io/fileio.c', '_io/bytesio.c', '_io/bufferedio.c', '_io/textio.c', '_io/stringio.c'],
    'zipimport': ['zipimport.c'], 'faulthandler': ['faulthandler.c'], '_tracemalloc': ['_tracemalloc.c', 'hashtable.c'],
    '_symtable': ['symtablemodule.c'], 'math': ['mathmodule.c', '_math.c'], 'cmath': ['cmathmodule.c'],
    '_struct': ['_struct.c'], 'binascii': ['binascii.c'], '_random': ['_randommodule.c'], 'array': ['arraymodule.c'],
    '_pickle': ['_pickle.c'], '_datetime': ['_datetimemodule.c'], '_bisect': ['_bisectmodule.c'],
    '_heapq': ['_heapqmodule.c'], 'unicodedata': ['unicodedata.c'], '_md5': ['md5module.c'], '_sha1': ['sha1module.c'],
    '_sha256': ['sha256module.c'], '_sha512': ['sha512module.c'], '_csv': ['_csv.c'], 'zlib': ['zlibmodule.c'],
    # sockets go to the host through the ARMithlon door, see port/qrx_socket.c
    '_socket': ['socketmodule.c'], 'select': ['selectmodule.c'],
}
CORE_MODULES = ['main.c', 'gcmodule.c', 'getbuildinfo.c', 'getpath.c', 'xxsubtype.c']


def fix_pyconfig():
    p = B / 'pyconfig.h'
    s = p.read_text()
    s = s.split('/* QRX OS One')[0]
    for name in ('_FILE_OFFSET_BITS', '_LARGEFILE_SOURCE', 'HAVE_LARGEFILE_SUPPORT', 'HAVE_DYNAMIC_LOADING',
                 'HAVE_SEM_OPEN', 'HAVE_BROKEN_SEM_GETVALUE', 'HAVE_CLOCK_GETTIME', 'HAVE_CLOCK_SETTIME', 'HAVE_SYS_RESOURCE_H', 'HAVE_GETRUSAGE',
                 'HAVE_PTHREAD_KILL', 'HAVE_GETADDRINFO', 'HAVE_GETNAMEINFO', 'HAVE_ADDRINFO', 'HAVE_SOCKADDR_STORAGE', 'HAVE_SYS_EPOLL_H', 'HAVE_EPOLL', 'HAVE_EPOLL_CREATE1', 'HAVE_KQUEUE', 'HAVE_SYS_DEVPOLL_H',
                 'HAVE_GETHOSTBYNAME_R', 'HAVE_GETHOSTBYNAME_R_6_ARG', 'HAVE_GETHOSTBYNAME_R_5_ARG', 'HAVE_GETHOSTBYNAME_R_3_ARG', 'HAVE_NETPACKET_PACKET_H',
                 'HAVE_LINUX_NETLINK_H', 'HAVE_LINUX_CAN_H', 'HAVE_LINUX_TIPC_H', 'HAVE_SOCKADDR_ALG', 'HAVE_SYS_UN_H', 'HAVE_SENDMSG', 'HAVE_RECVMSG', 'HAVE_HSTRERROR',
                 'HAVE_INET_PTON', 'HAVE_INET_NTOP', 'HAVE_GETLOADAVG', 'HAVE_SETHOSTNAME', 'HAVE_GETHOSTBYADDR', 'HAVE_PTHREAD_SIGMASK', 'HAVE_FORK', 'HAVE_FORKPTY', 'HAVE_OPENPTY', '_POSIX_C_SOURCE', '_XOPEN_SOURCE', '_XOPEN_SOURCE_EXTENDED'):
        s = re.sub(rf'^#define {name}\b.*$', f'/* #undef {name} */', s, flags=re.M)
    s = re.sub(r'^#define SIZEOF_OFF_T .*$', '#define SIZEOF_OFF_T 4', s, flags=re.M)
    s = s.replace('/* #undef DOUBLE_IS_BIG_ENDIAN_IEEE754 */', '#define DOUBLE_IS_BIG_ENDIAN_IEEE754 1')
    # select and poll come from port/qrx_socket.c and the headers in shim/include
    for name in ('HAVE_SYS_SELECT_H', 'HAVE_POLL', 'HAVE_POLL_H'):
        s = s.replace(f'/* #undef {name} */', f'#define {name} 1')
    p.write_text(s + '\n/* QRX OS One: corrected by tools/blender/build_python.py */\n')


def makefile_objs():
    text = (B / 'Makefile').read_text().replace('\\\n', ' ')
    out = {}
    for var in ('POBJS', 'OBJECT_OBJS', 'PYTHON_OBJS'):
        m = re.search(rf'^{var}=(.*)$', text, flags=re.M)
        out[var] = [w for w in m.group(1).split() if w.endswith('.o')]
    return out


def compile_one(src, obj, extra=()):
    obj.parent.mkdir(parents=True, exist_ok=True)
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, (B / 'pyconfig.h').stat().st_mtime):
        return None
    cmd = [CC, '-O2', '-fwrapv', '-DPy_BUILD_CORE', '-DNDEBUG', f'-I{B}', f'-I{SRC}/Include', f'-I{SRC}/Python',
           f'-I{SRC}/Modules', f'-I{SRC}/Modules/_io', *extra, '-c', str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return f'{src.relative_to(SRC)}: ' + ' | '.join(l[:150] for l in r.stderr.splitlines() if 'error' in l)[:600]
    return None


def write_config():
    text = (SRC / 'Modules/config.c.in').read_text()
    names = list(MODULES)
    decl = ''.join(f'extern PyObject* PyInit_{n}(void);\n' for n in names)
    table = ''.join(f'    {{"{n}", PyInit_{n}}},\n' for n in names)
    text = text.replace('/* -- ADDMODULE MARKER 1 -- */', decl).replace('/* -- ADDMODULE MARKER 2 -- */', table)
    (B / 'Modules').mkdir(exist_ok=True)
    (B / 'Modules/config.c').write_text(text)


def main():
    fix_pyconfig()
    write_config()
    objs = makefile_objs()
    jobs = []
    for var, lst in objs.items():
        for o in lst:
            if o.endswith('dynload_shlib.o'):
                o = 'Python/dynload_stub.o'
            jobs.append((SRC / o.replace('.o', '.c'), B / o, ()))
    for name in ('Parser/myreadline', 'Parser/parsetok', 'Parser/tokenizer', 'Python/dynload_stub', 'Objects/bytes_methods',
                 'Python/thread', 'Python/frozen'):
        o = B / (name + '.o')
        if not any(j[1] == o for j in jobs):
            jobs.append((SRC / (name + '.c'), o, ()))
    for m in CORE_MODULES:
        extra = ()
        if m == 'getpath.c':
            extra = ('-DPYTHONPATH=""', '-DPREFIX="PROGDIR:"', '-DEXEC_PREFIX="PROGDIR:"', '-DVERSION="3.5"', '-DVPATH=""')
        jobs.append((SRC / 'Modules' / m, B / 'Modules' / m.replace('.c', '.o'), extra))
    for srcs in MODULES.values():
        for m in srcs:
            jobs.append((SRC / 'Modules' / m, B / 'Modules' / m.replace('.c', '.o'), ()))
    jobs.append((B / 'Modules/config.c', B / 'Modules/config.o', ()))
    for i, j in enumerate(jobs):
        if j[0].name == 'sysmodule.c':
            jobs[i] = (j[0], j[1], ('-DABIFLAGS="m"',))
        if j[0].name == 'getplatform.c':
            jobs[i] = (j[0], j[1], ('-DPLATFORM="aros"',))
    with cf.ThreadPoolExecutor(8) as ex:
        res = list(ex.map(lambda j: compile_one(*j), jobs))
    errors = [r for r in res if r]
    for e in errors:
        print(e)
    print(f'{len(jobs) - len(errors)} of {len(jobs)} files compiled')
    if errors:
        return 1
    lib = B / 'libpython3.5m.a'
    lib.unlink(missing_ok=True)
    subprocess.check_call([AR, 'rcs', str(lib), *[str(j[1]) for j in jobs]])
    main_o = B / 'Programs/python.o'
    err = compile_one(SRC / 'Programs/python.c', main_o)
    if err:
        print(err)
        return 1
    return subprocess.call([CC, str(main_o), str(lib), '-lz', '-o', str(B / 'python.elf')])


if __name__ == '__main__':
    sys.exit(main())
