#!/usr/bin/env python3
"""QRX86 cross-OS benchmark. Zero fabricated machine scores or FPS (MIT).

CPU comparable workload: identical freestanding source+checksum, executed as
PE32/i386 and ELF32/i386 inside QRX86. The native host build is a distinct
non-emulated baseline. PrBoom attempts are reported separately because their
backends and clock/asset choices differ. GPU results retain their own units.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
SLOTS=['486DX2','Pentium','68060','68080','Pentium 4','Core i7','Apple M1']
EXPECTED='903be502'  # verified against native C workload, generated from fixed 300000 iterations
PAT=re.compile(r'QRXBENCH v=1 loops=(\d+) checksum=([0-9a-f]{8})')

def sha(p):
    h=hashlib.sha256()
    with open(p,'rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()

def run(label,command,repeats,timeout,env_extra=None,cwd=None):
    result={'label':label,'status':'SKIP','command':[str(c) for c in command],'runs':[]}
    if not command or not Path(command[0]).is_file():
        result['reason']='Runtime executable missing; compile the selected guest interpreter first'
        return result
    for i in range(repeats):
        env=dict(os.environ)
        if env_extra:env.update(env_extra)
        t=time.perf_counter()
        try:
            r=subprocess.run([str(a) for a in command],cwd=cwd,env=env,text=True,
                             capture_output=True,timeout=timeout,check=False)
        except (OSError,subprocess.TimeoutExpired) as e:
            result.update(status='ERROR',reason=str(e));return result
        elapsed=time.perf_counter()-t
        text=r.stdout+'\n'+r.stderr
        match=PAT.search(text)
        one={'repeat':i,'seconds':round(elapsed,6),'exit_code':r.returncode,
             'stdout':r.stdout[-1200:],'stderr':r.stderr[-1200:]}
        if match:
            one.update(loops=int(match[1]),checksum=match[2],loops_per_second=int(match[1])/elapsed)
        stats=re.search(r'qrx-i386: pid \d+ executed (\d+) guest instructions',text)
        if not stats: stats=re.search(r'(\d+) guest instructions in \d+ ms',text)
        if stats:one['guest_instructions']=int(stats[1]);one['guest_instructions_per_second']=int(stats[1])/elapsed
        if not match or match[2]!=EXPECTED or int(match[1])!=300000 or r.returncode!=0:
            result.update(status='FAIL',reason='Guest return code or fixed checksum mismatched; see captured output')
            result['runs'].append(one);return result
        result['runs'].append(one)
    result['status']='PASS'
    result['median_seconds']=round(statistics.median(x['seconds'] for x in result['runs']),6)
    result['median_loops_per_second']=round(statistics.median(x['loops_per_second'] for x in result['runs']),2)
    if all('guest_instructions_per_second' in x for x in result['runs']):
        result['median_guest_instructions_per_second']=round(statistics.median(x['guest_instructions_per_second'] for x in result['runs']),2)
    result['sha256_runtime']=sha(command[0]);guest=next((Path(a) for a in command[1:] if str(a).endswith(('.exe','.elf'))),None)
    if guest and guest.is_file():result['sha256_guest']=sha(guest)
    return result

def import_history():
    p=ROOT/'reports/reference/qrx-github-doom-2026-10-30.json'
    return json.loads(p.read_text()) if p.exists() else {}

def render_html(data,out):
    entries=data['results']; good=[x for x in entries if x['status']=='PASS']
    hardware={r['machine']:r for r in data.get('hardware_reference',[])}
    rows=[]
    for x in entries:
        val=f"{x['median_loops_per_second']:,.0f}" if x['status']=='PASS' else '—'
        rows.append(f"<tr><td>{html.escape(x['label'])}</td><td>{html.escape(x['status'])}</td><td>{val}</td><td>{x.get('median_seconds','—')}</td></tr>")
    slots=''.join(f'<tr><td>{html.escape(s)}</td><td>{f"{hardware[s]['median_iterations_per_second']:,.0f} Iter./s (gemessen; {html.escape(hardware[s]['measurement_source'])})" if s in hardware else "Noch keine validierte Vergleichsmessung"}</td></tr>' for s in SLOTS)
    gpurows=[]
    for r in data.get('gpu_results',[]):
        label=html.escape(str(r.get('api','?')))
        kind=html.escape(str(r.get('kind','—')))
        status=html.escape(str(r.get('status','?')))
        fps=f"{r['fps']:,.2f}" if isinstance(r.get('fps'),(int,float)) else '—'
        gpurows.append(f'<tr><td>{label}</td><td>{kind}</td><td>{fps}</td><td>{status}</td></tr>')
    bars=''.join(f'<div class="bar"><b>{html.escape(e["label"])}</b><span style="width:{100*e["median_loops_per_second"]/max(q["median_loops_per_second"] for q in good):.1f}%"></span><em>{e["median_loops_per_second"]:,.0f} Iter./s</em></div>' for e in good)
    page=f'''<!doctype html><html lang="de"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>QRX86 Benchmarks – echte Messungen</title><style>
    body{{font:16px system-ui;max-width:1100px;margin:auto;padding:22px;background:#101720;color:#e7edf7}}a{{color:#9fe1fa}}h1{{color:#fff}}small{{color:#9aa9bb}}table{{border-collapse:collapse;width:100%;margin:14px 0 30px}}td,th{{padding:10px;border-bottom:1px solid #344052;text-align:left}}.bar{{display:grid;grid-template-columns:190px 1fr 150px;gap:10px;align-items:center;margin:12px 0}}.bar span{{background:#62d2ac;height:22px;display:block;max-width:100%}}.box{{border:1px solid #344052;padding:15px;margin:18px 0;border-radius:8px}}@media(max-width:650px){{.bar{{grid-template-columns:1fr}}}}
    </style><h1>QRX86 – Windows / Linux Execution</h1><p>Benchmark: 300.000 identische C-Iterationen, Hash-Validierung {EXPECTED}. <strong>Nur gemessene Durchläufe erscheinen als Balken.</strong></p>
    <div class="box"><h2>Vergleichbare Testlast</h2>{bars if bars else 'Noch keine erfolgreich gemessenen Durchläufe.'}</div><h2>Ergebnisse</h2><table><tr><th>Laufzeit</th><th>Status</th><th>Iterationen/s</th><th>Median s</th></tr>{''.join(rows)}</table>
    <div class="box"><h2>Grafik – getrennte Messarten</h2><p>CPU-Software-FPS, OpenGL-HLE/Glide-Kommando-FPS und native Metal/Vulkan-GPU-FPS sind nicht dieselbe Metrik.</p><table><tr><th>API</th><th>Messart</th><th>FPS</th><th>Status</th></tr>{''.join(gpurows)}</table></div>
    <div class="box"><h2>Historische Hardware-Referenzen</h2><p>Für ein seriöses Ranking muss derselbe Algorithmus auf echter Hardware oder einer vollständig beschriebenen Simulation laufen. GHz, Geekbench-Punkte und Doom-FPS aus unterschiedlichen Szenen dürfen nicht umgerechnet werden.</p><table><tr><th>CPU</th><th>Matched-workload-Wert</th></tr>{slots}</table></div>
    <div class="box"><h2>Archivierte PrBoom-Messungen</h2><p>30.10.2026, anderer Testpfad und andere Hosthardware: FS-UAE (68060 emuliert, Host-JIT aus) 43 Frames / 370,45 s = 0,1161 FPS. Xeon 8370C Linux-Host mit QRX86 Win32-HLE: Median 29,94 FPS aus drei 1-Mrd.-Instruktionsbudget-Durchläufen. Diese Werte sind <strong>nicht</strong> direkt vergleichbar.</p></div><small>Erstellt {html.escape(data['timestamp_utc'])}; Rohdaten und Logs siehe execution-results.json. Fehlende Messungen werden nicht geschätzt.</small></html>'''
    out.write_text(page,encoding='utf-8')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--windows-runtime',type=Path,default=ROOT/'build-qrx86-host/qrx-win32')
    p.add_argument('--linux-runtime',type=Path,default=ROOT/'build-qrx86-linux-host/qrx-i386')
    p.add_argument('--native-runtime',type=Path,default=ROOT/'build-benchmarks/qrx-cross-host')
    p.add_argument('--windows-guest',type=Path,default=ROOT/'build-benchmarks/qrx-cross-win32.exe')
    p.add_argument('--linux-guest',type=Path,default=ROOT/'build-benchmarks/qrx-cross-linux-i386.elf')
    p.add_argument('--runs',type=int,default=3)
    p.add_argument('--timeout',type=int,default=120)
    p.add_argument('--out',type=Path,default=ROOT/'reports/latest')
    p.add_argument('--hardware-reference',type=Path,default=ROOT/'reports/reference/hardware-measurements.json')
    p.add_argument('--gpu-results',type=Path,default=ROOT/'reports/latest/results.json')
    a=p.parse_args()
    if not 1<=a.runs<=10 or not 1<=a.timeout<=1800:p.error('runs 1..10; timeout 1..1800')
    a.out.mkdir(parents=True,exist_ok=True)
    results=[]
    for label,cmd,env in [
        ('Native host baseline', [a.native_runtime], {}),
        ('QRX86 Linux ELF32', [a.linux_runtime,a.linux_guest], {'QRX_I386_STATS':'1'}),
        ('QRX86 Windows PE32',[a.windows_runtime,a.windows_guest],{'QRX_WIN_STATS':'1'})]:
        if len(cmd)==2 and not Path(cmd[1]).is_file():
            results.append({'label':label,'status':'SKIP','reason':'Guest binary missing; compile with build-crossbench'});continue
        print('Running',label,flush=True)
        results.append(run(label,cmd,a.runs,a.timeout,env))
    hardware=[]
    if a.hardware_reference.exists():
        ref=json.loads(a.hardware_reference.read_text())
        for row in ref.get('machines',[]):
            if row.get('machine') in SLOTS and row.get('workload')=='qrx-crossbench-v1' and row.get('iterations')==300000 and row.get('checksum')==EXPECTED and row.get('median_seconds',0)>0 and row.get('median_iterations_per_second',0)>0 and row.get('measurement_source'):
                hardware.append(row)
    gpu=[]
    if a.gpu_results.is_file():
        try:gpu=json.loads(a.gpu_results.read_text()).get('results',[])
        except (OSError,ValueError):pass
    evidence={'schema':'qrx86-execution-v1' ,'timestamp_utc':datetime.now(timezone.utc).isoformat(),
              'host':{'platform':platform.platform(),'machine':platform.machine()},
              'workload':{'name':'QRX crossbench C','iterations':300000,'expected_checksum':EXPECTED,
                          'source_sha256':sha(ROOT/'benchmarks/qrx86-cross/crossbench.c')},
              'results':results,'historic_doom':import_history(),'hardware_reference':hardware,'gpu_results':gpu,
              'reference_cpu_slots':[{ 'name':name, 'matched_workload':None} for name in SLOTS]}
    (a.out/'execution-results.json').write_text(json.dumps(evidence,indent=2,ensure_ascii=False)+'\n')
    render_html(evidence,a.out/'execution-dashboard.html')
    for x in results: print(x['label'],x['status'],x.get('median_loops_per_second','—'),x.get('reason',''))
    return int(any(x['status']=='FAIL' for x in results))

if __name__=='__main__':sys.exit(main())
