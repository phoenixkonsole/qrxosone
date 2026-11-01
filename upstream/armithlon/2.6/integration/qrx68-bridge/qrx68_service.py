#!/usr/bin/env python3
"""QRX68/1 secure directory transport: AROS (QRXDesktop) -> host QRX68 runner.

Untrusted guest input: no arbitrary host paths, arguments or shell commands.
A completed request consists of inbox/<id>.hunk and an atomically published
inbox/<id>.req. Results arrive as results/<id>.out, .err, .ppm, .status.
The `.status` file is published last, atomically. In-flight files survive a
server restart.  No AROS guest execution is claimed by host tests.
"""
from __future__ import annotations
import argparse
import multiprocessing
import os
from pathlib import Path
import re
import resource
import signal
import stat
import subprocess
import sys
import tempfile
import time

ID = re.compile(r'^[A-Za-z0-9_-]{8,48}$')
EVT = re.compile(r'^[A-Za-z0-9_-]{8,48}\.(\d{10})\.evt$')
EVENT_MARKER = 'QRX68/2 '
MAX_INPUT = 64 * 1024 * 1024
MAX_LOG = 1024 * 1024
MARKER = b'QRX68/1\n'


def atomic_publish(path: Path, data: bytes):
    # Pin the result directory by descriptor: guest may modify directory names
    # while a request executes. Never follow a swapped directory symlink.
    dfd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        name = path.name
        tmpname = name + f'.{os.getpid()}.tmp'
        fd = os.open(tmpname, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                     0o600, dir_fd=dfd)
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
        except BaseException:
            try: os.unlink(tmpname, dir_fd=dfd)
            except OSError: pass
            raise
        os.rename(tmpname, name, src_dir_fd=dfd, dst_dir_fd=dfd)
    finally:
        os.close(dfd)


def regular_read(dirfd: int, name: str, limit: int) -> bytes:
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=dirfd)
    try:
        s = os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or s.st_size > limit or s.st_size < 0:
            raise ValueError('not a bounded regular file')
        data = bytearray()
        while len(data) <= limit:
            part = os.read(fd, min(65536, limit + 1 - len(data)))
            if not part:
                break
            data.extend(part)
        if len(data) > limit:
            raise ValueError('file changed or exceeded limit')
        return bytes(data)
    finally:
        os.close(fd)


def check_hunk(data: bytes):
    if len(data) < 20 or len(data) > MAX_INPUT or data[:4] != b'\x00\x00\x03\xf3':
        raise ValueError('not a bounded Amiga HUNK executable')


def setup_spool(root: Path):
    root.mkdir(parents=True, exist_ok=True)
    if root.is_symlink():
        raise ValueError('bridge root cannot be symlink')
    for name in ('inbox', 'results'):
        p = root / name
        p.mkdir(exist_ok=True)
        if p.is_symlink() or not p.is_dir():
            raise ValueError(f'unsafe {name} directory')


def child_limits():
    # Hard cap on emitted files/logs and CPU time, rather than unbounded output.
    resource.setrlimit(resource.RLIMIT_FSIZE, (16 * 1024 * 1024, 16 * 1024 * 1024))
    resource.setrlimit(resource.RLIMIT_CPU, (90, 95))


