QRX OS One 68K v0.20.3.7.4.6.6.14 — ARES Security & QRX Web

Adds native ARESAntivirus and QRXWeb executables to the strict Deadwood AROS ABI/HUNK build.

QRX Web baseline:
- Up to 6 tabs.
- HTML text/layout baseline with line wrapping and scrolling.
- CSS architecture is present at policy/layout level; JavaScript is intentionally absent.
- Request-side Safe Mode / ad & tracker URL filtering.
- script/video/audio/iframe content is not rendered; media placeholders are used.
- Browser tries C:wget and then C:curl for HTTP(S). If neither exists, it reports network-fetch unavailable instead of silently failing.
- Image slots are recognized. Native PNG/JPEG/GIF pixel decoding is NOT falsely claimed by this release; decoder ABI/optimized 68K implementation remains the next hard gate before calling image support complete.
- Safe mode defaults ON; downloads default OFF. Disabling protection from the browser UI is intentionally not permitted in the production policy (parental-control settings will own it).

ARES Antivirus baseline:
- local-only scanner, no cloud upload
- streaming scan (4 KiB working buffer)
- fingerprint and signature/pattern hooks
- command-line file scan for browser/download integration
- GUI quick scan
- signature DB path reserved at QRX:ARES/signatures.db

Security note:
This release establishes the native ARES/Browser application and policy boundary. It is not marketed as a replacement for a maintained commercial AV signature service, nor as a complete modern web engine.
