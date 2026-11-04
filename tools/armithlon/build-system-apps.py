#!/usr/bin/env python3
"""Rebuild shipped QRX applications with their shared, matching theme."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
orig=next((root/'qrx-os-one').glob('*/diagnostics-v6.6/tools/qrx_unified_apps.c'))
out=root/'.build/qrx-apps';out.mkdir(parents=True,exist_ok=True)
sdk=root/'.build/qrx-sdk/bin'
# The settings panel opens the desktop's own look settings.
text=orig.read_text()
old='''btn(r,242,168,205,"SYSTEM / BOOT");wrap(r,3,20,225,'''
new='''btn(r,242,168,205,"SYSTEM / BOOT");btn(r,20,204,427,"DESKTOP LOOK: WALLPAPER AND POINTER");wrap(r,3,20,245,'''
assert text.count(old)==1
text=text.replace(old,new)
old='''else if(y>=168&&y<=189)c=x<230?"SYS:Prefs/Locale":"SYS:Prefs/Boot";if(c)'''
new='''else if(y>=168&&y<=189)c=x<230?"SYS:Prefs/Locale":"SYS:Prefs/Boot";else if(y>=204&&y<=225)c="SYS:QRX/C/QRXDesktopPrefs";if(c)'''
assert text.count(old)==1
text=text.replace(old,new)
src=out/'qrx_unified_apps.c'
src.write_text(text)
for h in orig.parent.glob('*.h'):
 (out/h.name).write_bytes(h.read_bytes())
for app,flag in [('QRXWelcome','WELCOME'),('QRXWriter','WRITER'),('QRXSpreadsheet','SHEET'),('QRXPresenter','PRESENTER'),('QRXHardware','HARDWARE'),('QRXSettings','SETTINGS')]:
 subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-DAPP_'+flag,str(src),'-o',str(out/(app+'.elf'))],check=True)
 subprocess.run([str(sdk/'elf2hunk'),str(out/(app+'.elf')),str(out/app)],check=True)
