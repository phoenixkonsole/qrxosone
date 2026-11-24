#!/usr/bin/env python3
"""Root file system of the QRX ARMithlon Pi 4 appliance.

Not a distribution: BusyBox, the ARMithlon engine, and exactly the programs the appliance runs
(wpa_supplicant, BlueZ with D-Bus), each with the shared libraries it really links (ELF
DT_NEEDED, resolved recursively) and the libraries SDL2 and Mesa load at run time. Files come
from Ubuntu 24.04 arm64 packages unpacked into STAGE (fetch-debs.sh); nothing else of those
packages is copied.

  mkrootfs.py STAGE ENGINE BUNDLE OUT
    ENGINE  the AArch64 qrx68100_full_run (SDL2 window); ENGINE64=qrx_ec64_run in the environment
            adds QRX OS One 64's machine
    BUNDLE  QRX OS One: aros-rom.bin, aros-ext.bin, boot.adf, SystemFull/ (bundle/build-bundle.sh)
"""
import os, shutil, stat, subprocess, sys
from pathlib import Path

STAGE, ENGINE, BUNDLE, OUT = map(Path, sys.argv[1:5])
HERE = Path(__file__).resolve().parent
LIB = 'usr/lib/aarch64-linux-gnu'
NATIVE = os.uname().machine == 'aarch64'          # built on the Pi itself: no cross tools, no qemu
READELF = os.environ.get('READELF', 'readelf' if NATIVE else 'aarch64-linux-gnu-readelf')

# programs taken from the packages
PROGRAMS = [
    'usr/bin/busybox',
    'usr/sbin/wpa_supplicant', 'usr/sbin/wpa_cli', 'usr/bin/wpa_passphrase', 'usr/sbin/iw',
    'usr/bin/dbus-daemon', 'usr/libexec/bluetooth/bluetoothd', 'usr/bin/bluetoothctl', 'usr/bin/btmgmt',
    'usr/sbin/resize2fs', 'usr/sbin/e2fsck',          # first start: the root fills the card
]
# loaded with dlopen: glvnd's EGL loads Mesa's EGL (libEGL_mesa), which loads the gallium driver.
# The engine itself links EGL, glvnd's libOpenGL (no GLX, so no X11) and ALSA directly; no SDL.
RUNTIME_LIBS = ['libEGL_mesa.so.0', 'dri/libdril_dri.so', 'gbm/dri_gbm.so']
DRI_NAMES = ['v3d_dri.so', 'vc4_dri.so', 'kms_swrast_dri.so', 'swrast_dri.so']   # links to libdril
DATA = ['usr/share/glvnd/egl_vendor.d/50_mesa.json', 'usr/share/drirc.d', 'usr/share/alsa',
        'usr/share/dbus-1/system.conf', 'usr/share/dbus-1/system.d/bluetooth.conf',
        'usr/share/dbus-1/system.d/wpa_supplicant.conf', 'etc/bluetooth/main.conf']

copied = set()
def put(rel):
    """copy STAGE/rel to OUT/rel, keeping symlinks (and their targets)"""
    src, dst = STAGE / rel, OUT / rel
    if rel in copied or not (src.exists() or src.is_symlink()):
        return src.exists() or src.is_symlink()
    copied.add(rel)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_symlink():
        t = os.readlink(src)
        if dst.is_symlink() or dst.exists(): dst.unlink()
        os.symlink(t, dst)
        put(os.path.normpath(os.path.join(os.path.dirname(rel), t)) if not t.startswith('/') else t.lstrip('/'))
    elif src.is_dir():
        shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)
    return True

def needed(path):
    out = subprocess.run([READELF, '-d', str(path)], capture_output=True, text=True).stdout
    return [l.split('[', 1)[1].split(']', 1)[0] for l in out.splitlines() if '(NEEDED)' in l]

def find_lib(name):
    for d in (LIB, 'usr/lib', 'lib/aarch64-linux-gnu', 'lib', f'{LIB}/pulseaudio'):   # pulseaudio: libpulse's RUNPATH
        p = STAGE / d / name
        if p.exists() or p.is_symlink():
            return f'{d}/{name}'
    return None

