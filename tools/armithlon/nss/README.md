# NSS für QRX OS One 64 (m68k64-unknown-aros)

Mozillas Kryptobibliothek (NSS 3.125.1 aus Firefox ESR 153.8.0) statisch auf AROS: Softoken und freebl sind in
das Programm gelinkt (`NSS_STATIC_SOFTOKEN`), weil AROS kein `dlopen` hat. Gebaut mit NSS' eigenen gyp-Dateien
(gyp aus Firefox' `third_party/python`) und ninja, `OS=aros`, `target_arch=m68k64`, ohne FIPS, DBM und libpkix.

| Datei | Zweck |
| --- | --- |
| `build.sh <firefox-tree> <nss-dir> <nspr-out> <out>` | gyp + ninja; die statischen Bibliotheken landen in `<out>/dist/Release/lib` (die `.so`-Ziele scheitern am Linken, AROS braucht sie nicht) |
| `aros-nss.h` | vor jede Quelle eingebunden: `getpagesize`, `syslog` (libqrxaroscompat) |
| `tests/nsstest.c`, `tests/build-test.sh`, `tests/run-nss.sh` | Gasttest mit lokalem HTTPS-Server auf dem Host |

Die Quelle kommt mit `../spidermonkey/fetch.sh` (jetzt mit `security/nss`), die AROS-Änderung im selben Patch.

## Anpassungen

- `freebl/unix_urandom.c`: auf AROS `getentropy()` (libqrxaroscompat: der Zufallsgenerator des Hosts über das
  Board), mit `-DSEED_ONLY_DEV_URANDOM`; ein `/dev/urandom` gibt es nicht. Ohne Entropie scheitert `NSS_Init`.
- Übersetzung: `-std=c99` setzt `__STRICT_ANSI__`, das AROS' Header viele POSIX-Funktionen verstecken lässt; also
  `-U__STRICT_ANSI__ -D_GNU_SOURCE` (unter Linux setzt NSS dafür `_DEFAULT_SOURCE`). Dazu `-DNS_PTR_GT_32`
  (64 Bit), `-Dalloca=__builtin_alloca` (HACL*), `-DSQLITE_OMIT_LOAD_EXTENSION`, die bsdsocket-Schalter wie bei NSPR.
- NSPR stellt seine Header jetzt wie Mozillas `dist/include/nspr` in einem Verzeichnis bereit (`../nspr/build.sh`).
- libqrxaroscompat: `getpagesize`, `fchown` (ENOSYS), `syslog` (auf die Fehlerausgabe).

## Stand (25.11.2026)

Gast (qrxboot, zwei EC64-Kerne), `tests/run-nss.sh`: **NSS ALL PASS**
- `NSS_NoDB_Init`; SHA-256("abc") nach FIPS 180; HMAC-SHA256 gleich Pythons `hmac`; AES-128-GCM mit
  Manipulationserkennung; ECDSA P-256 Schlüssel, Signatur, Prüfung (rund 1,5 s im Emulator); Zufall.
- TLS 1.3 (`TLS_AES_256_GCM_SHA384`) zu einem HTTPS-Server auf dem Host über die Netz-Tür, HTTP-Antwort `200 OK`.
- Zertifikatsprüfung durch NSS selbst: ohne Wurzel abgelehnt (`SEC_ERROR_UNTRUSTED_ISSUER`), mit dem Testzertifikat
  als Wurzel angenommen (Kette und Hostname). Testverbindungen ohne Sitzungs-Cache, sonst überspringt die
  Wiederaufnahme die Prüfung.

Offen: die eingebauten Wurzelzertifikate (`nssckbi`) statisch einbinden (NSPRs `PR_LoadStaticLibrary`), damit
öffentliche Server ohne mitgegebene Wurzel geprüft werden; aus diesem Container gibt es keine direkte Verbindung ins
Internet (nur über einen Proxy), echte Server sind hier daher nicht getestet.
