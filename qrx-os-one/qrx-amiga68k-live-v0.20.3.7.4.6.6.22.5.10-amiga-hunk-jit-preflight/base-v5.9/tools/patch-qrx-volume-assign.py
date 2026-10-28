#!/usr/bin/env python3
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-qrx-volume-assign.py <Startup-Sequence>")

p = Path(sys.argv[1])
text = p.read_text(errors="replace")
assign_line = 'Assign "QRX:" "SYS:QRX" >NIL:'
comment = '; QRX persistent payload assign - prevents volume requester for QRX:'

# Normalize any previous QRX payload assign so the result is deterministic.
text = re.sub(r'(?m)^\s*; QRX persistent payload assign.*\n?', '', text)
text = re.sub(r'(?m)^\s*Assign\s+"?QRX:"?\s+"?SYS:QRX"?\s*(?:>NIL:)?\s*$', '', text)

anchor_re = re.compile(r'(?m)^(\s*Assign\s+"WANDERER:"\s+"SYS:System/Wanderer"\s+DEFER\s*)$')
m = anchor_re.search(text)
if not m:
    raise SystemExit('canonical WANDERER assign anchor not found')
replacement = m.group(1) + '\n' + comment + '\n' + assign_line
text = text[:m.start()] + replacement + text[m.end():]

# Add a runtime checkpoint after the .5.7 startup log is created.
log_anchor = 'Echo "QRX: canonical Startup-Sequence restored" >RAM:QRX-Startup.log'
log_line = 'Echo "QRX: persistent assign QRX: -> SYS:QRX installed" >>RAM:QRX-Startup.log'
text = text.replace(log_line + '\n', '')
if log_anchor not in text:
    raise SystemExit('QRX startup log anchor not found')
text = text.replace(log_anchor, log_anchor + '\n' + log_line, 1)

if text.count(assign_line) != 1:
    raise SystemExit('QRX assign multiplicity is not exactly one')
if text.count(log_line) != 1:
    raise SystemExit('QRX assign log checkpoint multiplicity is not exactly one')
if text.find(assign_line) > text.find('WANDERER:Wanderer'):
    raise SystemExit('QRX assign is not installed before Wanderer launch')

p.write_text(text)
print(f"Persistent QRX: -> SYS:QRX startup assign installed: {p}")