missing = []
def with_deps(rel, real=None):
    real = real or STAGE / rel
    todo = [real]
    while todo:
        f = todo.pop()
        for n in needed(f):
            r = find_lib(n)
            if not r:
                missing.append(f'{n} (needed by {f.name})'); continue
            if r in copied: continue
            put(r)
            todo.append((STAGE / r).resolve())

def main():
    if OUT.exists(): shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    # merged /usr: /bin, /sbin, /lib point into /usr
    for d in ('usr/bin', 'usr/sbin', LIB):
        (OUT / d).mkdir(parents=True, exist_ok=True)
    for l, t in (('bin', 'usr/bin'), ('sbin', 'usr/sbin'), ('lib', 'usr/lib')):
        os.symlink(t, OUT / l)
    put('usr/lib/ld-linux-aarch64.so.1')
    for p in PROGRAMS:
        if not put(p): missing.append(p); continue
        with_deps(p)
    for l in RUNTIME_LIBS:
        r = f'{LIB}/{l}'
        if not put(r): missing.append(r); continue
        with_deps(r, (STAGE / r).resolve())
    for n in DRI_NAMES:
        put(f'{LIB}/dri/{n}')
    for d in DATA:
        if not put(d): missing.append(d)
    # BusyBox applets as links (init, sh, mount, ip, udhcpc, mdev, ...)
    applets = subprocess.run(([] if NATIVE else ['qemu-aarch64']) + [str(STAGE / 'usr/bin/busybox'), '--list-full'],
                             capture_output=True, text=True).stdout.split()
    for a in applets:
        # merged /usr: bin/x and sbin/x live in usr/bin and usr/sbin; linuxrc is not wanted
        if a.startswith('usr/'): dst = OUT / a
        elif a.startswith(('bin/', 'sbin/')): dst = OUT / 'usr' / a
        else: continue
        if not dst.exists() and not dst.is_symlink():
            dst.parent.mkdir(parents=True, exist_ok=True)
            os.symlink('/usr/bin/busybox', dst)
    # the engine and QRX OS One
    q = OUT / 'qrx'
    (q / 'bin').mkdir(parents=True)
    shutil.copy2(ENGINE, q / 'bin/armithlon')
    with_deps('qrx/bin/armithlon', ENGINE)
    # QRX OS One 64's machine (qrx_ec64_run with the SDL window), when given: ENGINE64=path
    e64 = os.environ.get('ENGINE64')
    if e64:
        shutil.copy2(e64, q / 'bin/armithlon64')
        with_deps('qrx/bin/armithlon64', Path(e64))
    for f in ('aros-rom.bin', 'aros-ext.bin', 'boot.adf'):
        shutil.copy2(BUNDLE / f, q / f)
    shutil.copytree(BUNDLE / 'SystemFull', q / 'System', symlinks=True)
    # the appliance's own files: init scripts, settings service, configuration
    shutil.copytree(HERE / 'rootfs', OUT, symlinks=True, dirs_exist_ok=True)
    for f in ('etc/init.d/rcS', 'etc/init.d/rcK', 'qrx/session', 'usr/share/udhcpc/default.script', 'qrx/netctl', 'qrx/appstore/store', 'usr/sbin/qrx-grow'):
        p = OUT / f
        if p.exists(): p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    for d in ('dev', 'proc', 'sys', 'run', 'tmp', 'var/lib/bluetooth', 'data', 'root', 'mnt', 'boot'):
        (OUT / d).mkdir(parents=True, exist_ok=True)
    (OUT / 'var/run').exists() or os.symlink('/run', OUT / 'var/run')
    if missing:
        print('MISSING:', *missing, sep='\n  ')
        sys.exit(1)
    size = sum(f.stat().st_size for f in OUT.rglob('*') if f.is_file() and not f.is_symlink())
    print(f'rootfs {OUT}: {len(copied)} files from packages, {size // (1024 * 1024)} MiB')

main()
