# Windows game used by QRX 22.5.12: Doom engine (PrBoom-plus) + Freedoom

GTA 1, Half-Life and Resident Evil demos could not be fetched (archive.org, moddb and
similar hosts are blocked by the network policy of the build environment) and are not
redistributable anyway.  The Windows-only game shown instead is the Doom engine
PrBoom-plus, built as a 32-bit Windows program that draws through DirectDraw
(SDL 1.2 "directx" driver), with the free Freedoom IWAD as game data.

| File | Content | Origin | License |
|---|---|---|---|
| libsdl1.2_1.2.15+dfsg2.orig.tar.gz | SDL 1.2.15 sources | http://archive.ubuntu.com/ubuntu/pool/main/libs/libsdl1.2/ | LGPL-2.1 |
| prboom-plus_2.5.1.4~svn4356+dfsg1.orig.tar.xz | PrBoom-plus 2.5.1.4 sources | http://archive.ubuntu.com/ubuntu/pool/universe/p/prboom-plus/ | GPL-2+ |
| prboom-plus_2.5.1.4~svn4356+dfsg1-1_i386.deb | used only for `prboom-plus.wad` (engine data, built from the sources above) | same | GPL-2+ |
| freedoom_0.13.0-3_all.deb | Freedoom 0.13 `freedoom1.wad` | http://archive.ubuntu.com/ubuntu/pool/universe/f/freedoom/ | BSD-3-clause |

Build (MinGW i686, own compiler calls, no configure scripts): see
`x86-runtime-22.5.12/win32/apps/doom/` (build-sdl.sh, build-doom.sh, SDL_config.h,
config.h, qrx_doom_stubs.c: audio/joystick/music stubs).  `tranmap.dat` is written by
PrBoom-plus on its first run (deterministic table) and copied to the Amiga to save
emulated CPU time.
