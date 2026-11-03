#!/usr/bin/env python3
"""Measure Windows PrBoom under host qrx86 using wall time, not its virtual clock.

Prepare the game directory with prboom-plus.exe, prboom-plus.wad, freedoom1.wad
and a completed tranmap.dat first. Results are HOST measurements, not m68k FPS.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import statistics
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game_dir', type=Path)
    ap.add_argument('output_dir', type=Path)
    ap.add_argument('runtimes', type=Path, nargs='+')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--budget', type=int, default=1_000_000_000)
    args = ap.parse_args()
    if args.runs < 1 or args.budget < 100_000_000:
        ap.error('runs must be positive and budget at least 100 million')
    game = args.game_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {name: digest(game / name) for name in
              ('prboom-plus.exe', 'prboom-plus.wad', 'freedoom1.wad', 'tranmap.dat')}
    if (game / 'tranmap.dat').stat().st_size != 65793:
        ap.error('tranmap.dat is incomplete; finish the initial table generation')
    records = []
    runtimes = [p.resolve() for p in args.runtimes]
    # Rotate order to reduce bias from host load and CPU frequency drift.
    for repeat in range(args.runs):
        order = runtimes[repeat % len(runtimes):] + runtimes[:repeat % len(runtimes)]
        for runtime in order:
            run = out / f'{runtimes.index(runtime)}-{repeat}'
            run.mkdir()
            # Each guest gets fresh defaults; one run must not write the next
            # run's configuration. File copying is outside the timed interval.
            sandbox = run / 'game'
            sandbox.mkdir()
            for name in inputs:
                shutil.copyfile(game / name, sandbox / name)
            script = run / 'input.txt'
            script.write_text(f'wait {args.budget // 20000}\nsnap frame.ppm\nwait 1000000000\n')
            cmd = [str(runtime), '-s', str(script), '-o', str(run), '-e',
                   'SDL_VIDEODRIVER=directx', str(sandbox / 'prboom-plus.exe'),
                   '-iwad', 'freedoom1.wad', '-width', '320', '-height', '200',
                   '-fullscreen', '-nosound', '-timedemo', 'demo1']
            env = dict(os.environ, QRX_WIN_STATS='1', QRX_WIN_BUDGET=str(args.budget),
                       QRX_HL_IPMS='10000', QRX_DX_RTG='1')
            start = time.perf_counter()
            cpu_start = resource.getrusage(resource.RUSAGE_CHILDREN)
            with (run / 'stdout.txt').open('wb') as stdout, (run / 'stderr.txt').open('wb') as stderr:
                result = subprocess.run(cmd, cwd=sandbox, env=env, stdout=stdout,
                                        stderr=stderr, timeout=600)
            elapsed = time.perf_counter() - start
            cpu_end = resource.getrusage(resource.RUSAGE_CHILDREN)
            cpu_seconds = cpu_end.ru_utime + cpu_end.ru_stime - cpu_start.ru_utime - cpu_start.ru_stime
            log = (run / 'stderr.txt').read_text(errors='replace')
            stats = re.search(r'(\d+) guest instructions in (\d+) ms .*?, (\d+) DirectDraw frames', log)
            if (result.returncode != 139 or not stats or int(stats[1]) != args.budget
                    or int(stats[3]) <= 0 or 'instruction budget exhausted' not in log
                    or 'unimplemented API called' in log):
                raise RuntimeError(f'invalid benchmark run: inspect {run}')
            record = dict(runtime=str(runtime), runtime_sha256=digest(runtime),
                          repeat=repeat, instructions=int(stats[1]), frames=int(stats[3]),
                          virtual_ms=int(stats[2]), wall_seconds=elapsed,
                          cpu_seconds=cpu_seconds,
                          wall_fps=int(stats[3]) / elapsed, exit_code=result.returncode,
                          frame_sha256=digest(run / 'frame.ppm'))
            records.append(record)
            (out / 'results.json').write_text(json.dumps(dict(inputs=inputs, runs=records), indent=2) + '\n')
            print(json.dumps(record), flush=True)
    for runtime in runtimes:
        rows = [r for r in records if r['runtime'] == str(runtime)]
        print(f'{runtime}: median {statistics.median(r["wall_fps"] for r in rows):.3f} host FPS')
    if len({(r['frames'], r['frame_sha256']) for r in records}) != 1:
        raise RuntimeError('frame count or snapshot differs; do not compare performance')


if __name__ == '__main__':
    main()
