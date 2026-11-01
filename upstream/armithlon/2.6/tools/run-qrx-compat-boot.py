#!/usr/bin/env python3
"""Actual FS-UAE boot smoke, not synthetic. Fails on missing BOOTED/DONE markers."""
import argparse,json,os,subprocess,time,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--system',type=Path,required=True)
p.add_argument('--rom',type=Path,required=True)
p.add_argument('--ext',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--timeout',type=int,default=180)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
if a.timeout<10 or a.timeout>1200:p.error('timeout must be 10..1200 seconds')
for f in [a.system/'S/Startup-Sequence',a.rom,a.ext]:
    if not f.is_file():p.error(f'Missing file {f}')
config=a.out/'boot.fs-uae.cfg'
config.write_text('[fs-uae]\n'+'amiga_model = A4000/040\n'+'cpu = 68060\n'+
                  f'kickstart_file = {a.rom.resolve()}\n'+f'kickstart_ext_file = {a.ext.resolve()}\n'+
                  'chip_memory = 2048\nzorro_iii_memory = 262144\n'+
                  'graphics_card = uaegfx\nfloppy_drive_count = 0\n'+
                  f'hard_drive_0 = {a.system.resolve()}\n'+'hard_drive_0_label = AROS\nhard_drive_0_priority = 6\n'+
                  f'hard_drive_1 = {a.out.resolve()}\n'+'hard_drive_1_label = QRXOUT\n'+
                  'jit_compiler = 0\nwarp_mode = 0\nuae_mmu_model = 0\nsound_card = none\n')
log=(a.out/'fs-uae.stdout').open('wb')
try:
    proc=subprocess.Popen(['fs-uae',str(config)],stdout=log,stderr=subprocess.STDOUT)
except OSError as exc:
    log.close();p.error(f'FS-UAE unavailable: {exc}')
state='TIMEOUT';start=time.monotonic()
try:
    while time.monotonic()-start<a.timeout:
        if (a.out/'DONE').is_file() and (a.out/'BOOTED').is_file():state='PASS';break
        if proc.poll() is not None:state=f'FSUAE_EXIT_{proc.returncode}';break
        time.sleep(.25)
finally:
    if proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=8)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    log.close()
(a.out/'STATUS').write_text(state+'\n')
print('FS-UAE AROS+QRX HUNK boot test:',state, 'log:',a.out)
sys.exit(0 if state=='PASS' else 1)
