#!/usr/bin/env python3
"""Actual guest 68k HUNK -> small graphics HLE -> Mac Q86F GPU socket bytes.
No Metal hardware is available in this test; validates protocol only.
"""
import socket,struct,subprocess,sys,tempfile
from pathlib import Path
exe,hunk=map(Path,sys.argv[1:3]);
with tempfile.TemporaryDirectory(prefix='qrx68-metal23-') as td:
    path=td+'/x.sock'
    listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    listener.bind(path);listener.listen(1);listener.settimeout(10)
    run=subprocess.Popen([str(exe),'--budget','100000','--experimental-gui',
                          '--gpu-socket',path,str(hunk)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        conn,_=listener.accept()
        with conn:
            def recv(n):
                b=bytearray()
                while len(b)<n:
                    c=conn.recv(n-len(b))
                    if not c:raise AssertionError('unexpected EOF')
                    b.extend(c)
                return bytes(b)
            magic,w,h,n=struct.unpack('>4I',recv(16))
            assert (magic,w,h,n)==(0x51383646,640,256,640*256*4)
            frame=recv(n)
            def at(x,y):return frame[(y*w+x)*4:(y*w+x)*4+4]
            assert at(25,30)==bytes((255,64,96,244)),at(25,30)
            assert at(3,3)==bytes((255,0,0,0)),at(3,3)
        out,err=run.communicate(timeout=10)
        assert run.returncode==0,(out,err)
    finally:
        if run.poll() is None:run.kill();run.wait()
        listener.close()
print('PASS: guest Amiga HUNK rendered real Q86F framebuffer to native Metal-compatible socket (Metal device not tested)')
