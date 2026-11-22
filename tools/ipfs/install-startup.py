#!/usr/bin/env python3
"""Wire the IPFS drive, node daemon and sync into a real AROS Startup-Sequence."""
from pathlib import Path
import re
import sys

# Wording from builds made before the drive/assign distinction; migrate it.
OLD_COMMENT = """; IPFS: is a normal drawer exposed as a volume.  QRXIPFSSync publishes
; changes through the local IPFS daemon; everything in it is public."""
PREV_COMMENT = """; IPFS: is an assign to a normal drawer; QRX Drive shows it as a volume.
; QRXIPFSSync publishes changes through the local IPFS daemon; all of it is public."""
NEW_COMMENT = """; IPFS: is an assign to a normal drawer; QRX Drive shows it as a volume.
; QRXIPFSD is the local node; QRXIPFSP2P is the libp2p peer;
; QRXIPFSSync publishes changes through the local node.
; Everything published there is public."""

ASSIGN = """
""" + NEW_COMMENT + """
If NOT EXISTS "SYS:Storage/IPFS"
    MakeDir "SYS:Storage/IPFS"
EndIf
Assign "IPFS:" "SYS:Storage/IPFS"
"""

NODE = """If EXISTS "C:QRXIPFSD"
    Run >NIL: C:QRXIPFSD
EndIf
"""

P2P = """If EXISTS "C:QRXIPFSP2P"
    Run >NIL: C:QRXIPFSP2P
EndIf
"""

SYNC = """If EXISTS "C:QRXIPFSSync"
    Run >NIL: C:QRXIPFSSync
EndIf
"""

START = NODE + P2P + SYNC


def patch(path: str) -> bool:
    p = Path(path)
    if not p.is_file():
        print(f"skip: {p}")
        return False
    original = p.read_text()
    text = original.replace(OLD_COMMENT, NEW_COMMENT)
    text = text.replace(PREV_COMMENT, NEW_COMMENT)

    # Drop old node/sync blocks so both are started exactly once, directly
    # after the assign and before any blocking desktop command.
    while START in text:
        text = text.replace(START, "", 1)
    while NODE in text:
        text = text.replace(NODE, "", 1)
    # Older builds redirected the P2P log, so the exact >NIL: block did not
    # match and a second copy could be inserted.  Remove every variant.
    text = re.sub(
        r'If EXISTS "C:QRXIPFSP2P"\r?\n[ \t]*Run[^\n]*C:QRXIPFSP2P[^\n]*\r?\nEndIf\r?\n',
        '', text)
    while SYNC in text:
        text = text.replace(SYNC, "", 1)

    match = re.search(r'^Assign "IPFS:"[^\n]*\n', text, re.MULTILINE)
    if match is None:
        anchor = 'Assign "CLIPS:"   "RAM:Clipboards"\n'
        if anchor not in text:
            print(f"skip (not a startup script): {p}")
            return False
        block = ASSIGN + "\n" + START
        text = text.replace(anchor, anchor + block, 1)
    else:
        text = text[:match.end()] + START + text[match.end():]

    changed = text != original
    if changed:
        p.write_text(text)
    print(f"{'patched' if changed else 'already wired'}: {p}")
    return True


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: install-startup.py <Startup-Sequence> [...]", file=sys.stderr)
        sys.exit(2)
    for arg in sys.argv[1:]:
        patch(arg)