def process_request(root: Path, ident: str, runner: Path, timeout: int = 100) -> str:
    if not ID.fullmatch(ident):
        raise ValueError('invalid request ID')
    incoming = root / 'inbox'
    outgoing = root / 'results'
    final_status = outgoing / (ident + '.status')
    if final_status.exists():
        return 'already-completed'
    # Exclusivity for concurrent daemon instances, without trusting .req contents.
    inputfd = os.open(incoming, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    lockname = ident + '.lock'
    try:
        lockfd = os.open(lockname, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                         0o600, dir_fd=inputfd)
    except FileExistsError:
        os.close(inputfd)
        return 'in-progress'
    except BaseException:
        os.close(inputfd)
        raise
    os.close(lockfd)
    ok, message = False, 'unknown error'
    stdout, stderr, frame = b'', b'', None
    try:
        if not runner.is_file() or not os.access(runner, os.X_OK):
            raise RuntimeError(f'QRX68 runtime missing: {runner}')
        req = regular_read(inputfd, ident + '.req', 128)
        if req != MARKER:
            raise ValueError('protocol marker mismatch')
        data = regular_read(inputfd, ident + '.hunk', MAX_INPUT)
        check_hunk(data)
        with tempfile.TemporaryDirectory(prefix='qrx68-guest-') as temp:
            tmp = Path(temp)
            (tmp / 'program.hunk').write_bytes(data)
            cmd = [str(runner), '--ram', '256', '--budget', '15000000',
                   '--experimental-gui', '--snapshot', str(tmp / 'frame.ppm'),
                   str(tmp / 'program.hunk')]
            with (tmp / 'stdout').open('wb') as o, (tmp / 'stderr').open('wb') as e:
                result = subprocess.run(cmd, cwd=tmp, stdin=subprocess.DEVNULL,
                                        stdout=o, stderr=e, timeout=timeout,
                                        check=False, preexec_fn=child_limits)
            stdout = (tmp / 'stdout').read_bytes()[:MAX_LOG]
            stderr = (tmp / 'stderr').read_bytes()[:MAX_LOG]
            if (tmp / 'frame.ppm').is_file():
                b = (tmp / 'frame.ppm').read_bytes()
                # P6 (640x256) exact frame from QRX68; no untrusted image parsing in host.
                if len(b) <= 1024 * 1024 and b.startswith(b'P6\n640 256\n255\n'):
                    frame = b
            ok = result.returncode == 0
            message = f'guest exit={result.returncode}'
    except subprocess.TimeoutExpired:
        message = 'guest timeout'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as e:
        message = str(e).replace('\n', ' ')[:200]
    finally:
        # Only fixed, bounded result files; no host source paths exposed to guest.
        try:
            atomic_publish(outgoing / (ident + '.out'), stdout)
            atomic_publish(outgoing / (ident + '.err'), stderr)
            if frame is not None and ok:
                atomic_publish(outgoing / (ident + '.ppm'), frame)
            atomic_publish(final_status, (('OK' if ok else 'ERR') + '\n' + message + '\n').encode('ascii', 'replace'))
            # Guest status survives until explicit user cleanup; consumed request marker removed.
            for name in (ident + '.req', ident + '.hunk', lockname):
                try: os.unlink(name, dir_fd=inputfd)
                except FileNotFoundError: pass

        except OSError:
            try: os.unlink(lockname, dir_fd=inputfd)
            except FileNotFoundError: pass
            raise
        finally:
            os.close(inputfd)
    return 'ok' if ok else 'error'


def validate_event(data: bytes) -> bytes:
    """Only a fixed typed IDCMP tuple crosses the privilege boundary."""
    if len(data) > 96 or len(data) < 12:
        raise ValueError('event length invalid')
    try:
        line = data.decode('ascii')
        fields = line.strip().split(' ')
        if len(fields) != 7 or fields[0] != 'QRX68/2':
            raise ValueError('invalid event marker')
        values = [int(x, 10) for x in fields[1:]]
    except (UnicodeError, ValueError) as e:
        raise ValueError('malformed IDCMP event') from e
    win, cls, code, qualifier, mx, my = values
    if not (0 <= win < 4 and cls in (8, 16, 512, 1024, 2097152)
            and 0 <= code <= 65535 and 0 <= qualifier <= 65535
            and -32768 <= mx <= 32767 and -32768 <= my <= 32767):
        raise ValueError('unsupported IDCMP event or out-of-range values')
    return ('QRX68/2 ' + ' '.join(map(str, values)) + '\n').encode('ascii')


def process_request_live(root: Path, ident: str, runner: Path, timeout: int = 100,
                         interval: float = 0.05) -> str:
    """Interactive QRX68/2 session with running HUNK, input, frames, stop.

    Result .frame is published only AFTER atomic PPM; .status published LAST.
    The service never opens a guest-supplied absolute path or runs a shell.
    """
    if not ID.fullmatch(ident):
        raise ValueError('invalid request ID')
    incoming, outgoing = root/'inbox', root/'results'
    def stop_session(_sig, _frame):
        raise RuntimeError('QRX68 service shutdown')
    signal.signal(signal.SIGTERM, stop_session)
    if (outgoing/(ident+'.status')).exists():
        return 'already-completed'
    fd = os.open(incoming, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    lock = ident+'.lock'
    child = None
    ok = False
    message = 'startup error'
    try:
        handle = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW,
                         0o600, dir_fd=fd)
        os.close(handle)
    except FileExistsError:
        os.close(fd)
        return 'in-progress'
    except BaseException:
        os.close(fd)
        raise
    try:
        if not runner.is_file() or not os.access(runner, os.X_OK):
            raise RuntimeError('QRX68 runtime missing')
        if regular_read(fd,ident+'.req',128) != MARKER:
            raise ValueError('protocol marker mismatch')
        data = regular_read(fd,ident+'.hunk',MAX_INPUT)
        check_hunk(data)
        with tempfile.TemporaryDirectory(prefix='qrx68-live-') as dirname:
            temp=Path(dirname)
            (temp/'program.hunk').write_bytes(data)
            events=temp/'events'
            events.mkdir(mode=0o700)
            frame=temp/'frame.ppm'
            stdout=temp/'stdout'
            stderr=temp/'stderr'
            cmd=[str(runner),'--ram','256','--budget','15000000',
                 '--experimental-gui','--live-events',str(events),
                 '--live-frame',str(frame),str(temp/'program.hunk')]
            with stdout.open('wb') as out, stderr.open('wb') as err:
                child=subprocess.Popen(cmd,cwd=temp,stdin=subprocess.DEVNULL,
                                       stdout=out,stderr=err,preexec_fn=child_limits)
                atomic_publish(outgoing/(ident+'.live'),b'QRX68/2 LIVE\n')
                deadline=time.monotonic()+timeout
                serial=0
                last_mtime=None
                while child.poll() is None:
                    if time.monotonic()>=deadline:
                        child.kill();message='guest timeout';break
                    # Fixed ordered event files, with no-follow reading and strict range checks.
                    names=sorted(n for n in os.listdir(incoming)
                                 if n.startswith(ident+'.') and EVT.fullmatch(n))[:64]
                    for name in names:
                        try:
                            body=validate_event(regular_read(fd,name,96))
                            atomic_publish(events/(name.split('.')[-2]+'.evt'),body)
                        except (ValueError,OSError):
                            pass # bad events are rejected, never forwarded
                        finally:
                            try: os.unlink(name,dir_fd=fd)
                            except FileNotFoundError: pass
                    try:
                        stop=regular_read(fd,ident+'.stop',64)
                        if stop==b'QRX68/2 STOP\n':
                            child.terminate();message='closed from AROS window'
                        os.unlink(ident+'.stop',dir_fd=fd)
                    except (FileNotFoundError,OSError,ValueError):
                        pass
                    try:
                        st=frame.stat()
                        if st.st_mtime_ns != last_mtime and st.st_size <= 1024*1024:
                            b=frame.read_bytes()
                            if b.startswith(b'P6\n640 256\n255\n') and len(b)==len(b'P6\n640 256\n255\n')+640*256*3:
                                atomic_publish(outgoing/(ident+'.ppm'),b)
                                serial+=1
                                atomic_publish(outgoing/(ident+'.frame'),f'QRX68/2 FRAME {serial}\n'.encode())
                                last_mtime=st.st_mtime_ns
                    except FileNotFoundError:
                        pass
                    time.sleep(interval)
                try: child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill();child.wait()
            if not message.startswith(('guest timeout','closed from')):
                ok=child.returncode==0
                message=f'guest exit={child.returncode}'
            atomic_publish(outgoing/(ident+'.out'),stdout.read_bytes()[:MAX_LOG])
            atomic_publish(outgoing/(ident+'.err'),stderr.read_bytes()[:MAX_LOG])
            # Last frame remains visible even after a successful guest exit.
            if frame.exists() and serial==0:
                b=frame.read_bytes()
                if len(b)==len(b'P6\n640 256\n255\n')+640*256*3 and b.startswith(b'P6\n640 256\n255\n'):
                    atomic_publish(outgoing/(ident+'.ppm'),b)
                    atomic_publish(outgoing/(ident+'.frame'),b'QRX68/2 FRAME 1\n')
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:
        message=str(e).replace('\n',' ')[:200]
    finally:
        if child is not None and child.poll() is None:
            child.kill();child.wait()
        try:
            atomic_publish(outgoing/(ident+'.status'),(('OK' if ok else 'ERR')+'\n'+message+'\n').encode('ascii','replace'))
        finally:
            try: (outgoing/(ident+'.live')).unlink(missing_ok=True)
            except OSError: pass
            for name in (ident+'.req',ident+'.hunk',lock):
                try: os.unlink(name,dir_fd=fd)
                except FileNotFoundError: pass
            os.close(fd)
    return 'ok' if ok else 'error'


def serve(root: Path, runner: Path, timeout: int, once: bool, interval: float, interactive: bool=False):
    setup_spool(root)
    print(f'QRX68 bridge ready at {root} (guest: QRXBRIDGE:)', flush=True)
    active = {}
    try:
        while True:
            # Separate processes: the Musashi CPU core is not thread-safe.
            for ident, process in list(active.items()):
                if not process.is_alive():
                    process.join()
                    del active[ident]
            names = []
            for p in (root / 'inbox').iterdir():
                if p.name.endswith('.req') and ID.fullmatch(p.name[:-4]) and not p.is_symlink():
                    names.append(p.name[:-4])
            for ident in sorted(names):
                if interactive:
                    if ident in active or len(active) >= 4:
                        continue
                    process = multiprocessing.Process(
                        target=process_request_live,
                        args=(root, ident, runner, timeout, interval))
                    process.start()
                    active[ident] = process
                    print(f'QRX68 {ident}: live process pid={process.pid}', flush=True)
                else:
                    r = process_request(root, ident, runner, timeout)
                    print(f'QRX68 {ident}: {r}', flush=True)
            if once:
                for process in active.values(): process.join()
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        for process in active.values():
            if process.is_alive(): process.terminate()
        for process in active.values():
            process.join(timeout=3)
            if process.is_alive(): process.kill();process.join(timeout=2)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bridge', type=Path, required=True)
    p.add_argument('--runner', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=100)
    p.add_argument('--interval', type=float, default=0.2)
    p.add_argument('--once', action='store_true')
    p.add_argument('--interactive', action='store_true',
                   help='QRX68/2 live guest with IDCMP event and frame streaming')
    a = p.parse_args(argv)
    if a.timeout < 1 or a.timeout > 600 or a.interval < .01:
        p.error('invalid timeout or interval')
    serve(a.bridge.resolve(), a.runner.resolve(), a.timeout, a.once, a.interval, a.interactive)
    return 0

if __name__ == '__main__':
    sys.exit(main())
