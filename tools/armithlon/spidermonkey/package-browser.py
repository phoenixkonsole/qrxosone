#!/usr/bin/env python3
"""Packs the Gecko browser of an obj-browser dist/bin for QRX OS One 64 (AROS): like a shipped
Firefox, the files go into omni.ja (Gecko itself) and browser/omni.ja (the browser application);
AROS's FFS allows file names of 30 characters only, and the unpacked tree has hundreds longer.
Next to them: the firefox program (with libxul linked in), application.ini, platform.ini. The
AROS preferences (one process, software rendering, AROS's TrueType fonts) go into
browser/omni.ja as defaults/preferences/qrx-aros.js.
usage: package-browser.py <obj-browser/dist/bin> <output dir>"""
import os
import shutil
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
SKIP_TOP = {"firefox", "firefox-bin", "plugin-container", "xpcshell", "pingsender", "nsinstall",
            "certutil", "pk12util", "browser", "application.ini", "platform.ini", "dependentlibs.list",
            "dependentlibs.list.gtest", ".mkdir.done"}

AROS_PREFS = """// QRX OS One 64 (AROS): Gecko in one process, AROS's fonts. WebRender draws on the host's GPU
// through the board's GL 3.3 door when there is one (widget/aros/GfxInfo.cpp), else with SWGL; a GL
// that fails at run time falls back to SWGL in this process too.
pref("browser.tabs.remote.autostart", false);
pref("fission.autostart", false);
pref("dom.ipc.processCount", 1);
pref("network.process.enabled", false);
pref("media.rdd-process.enabled", false);
pref("layers.gpu-process.enabled", false);
pref("gfx.webrender.fallback.software-requires-gpu-process", false);
// no helper processes either: the JS validator of Opaque Response Blocking and media decoding run in
// utility processes, and the web extensions in an extension process; AROS has none of them
pref("browser.opaqueResponseBlocking.javascriptValidator", false);
pref("media.utility-process.enabled", false);
pref("dom.ipc.processPrelaunch.enabled", false);
pref("extensions.webextensions.remote", false);
// Glean's upload thread: no network for telemetry; a negative port makes FOG treat every upload as
// done without one (its upload thread crashed on QRX OS One 64)
pref("telemetry.fog.test.localhost_port", -1);
// a built-in extension never finishes starting without an extension process, and shutdown waits for
// it; after the default 60 s AsyncShutdown would crash the browser (and with it the guest)
pref("toolkit.asyncshutdown.crash_timeout", 86400000);
pref("browser.shell.checkDefaultBrowser", false);
pref("browser.startup.homepage_override.mstone", "ignore");
pref("startup.homepage_welcome_url", "");
pref("startup.homepage_welcome_url.additional", "");
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("toolkit.telemetry.enabled", false);
pref("app.update.enabled", false);
pref("app.normandy.enabled", false);
pref("extensions.update.enabled", false);
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
pref("toolkit.startup.max_resumed_crashes", -1);
pref("font.name.sans-serif.x-western", "Bitstream Vera Sans");
pref("font.name.serif.x-western", "Bitstream Vera Serif");
pref("font.name.monospace.x-western", "Bitstream Vera Sans Mono");
pref("font.default.x-western", "sans-serif");
"""


def add_tree(z, root, rel_prefix, skip):
    n = 0
    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        rel = os.path.relpath(dirpath, root)
        if rel == ".":
            dirnames[:] = sorted(d for d in dirnames if d not in skip and not d.startswith("gmp-"))
            filenames = [f for f in filenames if f not in skip]
        else:
            dirnames.sort()
        for f in sorted(filenames):
            if f.endswith((".so", ".so.deps")):
                continue
            full = os.path.join(dirpath, f)
            arc = os.path.normpath(os.path.join(rel_prefix, rel, f)) if rel != "." else os.path.join(rel_prefix, f)
            z.write(os.path.realpath(full), arc.lstrip("./") if arc.startswith("./") else arc)
            n += 1
    return n


os.makedirs(os.path.join(out, "browser"), exist_ok=True)
with zipfile.ZipFile(os.path.join(out, "omni.ja"), "w", zipfile.ZIP_DEFLATED) as z:
    n1 = add_tree(z, src, "", SKIP_TOP)
with zipfile.ZipFile(os.path.join(out, "browser", "omni.ja"), "w", zipfile.ZIP_DEFLATED) as z:
    n2 = add_tree(z, os.path.join(src, "browser"), "", {"omni.ja"})
    # a fixed date: the same omni.ja on every run (package-appstore.sh is reproducible)
    zi = zipfile.ZipInfo("defaults/preferences/qrx-aros.js", date_time=(2026, 1, 1, 0, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, AROS_PREFS)
for f in ("application.ini", "platform.ini"):
    if os.path.exists(os.path.join(src, f)):
        shutil.copy(os.path.join(src, f), out)
shutil.copy(os.path.join(src, "firefox"), out)
print(f"omni.ja: {n1} files, browser/omni.ja: {n2} files + qrx-aros.js")
