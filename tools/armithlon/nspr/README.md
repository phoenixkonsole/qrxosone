# NSPR für QRX OS One 64 (m68k64-unknown-aros)

Mozillas Laufzeitbibliothek (NSPR 4.39 aus Firefox ESR 153.8.0) auf AROS: Threads, Sperren, Zeit,
Dateien, Sockets, Poll, Namensauflösung. Die Portierung steckt im Firefox-Patch
`../spidermonkey/spidermonkey-153.8.0-aros.patch` (der gesamte Quellbaum wird von `../spidermonkey/fetch.sh`
ausgepackt und gepatcht).

| Datei | Zweck |
| --- | --- |
| `build.sh <firefox-tree> <out>` | baut `libnspr4.a`, `libplc4.a`, `libplds4.a` (Quellen und Defines wie Mozillas `config/external/nspr/*/moz.build` für ein pthreads-Unix, dazu `md/unix/aros.c`) |
| `tests/nsprtest.c`, `tests/build-test.sh` | Gasttest; Lauf mit `../rust/tests/run-guest.sh <out>/nsprtest 300` |

## Portierung

- `pr/include/md/_aros.h`, `_aros.cfg`, `pr/src/md/unix/aros.c`: nach der RISC-OS-Portierung (pthreads, `poll()`),
  64 Bit Big Endian, BSD-Sockets (`sa_len`, `AF_INET6` = 28), `flock`, Prozessorzahl über `KrnGetCPUCount`
  (kernel.resource; QRX OS One 64 hat keine processor.resource).
- AROS daneben in den Plattformlisten von `_pth.h`, `prosdep.h`, `unix.c`, `uxrng.c` (Zufall aus `clock_gettime`,
  AROS hat kein `times()`), `ptio.c`, `ptthread.c` (kein `dlsym` für Thread-Namen, kein `sigaction` für die alte
  GC-Unterstützung), `prsystem.c`, `unix_errors.c` (kein `EHOSTDOWN`); `config/external/nspr/prcpucfg.h`.
- **Sockets:** AROS' `<netdb.h>` zieht `proto/socket.h` an und macht `socket()`, `connect()` usw. zu Inline-Aufrufen
  von bsdsocket.library. Mit `-DBSDSOCKET_NOLIBDEFINES -D__BSDSOCKET_NOLIBBASE__` gehen sie an libqrxaroscompat
  (die Host-Netz-Tür des Boards), `h_errno` ist eine Variable dort.
- Nur IPv4 (die Tür löst IPv4 auf): `_PR_HAVE_GETADDRINFO` bleibt aus, NSPR nimmt `gethostbyname`; IPv6-Sockets
  bildet NSPRs eigene Schicht auf IPv4 ab. Kein Multicast (AROS' Header haben keine `IP_ADD_MEMBERSHIP`).
- Ergänzt in libqrxaroscompat: `readv`/`writev`, `select`/`WaitSelect` (auf `poll`), `gethostbyname`,
  `gethostbyaddr` (ohne Rückwärtsauflösung), `getprotobyname`/`-number`; im AROS-Patch `<poll.h>` und ein
  variadisches `sem_open` (POSIX).
- `PR_CreateProcess` (fork/exec) geht auf AROS nicht; Gecko braucht es nicht.

## Stand (25.11.2026)

Gast (qrxboot, zwei EC64-Kerne): **NSPR ALL PASS** — Systeminfo, 2 Prozessoren, Erzeuger/Verbraucher mit
Lock/CondVar über 5 Threads, wiedereintrittsfähiger Monitor, Thread-private Daten mit Destruktor, `PR_Sleep` und
Intervalle, Zeit zerlegen/parsen, Datei schreiben/lesen/Info/löschen, plds-Hash und plc-Strings, TCP-Echo über
127.0.0.1 mit `PR_Poll`, `localhost` → 127.0.0.1, Zufallsrauschen.
