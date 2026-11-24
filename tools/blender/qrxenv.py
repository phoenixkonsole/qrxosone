#!/usr/bin/env python3
"""Where the Blender build finds its things, on the Mac and on Linux alike.

    QRX_ROOT          the checkout with .build/ (default: the repository this script is in)
    QRX_BLENDER_WORK  the work directory (default: $QRX_ROOT/.build/blender-2.79b)
    QRX_SDK_DIR       the AROS SDK and its toolchain.json (default: $QRX_ROOT/.build/qrx-sdk if it has
                      bin/toolchain.json, else $QRX_ROOT/.build/qrx-sdk-elf as tools/qrx86/bootstrap-aros-sdk.py
                      makes it around m68k-linux-gnu-gcc)
    QRX_BOOT_DIR      boot.adf, aros-rom.bin, aros-ext.bin for the guest (default: the Mac bundle's Resources)
    QRX_SYSTEM_TREE   the system the guest's test tree is copied from (default: $QRX_ROOT/.build/bundle/SystemFull)

The cross tools (gcc, g++, ar, nm, ...) and the compiler version come from toolchain.json, so the
libstdc++ of the compiler's own version is built (see LIBSTDCXX). Python: `from qrxenv import ...`;
shell: `eval "$("$HERE/qrxenv.py" sh)"`.
"""
import json, os, shlex, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('QRX_ROOT') or HERE.parents[1])
WORK = Path(os.environ.get('QRX_BLENDER_WORK') or ROOT / '.build/blender-2.79b')


def _sdk_dir():
    if os.environ.get('QRX_SDK_DIR'):
        return Path(os.environ['QRX_SDK_DIR'])
    mac = ROOT / '.build/qrx-sdk'
    return mac if (mac / 'bin/toolchain.json').exists() else ROOT / '.build/qrx-sdk-elf'


SDK_DIR = _sdk_dir()
CFG = json.loads((SDK_DIR / 'bin/toolchain.json').read_text())
SDK = Path(CFG['sdk'])
_PREFIX = CFG['ar'][:-2]                     # /opt/homebrew/bin/m68k-elf- or /usr/bin/m68k-linux-gnu-


def tool(name):
    """A cross tool: ar, nm, ranlib, readelf, as, ld, objdump, strip."""
    return CFG[name] if name in CFG and name != 'gcc' else _PREFIX + name


GCC = CFG['gcc']
_d, _b = os.path.split(GCC)
GXX = os.path.join(_d, _b.replace('gcc', 'g++', 1))
GCC_VERSION = subprocess.run([GCC, '-dumpfullversion'], capture_output=True, text=True).stdout.strip()
# libstdc++ for the compiler's own version (the headers must match the compiler); the tarballs are pinned
LIBSTDCXX = {
    '16.2.0': ('https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz',
               'e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e'),
    # signature checked with the GNU keyring (Jakub Jelinek, key 6C35B99309B5FA62) on 2026-11-24
    '13.3.0': ('https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/gcc-13.3.0.tar.xz',
               '0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083'),
}
GCC_SRC = WORK / 'gcc' / f'gcc-{GCC_VERSION}'
BUILD_TRIPLE = subprocess.run(['cc', '-dumpmachine'], capture_output=True, text=True).stdout.strip()
BOOT_DIR = Path(os.environ.get('QRX_BOOT_DIR') or ROOT / '.build/bundle/ARMithlon.app/Contents/Resources')
SYSTEM_TREE = Path(os.environ.get('QRX_SYSTEM_TREE') or ROOT / '.build/bundle/SystemFull')


def shell():
    url, sha = LIBSTDCXX.get(GCC_VERSION, ('', ''))
    v = dict(QRX_ROOT=ROOT, W=WORK, SDK=SDK, SDK_DIR=SDK_DIR, XGCC=GCC, XGXX=GXX, XAR=tool('ar'), XNM=tool('nm'),
             XRANLIB=tool('ranlib'), XREADELF=tool('readelf'), XAS=tool('as'), XLD=tool('ld'), GCC_VERSION=GCC_VERSION,
             GCC_URL=url, GCC_SHA256=sha, BUILD_TRIPLE=BUILD_TRIPLE, BOOT_DIR=BOOT_DIR, SYSTEM_TREE=SYSTEM_TREE)
    return ''.join(f'{k}={shlex.quote(str(x))}\n' for k, x in v.items())


if __name__ == '__main__':
    if sys.argv[1:] == ['sh']:
        sys.stdout.write(shell())
    else:
        print(__doc__)
