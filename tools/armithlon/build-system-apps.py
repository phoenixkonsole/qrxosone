#!/usr/bin/env python3
"""Rebuild shipped QRX applications with their shared, matching theme."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
src=next((root/'qrx-os-one').glob('*/diagnostics-v6.6/tools/qrx_unified_apps.c'))
out=root/'.build/qrx-apps';out.mkdir(parents=True,exist_ok=True)
sdk=root/'.build/aros-sdk/bin'
for app,flag in [('QRXWelcome','WELCOME'),('QRXWriter','WRITER'),('QRXSpreadsheet','SHEET'),('QRXPresenter','PRESENTER'),('QRXHardware','HARDWARE'),('QRXSettings','SETTINGS')]:
 subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-DAPP_'+flag,str(src),'-o',str(out/(app+'.elf'))],check=True)
 subprocess.run([str(sdk/'elf2hunk'),str(out/(app+'.elf')),str(out/app)],check=True)
