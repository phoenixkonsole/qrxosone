#!/usr/bin/env python3
"""QRX68 safe explicit batch characterization of OWNED AmigaDOS HUNK programs.

No executable is run on the host natively. QRX68 uses a bounded CPU and ABI
sandbox, disabled host FS, hard step budget and subprocess timeout. File names,
exit codes, and potential LVO locations are diagnostic observations, not claims
of full application compatibility.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path


def scan(folder: Path, runtime: Path, budget: int, timeout: float):
    records = []
    for item in sorted(folder.iterdir()):
        if len(records) >= 500:
            break
        if item.is_symlink() or not item.is_file() or not 20 <= item.stat().st_size <= 1024 * 1024:
            continue
        with item.open('rb') as f:
            header = f.read(4)
        if header != bytes.fromhex('000003f3'):
            continue
        cmd = [str(runtime), '--budget', str(budget), str(item)]
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            raw = p.stderr.strip()
            status = 'PASS' if p.returncode == 0 else (
                'UNSUPPORTED_LVO' if 'unsupported lib=' in raw else
                'STEP_BUDGET' if 'QRX68: BUDGET' in raw else 'GUEST_FAULT')
        except subprocess.TimeoutExpired:
            raw = 'subprocess timed out'
            status = 'TIMEOUT'
        m = re.search(r'unsupported lib=(\d+) LVO=-(\d+)', raw)
        records.append(dict(filename=item.name, size=item.stat().st_size,
                            status=status, error=raw[-600:],
                            missing_library_id=int(m[1]) if m else None,
                            missing_lvo=int(m[2]) if m else None))
    return records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('folder', type=Path)
    p.add_argument('--runtime', required=True, type=Path)
    p.add_argument('--out', type=Path, default=Path('reports/qrx68-compat.json'))
    p.add_argument('--budget', type=int, default=200000)
    p.add_argument('--timeout', type=float, default=5.0)
    a = p.parse_args()
    if not a.folder.is_dir() or a.folder.is_symlink():
        p.error('folder must be a real, local directory')
    if not a.runtime.is_file():
        p.error('runtime not built')
    if not 100 <= a.budget <= 2_000_000 or not .1 <= a.timeout <= 30:
        p.error('budget or timeout out of bounds')
    r = scan(a.folder.resolve(), a.runtime.resolve(), a.budget, a.timeout)
    counts = {state: sum(x['status'] == state for x in r)
              for state in ('PASS', 'UNSUPPORTED_LVO', 'STEP_BUDGET', 'GUEST_FAULT', 'TIMEOUT')}
    payload = dict(schema='QRX68-compat-scan-1', backend='portable-limited-68k',
                   caveat='PASS means the selected executable completed in the isolated sandbox;'
                          ' NOT equivalent to full AmigaOS 3.x usability',
                   samples=len(r), status_counts=counts, files=r)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    tmp = a.out.with_suffix(a.out.suffix + '.tmp')
    tmp.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')
    tmp.replace(a.out)
    print('QRX68 compatibility scan:', len(r), 'HUNK files,', counts, '->', a.out)


if __name__ == '__main__':
    main()
