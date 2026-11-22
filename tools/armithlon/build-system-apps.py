#!/usr/bin/env python3
"""Rebuild shipped QRX applications with their shared, matching theme."""
from pathlib import Path
import shutil
import subprocess
root=Path(__file__).resolve().parents[2]
orig=next((root/'qrx-os-one').glob('*/diagnostics-v6.6/tools/qrx_unified_apps.c'))
out=root/'.build/qrx-apps';out.mkdir(parents=True,exist_ok=True)
sdk=root/'.build/qrx-sdk/bin'
# The settings panel opens the desktop's own look settings, IPFS, and Wi-Fi and Bluetooth of the
# Raspberry Pi appliance (C:QRXWireless; elsewhere it says that the host has them).
text=orig.read_text()
old='''btn(r,242,168,205,"SYSTEM / BOOT");wrap(r,3,20,225,'''
new='''btn(r,242,168,205,"SYSTEM / BOOT");btn(r,20,204,427,"DESKTOP LOOK: WALLPAPER AND POINTER");btn(r,20,240,205,"IPFS: NETZ UND PINS");btn(r,242,240,205,"WI-FI & BLUETOOTH");wrap(r,3,20,278,'''
assert text.count(old)==1
text=text.replace(old,new)
old='''else if(y>=168&&y<=189)c=x<230?"SYS:Prefs/Locale":"SYS:Prefs/Boot";if(c)'''
new='''else if(y>=168&&y<=189)c=x<230?"SYS:Prefs/Locale":"SYS:Prefs/Boot";else if(y>=204&&y<=225)c="SYS:QRX/C/QRXDesktopPrefs";else if(y>=240&&y<=261)c=x<230?"SYS:QRX/C/QRXIPFSPrefs":"C:QRXWireless";if(c)'''
assert text.count(old)==1
text=text.replace(old,new)
src=out/'qrx_unified_apps.c'
src.write_text(text)
for h in orig.parent.glob('*.h'):
 (out/h.name).write_bytes(h.read_bytes())
for app,flag in [('QRXWelcome','WELCOME'),('QRXWriter','WRITER'),('QRXSpreadsheet','SHEET'),('QRXPresenter','PRESENTER'),('QRXHardware','HARDWARE'),('QRXSettings','SETTINGS')]:
 subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC','-DAPP_'+flag,str(src),'-o',str(out/(app+'.elf'))],check=True)
 subprocess.run([str(sdk/'elf2hunk'),str(out/(app+'.elf')),str(out/app)],check=True)
# IPFS settings is a separate native panel; it reads the P2P status file and
# writes the auto-fetch switch consumed by QRXIPFSD at its next start.
subprocess.run([str(sdk/'aros-gcc'),'-O2','-m68040','-fno-pic','-fno-PIC',str(root/'tools/armithlon/desktop/QRXIPFSPrefs.c'),'-o',str(out/'QRXIPFSPrefs.elf')],check=True)
subprocess.run([str(sdk/'elf2hunk'),str(out/'QRXIPFSPrefs.elf'),str(out/'QRXIPFSPrefs')],check=True)
system=root/'.build/bundle/SystemFull/QRX/C'
if system.is_dir():
 shutil.copy(out/'QRXSettings',system/'QRXSettings')
 shutil.copy(out/'QRXIPFSPrefs',system/'QRXIPFSPrefs')
