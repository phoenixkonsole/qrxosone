#!/usr/bin/env python3
"""Add an AROS/Wanderer display fallback to QRXDesktop.

The fixed RTG/AGA candidates remain first choice.  If none opens, QRX clones
whatever display mode the current default public screen is already using.  A
final SA_LikeWorkbench attempt lets Intuition inherit the current desktop mode
when the public-screen mode cannot be queried reliably.
"""
from pathlib import Path
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-qrxdesktop-display-fallback.py qrx_desktop.c")

p = Path(sys.argv[1])
s = p.read_text(errors="replace")
marker = "/* QRX v0.20.3.7.4.6.5.7 AROS display fallback */"

if marker in s:
    print("QRXDesktop display fallback already present:", p)
    raise SystemExit(0)

needle = '''    if (!g_screen) {
        PutStr("QRX: no usable RTG or AGA display mode found.\\n");
        return 0;
    }
'''
if needle not in s:
    raise SystemExit("QRXDesktop fixed-mode failure anchor not found")

replacement = r'''    /* QRX v0.20.3.7.4.6.5.7 AROS display fallback */
    if (!g_screen) {
        struct Screen *active = LockPubScreen(NULL);
        if (active) {
            ULONG active_mode = GetVPModeID(&active->ViewPort);
            WORD active_width = active->Width;
            WORD active_height = active->Height;
            ULONG active_depth = GetBitMapAttr(active->RastPort.BitMap, BMA_DEPTH);
            ULONG requested_depth = active_depth < QRX_DEPTH ? QRX_DEPTH : active_depth;

            UnlockPubScreen(NULL, active);

            if (active_mode != INVALID_ID &&
                active_width > 0 && active_height > 0 && active_depth > 0) {
                g_screen = OpenScreenTags(NULL,
                    SA_DisplayID, active_mode,
                    SA_Width, active_width,
                    SA_Height, active_height,
                    SA_Depth, requested_depth,
                    SA_Type, CUSTOMSCREEN,
                    SA_Title, (ULONG)"QRX OS One Amiga 68K v0.20",
                    SA_PubName, (ULONG)"QRX",
                    SA_ShowTitle, FALSE,
                    SA_Quiet, TRUE,
                    TAG_DONE);

                if (g_screen) {
                    ULONG depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);
                    if (depth < QRX_DEPTH) {
                        CloseScreen(g_screen);
                        g_screen = NULL;
                    }
                    if (g_screen) {
                    g_screen_mode = GetVPModeID(&g_screen->ViewPort);
                    g_screen_width = g_screen->Width;
                    g_screen_height = g_screen->Height;
                    g_screen_depth = (UBYTE)depth;
                    g_display_is_rtg =
                        (g_screen_width > QRX_W ||
                         g_screen_height > QRX_H ||
                         g_screen_depth > QRX_DEPTH);
                    snprintf(g_display_label, sizeof(g_display_label),
                             "AROS current %dx%dx%d",
                             (int)g_screen_width, (int)g_screen_height,
                             (int)g_screen_depth);
                    PutStr("QRX: using current AROS/Wanderer display mode fallback.\n");
                    }
                }
            }
        }
    }

    if (!g_screen) {
        g_screen = OpenScreenTags(NULL,
            SA_LikeWorkbench, TRUE,
            SA_Depth, QRX_DEPTH,
            SA_Type, CUSTOMSCREEN,
            SA_Title, (ULONG)"QRX OS One Amiga 68K v0.20",
            SA_PubName, (ULONG)"QRX",
            SA_ShowTitle, FALSE,
            SA_Quiet, TRUE,
            TAG_DONE);

        if (g_screen) {
            ULONG depth = GetBitMapAttr(g_screen->RastPort.BitMap, BMA_DEPTH);
            if (depth < QRX_DEPTH) {
                CloseScreen(g_screen);
                g_screen = NULL;
            }
            if (g_screen) {
            g_screen_mode = GetVPModeID(&g_screen->ViewPort);
            g_screen_width = g_screen->Width;
            g_screen_height = g_screen->Height;
            g_screen_depth = (UBYTE)depth;
            g_display_is_rtg =
                (g_screen_width > QRX_W ||
                 g_screen_height > QRX_H ||
                 g_screen_depth > QRX_DEPTH);
            snprintf(g_display_label, sizeof(g_display_label),
                     "AROS Workbench-like %dx%dx%d",
                     (int)g_screen_width, (int)g_screen_height,
                     (int)g_screen_depth);
            PutStr("QRX: using SA_LikeWorkbench display fallback.\n");
            }
        }
    }

    if (!g_screen) {
        PutStr("QRX: no usable RTG, AGA, or current AROS display mode found.\n");
        return 0;
    }
'''

s = s.replace(needle, replacement, 1)
p.write_text(s)

check = p.read_text(errors="replace")
required = [
    marker,
    "LockPubScreen(NULL)",
    "GetVPModeID(&active->ViewPort)",
    "UnlockPubScreen(NULL, active)",
    "SA_LikeWorkbench, TRUE",
    "SA_Depth, QRX_DEPTH",
    "requested_depth = active_depth < QRX_DEPTH ? QRX_DEPTH : active_depth",
    'SA_PubName, (ULONG)"QRX"',
    "using current AROS/Wanderer display mode fallback",
    "using SA_LikeWorkbench display fallback",
]
for token in required:
    if token not in check:
        raise SystemExit(f"display fallback patch verification failed: {token}")

# Fixed RTG/AGA candidates must remain ahead of the new fallback.
if check.index("BestModeID(") > check.index(marker):
    raise SystemExit("display fallback was inserted before the fixed candidate path")

# Public-screen publication remains after successful selection.
if check.index(marker) > check.index("PubScreenStatus(g_screen, 0);"):
    raise SystemExit("display fallback is not before public-screen publication")

print("QRXDesktop AROS display fallback patch OK:", p)
