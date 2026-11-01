#!/usr/bin/env python3
"""Generate and optionally run a REAL AmigaOS 3.x compatibility profile.

This runs user-provided AmigaOS in FS-UAE. It is NOT native QRX68100 boot,
nor an attempt to redistribute a copyrighted Kickstart/Workbench image.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def clean(p: Path) -> str:
    resolved = str(p.resolve())
    if any(ch in resolved for ch in ('\n', '\r', '[', ']')):
        raise ValueError('unsafe FS-UAE config pathname')
    return resolved


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kickstart', type=Path, required=True, help='licensed AmigaOS 3.x ROM (not bundled)')
    ap.add_argument('--system-dir', type=Path, required=True, help='installed AmigaOS 3.x Workbench folder')
    ap.add_argument('--share', type=Path, help='optional secondary host directory volume')
    ap.add_argument('--cpu', choices=('68020', '68040', '68060'), default='68040')
    ap.add_argument('--output', type=Path, default=Path.home() / '.local/share/qrxosone/amigaos3.fs-uae')
    ap.add_argument('--dry-run', action='store_true', help='generate and verify config without FS-UAE')
    a = ap.parse_args(argv)
    try:
        rom = clean(a.kickstart)
        system = clean(a.system_dir)
        if not a.kickstart.is_file() or a.kickstart.stat().st_size not in (256 * 1024, 512 * 1024, 1024 * 1024):
            raise ValueError('Kickstart ROM must exist and be 256 KiB, 512 KiB or 1 MiB')
        if not a.system_dir.is_dir() or not any((a.system_dir / p).is_file() for p in ('S/Startup-Sequence', 'S/startup-sequence')):
            raise ValueError('system directory must contain an installed S/Startup-Sequence')
        share = clean(a.share) if a.share is not None else None
        if share and not a.share.is_dir():
            raise ValueError('--share directory does not exist')
        out = a.output.expanduser()
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists() and out.is_symlink():
            raise ValueError('refusing symlink output configuration')
        # Real compatible Amiga hardware virtualized by FS-UAE. This is an
        # intentional single-CPU fallback for AmigaOS 3.x applications that need
        # Exec/DOS/intuition.library/custom-chip hardware and cannot run in QRX firmware.
        cfg = ('[fs-uae]\n'
               'amiga_model = A4000/040\n'
               f'cpu = {a.cpu}\n'
               f'kickstart_file = {rom}\n'
               'chip_memory = 2048\n'
               'motherboard_ram = 16384\n'
               'zorro_iii_memory = 131072\n'
               'graphics_card = uaegfx\n'
               'graphics_memory = 65536\n'
               'uae_gfxcard_size = 64\n'
               'floppy_drive_count = 0\n'
               f'hard_drive_0 = {system}\n'
               'hard_drive_0_label = SYS\n'
               'hard_drive_0_priority = 6\n'
               'jit_compiler = 0\n'
               'warp_mode = 0\n'
               'uae_mmu_model = 0\n'
               'fullscreen = 0\n'
               'automatic_input_grab = 0\n')
        if share:
            cfg += f'hard_drive_1 = {share}\nhard_drive_1_label = QRXSHARE\nhard_drive_1_priority = -128\n'
        out.write_text(cfg, encoding='utf-8')
        print(f'Generated AmigaOS3.x compatibility configuration: {out}', flush=True)
        print('Real AmigaOS3.x/Exec/DOS guest in FS-UAE; NOT native QRX68100 or AROS SMP.', flush=True)
        if a.dry_run:
            return 0
        emulator = shutil.which('fs-uae')
        if not emulator:
            raise ValueError('FS-UAE is required to run this AmigaOS 3.x profile')
        return subprocess.call([emulator, str(out)])
    except (OSError, ValueError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
