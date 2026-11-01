#!/usr/bin/env python3
"""PrBoom Windows (DirectDraw) / Linux (SDL/X11) capability+wall-time runner.

Actual executable/WAD assets and two QRX86 host runtimes are required.  No
pretend success or synthetic CPU equivalence.  Different graphics backends may
make results non-comparable; each run records precisely which was attempted.
"""
import argparse,hashlib,json,os,re,subprocess,time,sys
from pathlib import Path
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[1]
WIN_NAMES=('prboom-plus.exe','prboom-plus.wad','freedoom1.wad','tranmap.dat')
def sha(f):return hashlib.sha256(Path(f).read_bytes()).hexdigest()
def test(label,command,cwd,timeout,output,envextra,backend):
    item={'name':label,'backend':backend,'command':list(map(str,command))}
    if not Path(command[0]).is_file():
        return {**item,'status':'SKIP','reason':'QRX86 interpreter missing; compile runtime first'}
    env=os.environ.copy();env.update(envextra)
    start=time.perf_counter()
    try:
        cp=subprocess.run(list(map(str,command)),cwd=cwd,timeout=timeout,
                          capture_output=True,text=True,env=env)
        seconds=time.perf_counter()-start
    except subprocess.TimeoutExpired as exc:
        return {**item,'status':'TIMEOUT','seconds':round(time.perf_counter()-start,3),'reason':str(exc)}
    (output/(label+'.stdout.log')).write_text(cp.stdout)
    (output/(label+'.stderr.log')).write_text(cp.stderr)
    text=cp.stdout+'\n'+cp.stderr
    # PrBoom's internal FPS uses virtual time in headless QRX86; never present as wall FPS.
    m=re.search(r'Timed\s+(\d+)\s+gametics',text)
    frame=re.search(r'(\d+) DirectDraw frames',text)
    done=bool(m)
    result={**item,'status':'PASS' if done else 'UNVERIFIED', 'exit_code':cp.returncode,
            'wall_seconds':round(seconds,4),'guest_sha256':sha(next(a for a in command if str(a).lower().endswith(('.exe','.elf')))),
            'completed_gametics':int(m[1]) if m else None,
            'directdraw_frames':int(frame[1]) if frame else None}
    if done:
        # Gametics/s are NOT render FPS. PrBoom timedemo has 35 tics per game second.
        result['game_tics_per_wall_second']=round(int(m[1])/seconds,3)
        if frame:result['directdraw_frames_per_wall_second']=round(int(frame[1])/seconds,3)
    else:
        result['reason']='No final PrBoom timedemo marker. The runtime may lack guest dependencies, SDL/X11 calls, or supported syscalls.'
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--windows-game',type=Path)
    ap.add_argument('--linux-elf',type=Path)
    ap.add_argument('--freedoom-wad',type=Path)
    ap.add_argument('--linux-root',type=Path)
    ap.add_argument('--windows-runtime',type=Path,default=ROOT/'build-qrx86-host/qrx-win32')
    ap.add_argument('--linux-runtime',type=Path,default=ROOT/'build-qrx86-linux-host/qrx-i386')
    ap.add_argument('--timeout',type=int,default=300)
    ap.add_argument('--out',type=Path,default=ROOT/'reports/latest/prboom')
    a=ap.parse_args()
    if not 10<=a.timeout<=3600:ap.error('timeout 10..3600')
    a.out.mkdir(parents=True,exist_ok=True)
    results=[]
    if a.windows_game is not None:
        game=a.windows_game.resolve()
        absent=[n for n in WIN_NAMES if not (game/n).is_file()]
        if absent:results.append({'name':'prboom_windows','status':'SKIP','reason':'Missing '+', '.join(absent)})
        elif (game/'tranmap.dat').stat().st_size!=65793:
            results.append({'name':'prboom_windows','status':'SKIP','reason':'tranmap.dat incomplete (must be 65793 bytes)'})
        else:
            # Real playback; headless QRX script keeps its simulated clock independent of wall time.
            script=a.out/'prboom.win.script';script.write_text('wait 200000000\n')
            results.append(test('prboom_windows',[a.windows_runtime,'-s',script,
                '-e','SDL_VIDEODRIVER=directx',game/'prboom-plus.exe','-iwad','freedoom1.wad',
                '-width','320','-height','200','-fullscreen','-nosound','-timedemo','demo1'],
                game,a.timeout,a.out,{'QRX_WIN_STATS':'1','QRX86_JIT':'off'},'DirectDraw 320x200x8'))
    else:results.append({'name':'prboom_windows','status':'SKIP','reason':'Pass --windows-game DIR containing audited PrBoom and Freedoom assets'})
    if a.linux_elf and a.freedoom_wad and a.linux_root:
        exe=a.linux_elf.resolve();wad=a.freedoom_wad.resolve();root=a.linux_root.resolve()
        if not exe.is_file() or not wad.is_file() or not root.is_dir():
            results.append({'name':'prboom_linux','status':'SKIP','reason':'Missing guest ELF32, WAD, or dependency rootfs'})
        else:
            results.append(test('prboom_linux',[a.linux_runtime,'-r',root,'-e','DISPLAY=:0',exe,
                '-iwad',wad,'-width','320','-height','200','-nosound','-timedemo','demo1'],
                exe.parent,a.timeout,a.out,{'QRX_I386_STATS':'1'},'SDL1/X11 embedded subset'))
    else:results.append({'name':'prboom_linux','status':'SKIP','reason':'Provide --linux-elf, --linux-root and --freedoom-wad; QRX86 X11/SDL is incomplete'})
    report={'schema':'qrx-prboom-capability-v1','date':datetime.now(timezone.utc).isoformat(),
            'host':os.uname().sysname+' '+os.uname().machine,
            'warning':'Windows DirectDraw and Linux SDL/X11 may exercise different rendering paths. Do not compare FPS until same demo and end-to-end output are verified.',
            'runs':results}
    (a.out/'prboom-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    for v in results:print(v['name'],v['status'],v.get('directdraw_frames_per_wall_second','—'),v.get('reason',''))
    return int(any(v['status'] in ('TIMEOUT','UNVERIFIED') for v in results))
if __name__=='__main__':sys.exit(main())
