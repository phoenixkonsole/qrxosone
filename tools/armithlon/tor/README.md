# tor for AROS m68k

Builds tor 0.4.9.9 for QRX OS One under ARMithlon. Background, results and limits are in
[docs/tor-browser-machbarkeit-2026-11.md](../../../docs/tor-browser-machbarkeit-2026-11.md).
This is the tor daemon, not Tor Browser.

## Build

Put these in `<work dir>/dl`; `patches/SHA256SUMS` checks them:

- `tor-0.4.9.9.tar.gz` from dist.torproject.org
- `libevent-2.1.12-stable.tar.gz` from the libevent GitHub releases
- `openssl-3.5.4.tar.gz` from the OpenSSL GitHub releases

```sh
QRX_SDK_BIN=/path/to/.build/qrx-sdk/bin tools/armithlon/tor/build-tor.sh <work dir> all
m68k-elf-strip --strip-debug -o tor <work dir>/b-tor/src/app/tor
```

zlib comes from the SDK. `build-tor.sh <work dir> tests` builds tor's test program.

The binary needs an engine with this branch's door changes. On an older engine the first socket
or random call reads a door register that does not exist there, and the emulator stops with a
memory fault.

## Run

Copy `tor` and `torrc-qrx` into the system tree, create `SYS:QRX/Tor`, then:

```
Stack 2000000
SYS:QRX/Tor/tor -f SYS:QRX/Tor/torrc-qrx
```

The boot's own Startup-Sequence provides `ENV:`, `T:` and `C:QRXMoreMemory`. A test tree must set
them up itself. `torrc-qrx` has not been run against the real Tor network.

## Files

| File | Purpose |
| --- | --- |
| `qrx-tor-cc` | Compiler and linker wrapper around the SDK's m68k-elf toolchain |
| `shim/qrx_posix.c` | Sockets through door service 4, select, poll, host random bytes and clock, the libc repairs |
| `shim/qrx_alloc.c` | malloc on one semaphore-protected Exec pool |
| `shim/qrx_core1.c` | Optional X25519 on the second core, off unless `QRX_TOR_CORE1` is set |
| `shim/gen-bridges.py` | Bridges for libc functions that return pointers (d0 to a0) or doubles (d0/d1 to fp0) |
| `shim/include` | Headers in place of AROS's bsdsocket-bound ones, and what AROS lacks |
| `patches/tor-0.4.9.9-aros.patch` | AROS paths, private directory check, core 1 hook, test temp directory |
| `private-net.sh` | Private Tor network on the Mac's loopback, for tests without the real network |

## Test without the real network

```sh
tools/armithlon/tor/private-net.sh build <work dir>
tools/armithlon/tor/private-net.sh start <work dir>
```

`<work dir>/net/guest-torrc` is the guest's configuration. After about a minute the network has a
consensus. Point `curl --socks5-hostname 127.0.0.1:19050` at a server on 127.0.0.1. The exit relays
accept 127.0.0.1 only, and a name other than an address makes them use the Mac's resolver, so test
with addresses. `private-net.sh stop <work dir>` ends the relays.

Log output of tests in the guest: tor writes through `Log notice file`. Text on stderr does not
reach a file through the shell's `*>` here.
