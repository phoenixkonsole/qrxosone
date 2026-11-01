#!/usr/bin/env python3
"""Prepare *real* AROS 68k system tree + QRX 22.5.12 HUNK tools for FS-UAE.
Not an ARMITHLON native QRX68100 boot and not a complete QRX OS One distribution.
Requires an ISO extracted with xorriso. Never alters downloaded source/ISO.
"""
import argparse,hashlib,json,shutil,subprocess,sys,zipfile
from pathlib import Path
EXPECTED_HUNK='e0d79439711cfeb6b44ec43e5f50b60c94dedec6ba1b7ab9e77f54e4cf6eccaf'
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def stage_system(source,dest):
    if dest.exists():raise RuntimeError(f'Output already exists, will not overwrite: {dest}')
    subprocess.run(['xorriso','-osirrox','on','-indev',str(source),'-extract','/',str(dest)],
                   check=True,stdout=subprocess.DEVNULL)
    if (dest/'S/Startup-Sequence').is_file():return dest
    matches=list(dest.rglob('S/Startup-Sequence'))
    if len(matches)!=1:raise RuntimeError(f'Expected one AROS Startup-Sequence in extracted ISO, found {len(matches)}')
    return matches[0].parent.parent

def install(system,archive):
    if digest(archive)!=EXPECTED_HUNK:raise RuntimeError('22.5.12 HUNK archive SHA256 mismatch; fail closed')
    installs=[]
    with zipfile.ZipFile(archive) as z:
        for n in z.namelist():
            p=Path(n)
            if n.endswith('/') or '..' in p.parts:continue
            # Archive has a version-specific top-level folder, with C and Tests.
            idx=next((i for i,q in enumerate(p.parts) if q=='C'),None)
            if idx is None or idx+1>=len(p.parts):continue
            if len(p.parts[idx+1:])!=1:continue
            target=system/'C'/p.parts[-1];target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(z.read(n));target.chmod(0o755);installs.append(target.name)
    if not any(v in installs for v in ('qrx-win32','qrx-i386','amigarun','QRXAmigaRun')):
        raise RuntimeError('QRX HUNK zip did not expose recognizable executables under C/')
    return installs

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso',type=Path,required=True)
    ap.add_argument('--hunk-zip',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();a.iso=a.iso.resolve();a.hunk_zip=a.hunk_zip.resolve();a.out=a.out.resolve()
    if not a.iso.is_file() or not a.hunk_zip.is_file():ap.error('Need verified AROS ISO and HUNK zip')
    a.out.mkdir(parents=True,exist_ok=True)
    system=stage_system(a.iso,a.out/'aros-system')
    tools=install(system,a.hunk_zip)
    # AROS startup is preserved; one added User-Startup runs after the real OS startup.
    old=system/'S/User-Startup'
    if old.exists():shutil.copy2(old,a.out/'ORIGINAL-USER-STARTUP')
    with old.open('a') as f:
        f.write('\n; ARMITHLON 1.3 AROS + QRX 22.5.12 HUNK-tool smoke (not native QRX boot)\n')
        f.write('Echo QRX-TOOLS-BOOTED >QRXOUT:BOOTED\n')
        f.write('Version >QRXOUT:aros-version.txt\n')
        f.write('Echo QRX-HUNK-22.5.12 >QRXOUT:DONE\n')
    manifest={'source':'AROS Deadwood m68k ISO + QRX OS One 22.5.12 m68k HUNK tools',
              'base_iso_sha256':digest(a.iso),'qrx_hunk_sha256':digest(a.hunk_zip),
              'installed_tools':tools,'system_root':str(system),'guest_boot_type':'FS-UAE AROS + QRX tools, not native ARMITHLON QRX68100'}
    (a.out/'boot-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Staged AROS + QRX tools:',system,'count',len(tools))
    return 0
if __name__=='__main__':sys.exit(main())
