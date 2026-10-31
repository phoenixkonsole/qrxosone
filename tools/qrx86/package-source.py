#!/usr/bin/env python3
"""Package cumulative experimental source without altering historical release hashes."""
import argparse,hashlib,json,subprocess,zipfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
component=Path('qrx-os-one/qrx-amiga68k-live-v0.20.3.7.4.6.6.22.5.11-amiga-launcher-jit-selftest')
files=[r/'README.md']
for directory in [component,Path('docs'),Path('tools/qrx86'),Path('benchmarks/jit-2026-10-30'),Path('benchmarks/jit-2026-10-31')]:
 files.extend(p for p in (r/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
files=sorted(set(files));manifest=''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(r))+'\n' for p in files)
(a.output/'EXPERIMENTAL-SOURCE-SHA256SUMS').write_text(manifest)
changes=subprocess.check_output(['git','diff','--name-status','HEAD'],cwd=r,text=True)
(a.output/'CHANGE-JOURNAL.txt').write_text('Base '+subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True)+'\nTracked changes:\n'+changes+'\nAdditional files are listed in EXPERIMENTAL-SOURCE-SHA256SUMS. Vendor inputs and historical release ZIPs are excluded and unchanged.\n')
z=a.output/'QRX86-EXPERIMENTAL-CUMULATIVE-SOURCE.zip'
with zipfile.ZipFile(z,'w',zipfile.ZIP_DEFLATED) as f:
 for p in files:f.write(p,str(p.relative_to(r)))
 f.write(a.output/'EXPERIMENTAL-SOURCE-SHA256SUMS','EXPERIMENTAL-SOURCE-SHA256SUMS')
 f.write(a.output/'CHANGE-JOURNAL.txt','CHANGE-JOURNAL.txt')
(a.output/'SHA256SUMS').write_text(hashlib.sha256(z.read_bytes()).hexdigest()+'  '+z.name+'\n')
print(z)
