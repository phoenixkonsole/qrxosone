#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT. Real-process benchmark collector; no invented FPS.

Runs QRX3D guest MMIO, OpenGL 1.x HLE, reference CPU and, when available,
actual offscreen Metal/Vulkan graphics. Skips unimplemented APIs explicitly.
Never mixes command throughput with GPU-rendering frame rates.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
W, H = 800, 450

def run_json(label, args, timeout=150):
    try:
        p = subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True,
                           text=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"api": label, "status": "SKIP" if isinstance(exc, OSError) else "ERROR",
                "reason": str(exc)}
    records = [ln for ln in p.stdout.splitlines() if ln.startswith('{')]
    if p.returncode or not records:
        return {"api": label, "status": "ERROR", "exit_code": p.returncode,
                "stderr": p.stderr[-600:], "stdout": p.stdout[-600:]}
    try:
        result = json.loads(records[-1])
        if result.get("status") != "PASS":
            raise ValueError("Benchmark did not report PASS")
        if result.get("frames", 0) < 1 or result.get("fps", 0) <= 0:
            raise ValueError("Missing real timing")
        return result
    except (ValueError, json.JSONDecodeError) as exc:
        return {"api": label, "status": "ERROR", "reason": str(exc)}

def read_ppm(path):
    with path.open('rb') as f:
        if f.readline().strip() != b'P6':
            raise ValueError('Image is not binary PPM')
        dim = f.readline().strip()
        if dim != b'800 450' or f.readline().strip() != b'255':
            raise ValueError('Unexpected image format or dimensions')
        pixels = f.read()
        if len(pixels) != W * H * 3:
            raise ValueError('Truncated/oversized PPM')
        return pixels

def compare(reference, actual):
    a = read_ppm(reference)
    b = read_ppm(actual)
    # A full image diff catches invisible geometry and wrong API byte-order.
    abs_diff = sum(abs(x-y) for x, y in zip(a, b))
    threshold = 25.0  # tolerate small shader/raster differences, not missing scene
    mae = abs_diff / len(a)
    background = a[:3]
    occupied = sum(1 for i in range(0, len(b), 3)
                   if sum(abs(b[i+j]-background[j]) for j in range(3)) > 30)
    return {'mae_rgb': round(mae, 3), 'occupied_pixels': occupied,
            'image_check': 'PASS' if mae < threshold and occupied > 20000 else 'FAIL'}

