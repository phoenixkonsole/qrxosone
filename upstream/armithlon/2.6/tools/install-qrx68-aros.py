#!/usr/bin/env python3
"""Install a *cross-compiled* QRX68Run into an AROS system host directory.
Does not edit existing QRXDesktop, User-Startup or boot ROMs.
"""
from pathlib import Path
import argparse
import shutil
import stat
import sys


def install(binary: Path, sys_dir: Path):
    binary = binary.resolve(strict=True)
    sys_dir = sys_dir.resolve(strict=True)
    if not sys_dir.is_dir() or not (sys_dir / 'S').is_dir():
        raise ValueError('expected extracted AROS SYSTEM directory with S/ (not disk image)')
    if not binary.is_file() or not binary.stat().st_size:
        raise ValueError('guest executable not built')
    # Avoid silently installing ELF32 Linux or Mach-O as an AROS command.
    magic = binary.read_bytes()[:4]
    if magic != b'\x00\x00\x03\xf3':
        raise ValueError('guest executable must be validated Amiga HUNK; ELF or host binary rejected')
    targets = [sys_dir / 'C' / 'QRX68Run']
    # Existing QRXDesktop uses QRX:C/QRX68Run for opt-in launches. The
    # destination is updated only if this actually belongs to the SYS: tree.
    if (sys_dir / 'QRX' / 'C').is_dir():
        targets.append(sys_dir / 'QRX' / 'C' / 'QRX68Run')
    # Preflight all destinations before changing even one of them.
    source_bytes = binary.read_bytes()
    for dest in targets:
        if dest.is_symlink():
            raise ValueError('refusing to replace symlink')
        if dest.exists() and dest.read_bytes() != source_bytes:
            raise ValueError(f'refusing to overwrite existing {dest}; back up explicitly')
    for dest in targets:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            shutil.copyfile(binary, dest)
            dest.chmod(dest.stat().st_mode | stat.S_IXUSR)
    print('installed:', *(str(t) for t in targets))
    if len(targets) == 1:
        print('QRXDesktop expects QRX:C/QRX68Run; ensure QRX: is assigned to SYS: or adapt the existing QRX: partition.')
    print('No startup scripts or existing QRXDesktop binary changed.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('guest_hunk', type=Path)
    p.add_argument('aros_system_directory', type=Path)
    a=p.parse_args()
    try: install(a.guest_hunk,a.aros_system_directory)
    except (ValueError,OSError) as e: p.error(str(e))

if __name__ == '__main__':main()
