#!/usr/bin/env python3
"""Read-only binary identification. Does NOT claim execution support."""
import sys,struct,pathlib

def identify(path):
    data=pathlib.Path(path).read_bytes()[:64]
    if len(data)>=4 and data[:4]==b"\x00\x00\x03\xf3": return "Amiga HUNK (68K candidate); AROS binary compatibility requires testing"
    if data[:4]==b"\x7fELF" and len(data)>=20:
        order="little" if data[5]==1 else "big" if data[5]==2 else None
        if not order:return "ELF with invalid byte order"
        machine=int.from_bytes(data[18:20],order)
        bits={1:32,2:64}.get(data[4],"unknown")
        if machine==20:return f"Linux/other PPC ELF{bits}; ABI/OS not established; execution unsupported"
        if machine==21:return f"PPC64 ELF{bits}; execution unsupported"
        if machine==4:return f"m68k ELF{bits}; ABI/OS not established"
        return f"ELF{bits} machine={machine}; unsupported/unknown"
    if data[:2]==b"MZ":return "DOS/PE candidate; inspect with QRX86 loader"
    return "Unknown format"
if __name__=="__main__":
    if len(sys.argv)<2:raise SystemExit("Usage: python3 qrx-binary-identify.py FILE...")
    for name in sys.argv[1:]:
        try:print(f"{name}: {identify(name)}")
        except OSError as e:print(f"{name}: ERROR {e}")
