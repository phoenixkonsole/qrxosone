#!/usr/bin/env python3
"""Writes DEVS:AudioModes/QRXHOST, the IFF AHIM mode file for qrxhost.audio."""
import struct, sys
BASE = 0x80000000
AUDIOID, VOLUME, PANNING, STEREO, HIFI, MULTTAB, NAME = 100, 103, 104, 105, 106, 108, 109
TAGBASE_R = 0x8000  # the tag's value points inside the chunk
def chunk(cid, body):
    return cid + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
def mode(mode_id, name):
    tags = [(AUDIOID, mode_id), (VOLUME, 1), (PANNING, 1), (STEREO, 1), (HIFI, 0), (MULTTAB, 0)]
    head = len(tags) * 8 + 8 + 8     # tags, name tag, TAG_DONE
    body = b''.join(struct.pack('>II', BASE + t, v) for t, v in tags)
    body += struct.pack('>II', BASE + TAGBASE_R + NAME, head) + struct.pack('>II', 0, 0)
    return chunk(b'AUDM', body + name.encode() + b'\0')
form = b'AHIM' + chunk(b'AUDN', b'qrxhost\0') + mode(0x00510001, 'QRXHost:16 bit stereo++')
open(sys.argv[1], 'wb').write(b'FORM' + struct.pack('>I', len(form)) + form)
# ENV:Sys/ahi.prefs, so the default unit uses this mode: FORM PREF / AHIU.
if len(sys.argv) > 2:
    unit = struct.pack('>BBHIIIIIII', 0, 0, 8, 0x00510001, 48000, 0, 0x10000, 0x10000, 0, 0)
    pref = b'PREF' + chunk(b'AHIU', unit)
    open(sys.argv[2], 'wb').write(b'FORM' + struct.pack('>I', len(pref)) + pref)
