#!/usr/bin/env python3
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-aros-startup-arosvision-bootstrap.py <Startup-Sequence>")

p = Path(sys.argv[1])
text = p.read_text(errors="replace")

# Remove any previous QRX bootstrap blocks if a reused runtime tree contains one.
text = re.sub(
    r"(?ims)^; --- QRX v0\.20\.3\.7\.4\.6\.5\.[0-9]+ (?:AROS VISION STYLE DIRECT DESKTOP BOOTSTRAP|IPREFS NO-RESET DESKTOP BOOTSTRAP) ---\s*.*?^; --- /QRX (?:AROS VISION STYLE DIRECT DESKTOP BOOTSTRAP|IPREFS NO-RESET DESKTOP BOOTSTRAP) ---\s*",
    "",
    text,
)

# This release explicitly retires all detached/screen-reset startup paths.
for forbidden in (
    "QRX-PostIPrefs-Startup",
    "QRX-StartupChild.ready",
    "QRX-ScreenReset.log",
    "FAIL-SAFE INTUITION SCREEN RESET HANDOFF",
):
    if forbidden in text:
        raise SystemExit(f"old detached/screen-reset bootstrap still present: {forbidden}")

# Target evidence from the .5.5 smoke test:
#   - running IPrefs produces the Intuition screen-reset requester
#   - pressing Cancel lets the canonical Wanderer launch continue normally
# Therefore do NOT execute IPrefs during the initial desktop bootstrap. Preserve
# the already-created Workbench/default public screen and continue directly to
# the rest of the canonical startup sequence.
ipat = re.compile(r"(?m)^[ \t]*IPrefs[ \t]*$")
ims = list(ipat.finditer(text))
if len(ims) != 1:
    raise SystemExit(f"expected exactly one canonical IPrefs line, found {len(ims)}")

irepl = '''; --- QRX v0.20.3.7.4.6.5.7 IPREFS NO-RESET DESKTOP BOOTSTRAP ---
Delete >NIL: RAM:QRX-Startup.log QUIET
Echo "QRX: canonical Startup-Sequence restored" >RAM:QRX-Startup.log
Echo "QRX: IPrefs deferred - preserving existing Workbench screen" >>RAM:QRX-Startup.log
; IPrefs intentionally deferred by QRX v0.20.3.7.4.6.5.7.
; The .5.5 target smoke test proved that IPrefs requests a screen reset here;
; Cancel allowed Wanderer to continue. Do not reset the live Intuition screen.
; --- /QRX IPREFS NO-RESET DESKTOP BOOTSTRAP ---'''
text = ipat.sub(irepl, text, count=1)

# Keep the canonical direct synchronous Wanderer handoff. No child shell, no
# Run/Execute continuation, no wait loop, no screen reopening.
wpat = re.compile(r"(?m)^([ \t]*)WANDERER:Wanderer[ \t]*$")
wms = list(wpat.finditer(text))
if len(wms) != 1:
    raise SystemExit(f"expected exactly one direct WANDERER:Wanderer line, found {len(wms)}")

def wrepl(m):
    i=m.group(1)
    return (
        f'{i}Echo "QRX: launching Wanderer on preserved Workbench screen" >>RAM:QRX-Startup.log\n'
        f'{i}WANDERER:Wanderer\n'
        f'{i}Echo "QRX: Wanderer command returned" >>RAM:QRX-Startup.log'
    )
text = wpat.sub(wrepl, text, count=1)

# Structural safety gates.
if re.search(r"(?m)^[ \t]*IPrefs[ \t]*$", text):
    raise SystemExit("active IPrefs command survived no-reset bootstrap patch")
if len(re.findall(r"(?m)^[ \t]*WANDERER:Wanderer[ \t]*$", text)) != 1:
    raise SystemExit("unexpected direct Wanderer command multiplicity after patch")
if "IPrefs deferred - preserving existing Workbench screen" not in text:
    raise SystemExit("IPrefs deferred diagnostic missing")
if 'C:Run >NIL: QUIET C:Execute S:QRX-PostIPrefs-Startup' in text:
    raise SystemExit("detached continuation survived normalization")
if 'RAM:QRX-StartupChild.ready' in text:
    raise SystemExit("child-ready marker survived normalization")
if not re.search(r'(?ims)If EXISTS "WANDERER:Wanderer".*?WANDERER:Wanderer.*?If NOT ERROR\s+EndCLI', text):
    raise SystemExit("direct Wanderer -> successful EndCLI contract not found")

p.write_text(text)
print(f"IPrefs no-reset / preserved Workbench bootstrap installed: {p}")
