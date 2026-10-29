# Amiga 68k application sources used by QRX 22.5.12

| File | Program | Origin | License |
|---|---|---|---|
| DOpus5_104_os3.lha | Directory Opus 5.104 (AmigaOS 3.x m68k build) | https://github.com/BlitterStudio/dopus5 release asset | AROS Public License |
| HippoPlayer-2.67-00200975.tar.gz | HippoPlayer 2.67 (sources + prebuilt `dist/HiP`, `playsid.library`) | https://github.com/koobo/HippoPlayer commit 00200975c036ff908c3b0d8658f6df5e85e95cb1 | MIT (LICENSE.md) |

Aminet (the usual source for Amiga software) is blocked by the network policy of
the build environment, so the GitHub releases of these open-source programs are used.
AmigaGPT 3.4.0 was tested too but needs real MUI custom classes (and AmiSSL/TCP);
it does not start on AROS Zune and is therefore not shipped.
