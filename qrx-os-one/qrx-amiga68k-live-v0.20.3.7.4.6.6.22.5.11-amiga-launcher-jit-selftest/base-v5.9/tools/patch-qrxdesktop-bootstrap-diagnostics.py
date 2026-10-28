#!/usr/bin/env python3
from pathlib import Path
import re, sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-qrxdesktop-bootstrap-diagnostics.py qrx_desktop.c")

p = Path(sys.argv[1])
s = p.read_text(errors="replace")
ACTIVE = "/* QRX v0.20.3.4.6 bootstrap diagnostics ACTIVE */"
if ACTIVE in s:
    print("QRXDesktop v0.20.3.4.6 diagnostics already applied:", p)
    raise SystemExit(0)

if re.search(r'(?m)^static\s+volatile\s+ULONG\s+qrx_boot_stage\s*=', s):
    raise SystemExit("contaminated source: qrx_boot_stage already exists")
if re.search(r'(?m)^static\s+void\s+qrx_boot_mark\s*\(', s):
    raise SystemExit("contaminated source: qrx_boot_mark already exists")

required_tokens = (
    "#include <proto/graphics.h>", "#include <proto/dos.h>",
    "#include <string.h>", "#include <stdio.h>", "#include <stdlib.h>",
    "#define TERM_LINE_LEN", "#define INPUT_LEN", "#define PREFS_PATH",
    "#define SESSION_NONE", "#define SESSION_TAKEOVER", "#define SESSION_WHDLOAD",
    "#define TERM_MIN_W", "#define TERM_MIN_H", "#define FILE_MIN_W", "#define FILE_MIN_H",
    "#define CLIP_COPY", "#define CLIP_MOVE", "enum LookFeel",
    "static struct Screen *g_screen", "static struct Window *g_desktop",
    "static struct Window *g_terminal", "g_screen_width", "g_screen_height",
    "g_display_label", "PoseidonBase", "g_session_action",
)
for token in required_tokens:
    if token not in s:
        raise SystemExit("source integrity failure before diagnostics: missing " + token)

def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        raise SystemExit("function not found: " + signature)
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit("opening brace not found: " + signature)
    depth = 0
    i = brace
    in_str = in_chr = esc = line_comment = block_comment = False
    while i < len(text):
        ch = text[i]
        nxt = text[i+1] if i + 1 < len(text) else ""
        if line_comment:
            if ch == "\n": line_comment = False
            i += 1; continue
        if block_comment:
            if ch == "*" and nxt == "/": block_comment = False; i += 2
            else: i += 1
            continue
        if in_str:
            if esc: esc = False
            elif ch == "\\": esc = True
            elif ch == '"': in_str = False
            i += 1; continue
        if in_chr:
            if esc: esc = False
            elif ch == "\\": esc = True
            elif ch == "'": in_chr = False
            i += 1; continue
        if ch == "/" and nxt == "/": line_comment = True; i += 2; continue
        if ch == "/" and nxt == "*": block_comment = True; i += 2; continue
        if ch == '"': in_str = True; i += 1; continue
        if ch == "'": in_chr = True; i += 1; continue
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + replacement + text[i+1:]
        i += 1
    raise SystemExit("unbalanced function braces: " + signature)

decl = None
for pat in (r'(?m)^struct DosLibrary \*DOSBase\s*=\s*NULL;\s*$', r'(?m)^struct DosLibrary \*DOSBase;\s*$'):
    m = re.search(pat, s)
    if m: decl = m; break
if decl is None:
    raise SystemExit("DOSBase declaration not found")

helper = '''
/* QRX v0.20.3.4.6 bootstrap diagnostics */
static volatile ULONG qrx_boot_stage = 0;
static void qrx_boot_mark(ULONG stage, const char *msg) {
    qrx_boot_stage = stage;
    if (DOSBase && msg) {
        PutStr((STRPTR)msg);
        PutStr((STRPTR)"\\n");
    }
}
/* QRX v0.20.3.4.6 bootstrap diagnostics ACTIVE */
'''
s = s[:decl.end()] + "\n" + helper + s[decl.end():]

safe_helper = '''static struct Screen *qrx_try_lock_workbench(void) {
    return LockPubScreen((STRPTR)"Workbench");
}'''
s = replace_function(s, "static struct Screen *qrx_try_lock_workbench(void)", safe_helper)

fn = re.search(r'(?s)(static\s+int\s+qrx_open_runtime_screen\s*\([^)]*\)\s*\{)(.*?)(\n\})', s)
if not fn: raise SystemExit("qrx_open_runtime_screen not found")
body = fn.group(2)
body = '\n    qrx_boot_mark(50, "QRXBOOT 50 qrx_open_runtime_screen enter");\n' + body
if 'qrx_detect_screen_policy();' not in body: raise SystemExit("screen-policy anchor missing")
body = body.replace('qrx_detect_screen_policy();',
    'qrx_boot_mark(51, "QRXBOOT 51 before screen policy");\n    qrx_detect_screen_policy();\n    qrx_boot_mark(52, "QRXBOOT 52 after screen policy");', 1)
