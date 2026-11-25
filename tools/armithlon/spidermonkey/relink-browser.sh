#!/bin/sh
# Rebuilds the given objdir directories, then relinks libxul and firefox (AROS: relocatable libxul,
# see config/rules.mk). Usage: relink-browser.sh <dir in obj-browser>...
HERE="$(cd "$(dirname "$0")" && pwd)"
for d in "$@"; do sh "$HERE/make-browser.sh" -C "$d" || exit 1; done
rm -f /opt/qrx-os-one/gecko/obj-browser/dist/bin/libxul.so
rm -f /opt/qrx-os-one/gecko/obj-browser/dist/bin/firefox   # its rule does not depend on the libraries
sh "$HERE/make-browser.sh" -C toolkit/library/build && sh "$HERE/make-browser.sh" -C browser/app
