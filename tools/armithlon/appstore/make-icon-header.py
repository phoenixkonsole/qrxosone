#!/usr/bin/env python3
"""Writes an app icon for QRXAppStore.c as a C header: 128 x 128 pixels, 0xAARRGGBB.
Usage: make-icon-header.py <logo.png with transparency> <name> > icon_<name>.h
(ThunderCat: the 512 px PNG from spidermonkey/branding/make-logo.py)"""
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA").resize((128, 128), Image.LANCZOS)
name = sys.argv[2]
px = list(im.get_flattened_data()) if hasattr(im, "get_flattened_data") else list(im.getdata())
print(f"/* {name}: 128 x 128, 0xAARRGGBB; written by make-icon-header.py */")
print(f"static const unsigned int icon_{name}[128 * 128] = {{")
for i in range(0, len(px), 8):
    print("  " + ", ".join("0x%02x%02x%02x%02x" % (a, r, g, b) for r, g, b, a in px[i:i + 8]) + ",")
print("};")