if 'qrx_locked_workbench = qrx_try_lock_workbench();' not in body: raise SystemExit("Workbench lock anchor missing")
body = body.replace('qrx_locked_workbench = qrx_try_lock_workbench();',
    'qrx_boot_mark(59, "QRXBOOT 59 before LockPubScreen");\n        qrx_locked_workbench = qrx_try_lock_workbench();\n        qrx_boot_mark(qrx_locked_workbench ? 61 : 62, qrx_locked_workbench ? "QRXBOOT 61 Workbench locked" : "QRXBOOT 62 Workbench unavailable");', 1)
sm = re.search(r'(?m)^(\s*)g_screen\s*=\s*(OpenScreenTags|OpenScreenTagList)\(', body)
if sm:
    indent = sm.group(1)
    body = body[:sm.start()] + indent + 'qrx_boot_mark(70, "QRXBOOT 70 before OpenScreen");\n' + body[sm.start():]
s = s[:fn.start()] + fn.group(1) + body + fn.group(3) + s[fn.end():]

main = re.search(r'(?s)(int\s+main\s*\([^)]*\)\s*\{)(.*?)(\n\})\s*$', s)
if not main: raise SystemExit("main not found")
mb = '\n    qrx_boot_stage = 1;\n' + main.group(2)
pairs = [
('IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);', 'qrx_boot_stage = 10;\n    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);\n    qrx_boot_stage = IntuitionBase ? 11 : 12;'),
('GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);', 'qrx_boot_stage = 20;\n    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);\n    qrx_boot_stage = GfxBase ? 21 : 22;'),
('DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39);', 'qrx_boot_stage = 30;\n    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 39);\n    qrx_boot_stage = DOSBase ? 31 : 32;\n    if (DOSBase) {\n        qrx_boot_mark(31, "QRXBOOT 31 dos.library opened");\n        if (IntuitionBase) qrx_boot_mark(11, "QRXBOOT 11 intuition.library opened");\n        if (GfxBase) qrx_boot_mark(21, "QRXBOOT 21 graphics.library opened");\n    }'),
('IconBase = OpenLibrary("icon.library", 37);', 'qrx_boot_mark(40, "QRXBOOT 40 before icon.library");\n    IconBase = OpenLibrary("icon.library", 37);\n    qrx_boot_mark(IconBase ? 41 : 42, IconBase ? "QRXBOOT 41 icon.library opened" : "QRXBOOT 42 icon.library missing");'),
('PoseidonBase = OpenLibrary("poseidon.library", 0);', 'qrx_boot_mark(43, "QRXBOOT 43 before poseidon.library");\n    PoseidonBase = OpenLibrary("poseidon.library", 0);\n    qrx_boot_mark(PoseidonBase ? 44 : 45, PoseidonBase ? "QRXBOOT 44 poseidon.library opened" : "QRXBOOT 45 poseidon.library unavailable");')]
for old,new in pairs:
    if old not in mb: raise SystemExit("main anchor missing: " + old[:45])
    mb = mb.replace(old,new,1)
if 'load_preferences();' in mb:
    mb = mb.replace('load_preferences();', 'qrx_boot_mark(46, "QRXBOOT 46 before preferences");\n    load_preferences();\n    qrx_boot_mark(47, "QRXBOOT 47 after preferences");', 1)
s = s[:main.start()] + main.group(1) + mb + main.group(3) + s[main.end():]

for token in required_tokens:
    if token not in s: raise SystemExit("source integrity failure after diagnostics: missing " + token)
stage_defs = len(re.findall(r'(?m)^static\s+volatile\s+ULONG\s+qrx_boot_stage\s*=', s))
mark_defs = len(re.findall(r'(?m)^static\s+void\s+qrx_boot_mark\s*\(', s))
if stage_defs != 1 or mark_defs != 1: raise SystemExit(f"diagnostic helper count invalid: stage={stage_defs} mark={mark_defs}")
if 'g_screen = qrx_boot_mark' in s: raise SystemExit("OpenScreen assignment corruption detected")
proto_pos = s.find("#include <proto/intuition.h>")
launch_pos = s.find("struct QRXLaunchProfile")
if proto_pos < 0 or launch_pos <= proto_pos: raise SystemExit("top-of-file structure invalid")
seg = s[proto_pos:launch_pos]
for token in ("#include <proto/graphics.h>", "#include <stdio.h>", "#define TERM_LINE_LEN", "#define SESSION_TAKEOVER", "#define TERM_MIN_W", "#define CLIP_COPY", "enum LookFeel"):
    if token not in seg: raise SystemExit("top-of-file preservation failure: missing " + token)

p.write_text(s)
print("QRXDesktop v0.20.3.4.6 safe diagnostics patch OK:", p)
