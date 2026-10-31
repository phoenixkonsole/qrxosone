#!/usr/bin/env python3
"""Fresh FS-UAE/AROS Windows PrBoom runs measured by host monotonic markers.

This measures emulator wall FPS, never physical Amiga throughput. Guest rc139
is accepted only with the exact budget, explicit budget fault and positive frames.
"""
import argparse,hashlib,json,os,re,shutil,subprocess,time,sys
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for n in ['system','sdk','game','output']:ap.add_argument(n,type=Path)
    ap.add_argument('runtimes',type=Path,nargs='+')
    ap.add_argument('--capture-frame',type=int,default=10)
    ap.add_argument('--no-wipe',action='store_true',help='disable time-dependent Doom melt for deterministic captured frames')
    ap.add_argument('--guest-env',action='append',default=[],metavar='QRX_NAME=VALUE',help='explicit experimental guest settings; recorded in results')
    ap.add_argument('--jit',choices=['off','experimental'],default='off')
    ap.add_argument('--require-jit-features',type=lambda v:int(v,0),default=0,help='require a compiled/runtime QRX86 feature mask in the guest log')
    ap.add_argument('--fsuae-jit',choices=['off','on'],default='on' if os.environ.get('QRX_FSUAE_JIT')=='1' else 'off',help='FS-UAE host m68k JIT; default follows QRX_FSUAE_JIT; independent of QRX86 --jit')
    ap.add_argument('--budget',type=int,default=100_000_000)
    ap.add_argument('--runs',type=int,default=1)
    ap.add_argument('--cpu',choices=['68020','68030','68040','68060'],default='68060')
    ap.add_argument('--timeout',type=int,default=900)
    a=ap.parse_args()
    for setting in a.guest_env:
        if not re.fullmatch(r'QRX[A-Z0-9_]*=[A-Za-z0-9_.+-]+',setting):ap.error('guest settings require QRX names and simple literal values')
    guest_env_lines=''.join('SetEnv '+v.replace('=',' ',1)+'\n' for v in a.guest_env)
    if a.capture_frame<1:ap.error("positive capture frame required")
    if a.runs<1 or a.budget<100_000_000:ap.error('positive runs and budget >=100M required')
    for n in ['system','sdk','game','output']:setattr(a,n,getattr(a,n).resolve())
    a.runtimes=[p.resolve() for p in a.runtimes]
    a.output.mkdir();(a.output/'tmp').mkdir()
    (a.output/'configuration.json').write_text(json.dumps(dict(argv=sys.argv,environment={n:os.environ.get(n) for n in ['QRX_FSUAE_BIN','QRX_FSUAE_EXTRA','QRX_FSUAE_WARP','QRX_FSUAE_MMU']}),indent=2)+'\n')
    repo=Path(__file__).resolve().parents[2]
    root=repo/'qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest'
    harness=root/'amiga-runtime-22.5.11/emulator/run-fsuae-scenario.sh'
    subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=a.sdk,check=True,stdout=(a.output/'input-checksums.log').open('w'))
    names=['prboom-plus.exe','prboom-plus.wad','freedoom1.wad','tranmap.dat']
    inputs={n:sha(a.game/n) for n in names}
    if (a.game/'tranmap.dat').stat().st_size!=65793:raise RuntimeError('incomplete tranmap')
    rows=[]
    for repeat in range(a.runs):
        order=a.runtimes[repeat%len(a.runtimes):]+a.runtimes[:repeat%len(a.runtimes)]
        for runtime in order:
            run=a.output/f'{a.runtimes.index(runtime)}-{repeat}';run.mkdir()
            scn=run/'scenario';overlay=scn/'overlay';(overlay/'C').mkdir(parents=True);game=overlay/'X86/Doom';game.mkdir(parents=True)
            shutil.copy2(runtime,overlay/'C/qrx-win32')
            for n in names:shutil.copy2(a.game/n,game/n)
            game_config='render_wipescreen 0\n' if a.no_wipe else None
            if game_config:(game/'qrx-benchmark.cfg').write_text(game_config)
            game_args=' -config qrx-benchmark.cfg' if a.no_wipe else ''
            (overlay/'X86/input.txt').write_text(f'wait {a.budget//20000}\nsnap frame.rgb\nwait 1000000000\n')
            (scn/'User-Startup').write_text(f'''FailAt 1000
SetEnv QRX_WIN_STATS 1
SetEnv QRX_WIN_BUDGET {a.budget}
SetEnv QRX_HL_IPMS 10000
SetEnv QRX_DX_RTG 1
SetEnv QRX86_JIT {a.jit}
{guest_env_lines}CD SYS:X86/Doom
Echo START >QRXOUT:START
C:qrx-win32 -G QRXOUT:frame.rgb {a.capture_frame} -o QRXOUT: -L QRXOUT:runtime.log -e SDL_VIDEODRIVER=directx SYS:X86/Doom/prboom-plus.exe -iwad freedoom1.wad -width 320 -height 200 -fullscreen -nosound -timedemo demo1{game_args} >QRXOUT:stdout.txt
Echo "guest rc=$RC" >QRXOUT:guest.rc
Echo END >QRXOUT:END
Echo done >QRXOUT:DONE
''')
            out=run/'output';env=dict(os.environ,QRX_SCENARIO_TMPDIR=str(a.output/'tmp'),QRX_BOOT_RETRIES='0',QRX_FSUAE_JIT='1' if a.fsuae_jit=='on' else '0')
            cmd=['bash',str(harness),str(a.system),str(a.sdk/'aros-amiga-m68k-rom.bin'),str(a.sdk/'aros-amiga-m68k-ext.bin'),str(scn),str(out),a.cpu,str(a.timeout)]
            start=end=None;deadline=time.monotonic()+a.timeout+60
            with (run/'harness.log').open('w') as log:
                process=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
                while process.poll() is None:
                    now=time.perf_counter()
                    if start is None and (out/'START').exists():start=now
                    if end is None and (out/'END').exists():end=now
                    if time.monotonic()>deadline:
                        process.terminate();process.wait(timeout=15);raise RuntimeError(f'harness timeout: {run}')
                    time.sleep(.01)
            if process.returncode or not start or not end or (out/'STATUS').read_text().strip()!='COMPLETED':raise RuntimeError(f'incomplete run: {run}')
            text=(out/'runtime.log').read_text(errors='replace')+(out/'stdout.txt').read_text(errors='replace')
            stats=re.search(r'(\d+) guest instructions in (\d+) ms .*?, (\d+) DirectDraw frames',text)
            if not stats or int(stats[1])!=a.budget or int(stats[3])<=0 or 'instruction budget exhausted' not in text or 'unimplemented API called' in text or (out/'guest.rc').read_text().strip()!='guest rc=139':raise RuntimeError(f'invalid guest result: {run}')
            if a.jit=='experimental' and not re.search(r'native_insns=[1-9][0-9]*',text):raise RuntimeError(f'JIT did not execute: {run}')
            feature_match=re.search(r'QRX86-JIT-FEATURES mask=(\d+)',text)
            features=int(feature_match[1]) if feature_match else None
            if a.require_jit_features and (features is None or features&a.require_jit_features!=a.require_jit_features):raise RuntimeError(f'JIT feature mismatch: {run}')
            emulator_log=(out/'fs-uae.log.txt').read_text(errors='replace')
            enabled=bool(re.search(r'JIT=CPU(?:/FPU)?=[1-9][0-9]*',emulator_log))
            if enabled != (a.fsuae_jit=='on'):raise RuntimeError(f'FS-UAE JIT mode mismatch: {run}')
            emulator=shutil.which(os.environ.get('QRX_FSUAE_BIN','fs-uae'))
            row=dict(jit_features=features,guest_env=a.guest_env,game_config=game_config,fsuae_jit=a.fsuae_jit,fsuae_binary=emulator,fsuae_sha256=sha(Path(emulator)),jit=a.jit,runtime=str(runtime),runtime_sha256=sha(runtime),repeat=repeat,cpu=a.cpu,backend="AROS RTG",instructions=int(stats[1]),frames=int(stats[3]),backend_ms=int(stats[2]),wall_seconds=end-start,wall_fps=int(stats[3])/(end-start),capture_frame=a.capture_frame,frame_sha256=sha(out/'frame.rgb') if (out/'frame.rgb').exists() else None)
            rows.append(row);(a.output/'results.json').write_text(json.dumps(dict(inputs=inputs,runs=rows),indent=2)+'\n');print(json.dumps(row),flush=True)
    if any(not r['frame_sha256'] for r in rows) or len({r['frame_sha256'] for r in rows})!=1:raise RuntimeError('snapshot missing/different; do not compare performance')
if __name__=='__main__':main()
