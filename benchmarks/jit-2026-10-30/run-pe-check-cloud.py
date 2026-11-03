import subprocess,os,time,json,pathlib,hashlib
out=pathlib.Path('/workspace/qrx-jit-work/pe-current');out.mkdir(exist_ok=True)
rows=[]
for i in range(3):
 for mode in (['interpreter-final','jit-final'] if i%2==0 else ['jit-final','interpreter-final']):
  t=time.perf_counter();p=subprocess.run(['/workspace/qrx-jit-work/wrappers/'+mode,'-L',str(out/f'{mode}-{i}.log'),'/workspace/qrx-jit-work/native/jit-pe-loop.exe'],env=dict(os.environ,QRX_WIN_STATS='1',LD_LIBRARY_PATH='/workspace/qrx-tools/root/usr/lib/x86_64-linux-gnu'),stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  s=time.perf_counter()-t;(out/f'{mode}-{i}.stdout').write_bytes(p.stdout)
  assert p.returncode==42,(mode,p.returncode,p.stdout[-1000:])
  row=dict(mode=mode,repeat=i,seconds=s,exit_code=p.returncode);rows.append(row);print(json.dumps(row),flush=True)
(out/'results.json').write_text(json.dumps(rows,indent=2))