def md_report(data, output):
    rows = ["# ARMITHLON 1.0 – Benchmark-Ergebnisse", "",
            f"Zeitpunkt: {data['timestamp_utc']} | Host: {data['platform']}", "",
            "Auflösung: 800 × 450; 240 deckende + 16 transparente Dreiecke; 1 Clear pro Frame.",
            "", "| API | Messart | Frames | FPS | Status |", "|---|---|---:|---:|---|"]
    for r in data['results']:
        rows.append('| {} | {} | {} | {} | {} |'.format(r.get('api','?'),
            r.get('kind','—'),r.get('frames','—'),
            f"{r['fps']:.1f}" if 'fps' in r else '—',r.get('status','?')))
        if r.get('status') != 'PASS':
            rows.append(f"\n> {r.get('api')}: {r.get('reason', r.get('stderr', 'Nicht verfügbar'))}\n")
    rows += ["", "**Wichtig:** Encoder-FPS, CPU-Referenz-FPS und native GPU-FPS sind verschiedene Messgrößen.",
             "QRX86-OpenGL-HLE wird hier auf Hostseite gemessen, nicht als komplette emulierte Windows-Anwendung.",
             "Metal/Vulkan-Werte umfassen CPU-Submit und GPU-Wartezeit (offscreen), keine Display-VSync.",
             "Direct3D9 läuft ausschließlich als separates natives Windows-Referenzprogramm; kein QRX86-D3D9 vorhanden.",
             "3dfx = QRX3D-MMIO im Stil von Glide, keine originale Glide-DLL.", ""]
    output.write_text('\n'.join(rows), encoding='utf-8')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--warmup', type=int, default=8)
    parser.add_argument('--out', type=Path, default=ROOT / 'reports' / 'latest')
    args = parser.parse_args()
    if args.frames < 2 or args.frames > 2000 or args.warmup < 0 or args.warmup > 500:
        parser.error('frames must be 2..2000, warmup 0..500')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = ROOT / 'build-qrx68100'
    ref = out / 'qrx-reference.ppm'
    results = []
    cpu = run_json('cpu', [exe / 'qrx_bench_api', 'cpu', '--frames', args.frames,
                           '--warmup', args.warmup, '--preview', ref])
    results.append(cpu)
    for name in ['glide-mmio', 'opengl-hle']:
        results.append(run_json(name, [exe / 'qrx_bench_api', name, '--frames',
                                args.frames, '--warmup', args.warmup]))
    metal = exe / 'qrx_bench_metal'
    if platform.system() == 'Darwin' and metal.is_file():
        result = run_json('metal', [metal, args.frames, args.warmup,
                                   out / 'metal-gpu.ppm'])
        if result.get('status') == 'PASS':
            try:
                result.update(compare(ref, out / 'metal-gpu.ppm'))
                if result['image_check'] != 'PASS':result['status'] = 'FAIL_IMAGE'
            except (ValueError, OSError) as exc:
                result['status'] = 'FAIL_IMAGE';result['reason'] = str(exc)
        results.append(result)
    else:
        results.append({'api': 'metal', 'status': 'SKIP',
                        'reason': 'macOS mit erfolgreich kompiliertem Metal-Backend erforderlich'})
    vulkan = exe / 'qrx_vulkan_3d'
    if vulkan.is_file():
        result = run_json('vulkan', [vulkan, '--bench', out / 'vulkan-gpu.ppm',
                                     args.frames, args.warmup])
        if result.get('status') == 'PASS':
            try:
                result.update(compare(ref, out / 'vulkan-gpu.ppm'))
                if result['image_check'] != 'PASS':result['status']='FAIL_IMAGE'
            except (ValueError, OSError) as exc:
                result['status'] = 'FAIL_IMAGE';result['reason'] = str(exc)
        results.append(result)
    else:
        results.append({'api': 'vulkan', 'status': 'SKIP',
                        'reason': 'Vulkan SDK + glslangValidator + GPU-Treiber erforderlich'})
    results.append({'api':'direct3d9-native', 'status':'SKIP',
                    'reason':'Separates Windows/Direct3D9-Programm in benchmarks/direct3d9; QRX86 D3D9-HLE fehlt'})
    results.append({'api':'warp3d', 'status':'SKIP',
                    'reason':'Keine Warp3D-Bibliotheksanbindung in ARMITHLON; QRX3D ist nicht binärkompatibel'})
    results.append({'api':'glide2x.dll', 'status':'SKIP',
                    'reason':'Originale 3dfx Glide-DLL nicht implementiert; glide-mmio testet nur QRX3D-Adapter'})
    report = {'schema_version': 1, 'benchmark': 'qrx3d-grid-v1',
              'timestamp_utc':datetime.now(timezone.utc).isoformat(),
              'platform':f"{platform.system()} {platform.machine()} {platform.release()}",
              'python':sys.version.split()[0],
              'scene':{'width':W,'height':H,'opaque_triangles':240,
                       'blended_triangles':16,'frames':args.frames,'warmup':args.warmup},
              'results':results}
    (out / 'results.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    md_report(report,out/'REPORT.md')
    for result in results:
        name=result['api']
        if result.get('status')=='PASS':
            print(f"{name:18} {result['fps']:12.2f} frames/s  {result.get('kind','')}")
        else:print(f"{name:18} {result.get('status','?'):>12}  {result.get('reason','')}")
    print('JSON:',out/'results.json')
    print('Report:',out/'REPORT.md')
    return int(any(r['status'] not in {'PASS','SKIP'} for r in results))
if __name__ == '__main__':sys.exit(main())
