#!/usr/bin/env bash
# A private Tor network on the Mac's loopback, for testing the guest's tor without contacting
# the real one: three directory authorities that also relay and exit, and two relays.
# Everything binds to 127.0.0.1, and nothing here knows a public relay or fallback directory.
#
# Usage: private-net.sh build <work dir>      native libevent and tor from the same sources
#        private-net.sh start <work dir>      writes <work dir>/net/guest-torrc
#        private-net.sh stop  <work dir>
set -Eeuo pipefail
CMD="${1:?build, start or stop}"; WORK="$(cd "${2:?work dir}" && pwd)"
NATIVE="$WORK/native"; NET="$WORK/net"; TOR="$NATIVE/tor/src/app/tor"; GENCERT="$NATIVE/tor/src/tools/tor-gencert"
OR_BASE=17000; DIR_BASE=17100

build() {
  mkdir -p "$NATIVE/prefix" "$NATIVE/libevent" "$NATIVE/tor"
  (cd "$NATIVE/libevent"
   "$WORK/src/libevent-2.1.12-stable/configure" --prefix="$NATIVE/prefix" --disable-shared --disable-openssl --disable-samples --disable-libevent-regress >configure.log 2>&1
   make -j8 >make.log 2>&1; make install >>make.log 2>&1)
  (cd "$NATIVE/tor"
   "$WORK/src/tor-0.4.9.9/configure" --with-libevent-dir="$NATIVE/prefix" --with-openssl-dir="$(brew --prefix openssl@3)" \
     --disable-asciidoc --disable-manpage --disable-html-manual --disable-unittests --disable-lzma --disable-zstd --disable-seccomp >configure.log 2>&1
   make -j8 src/app/tor src/tools/tor-gencert >make.log 2>&1)
  "$TOR" --version | head -1
}

common() {
  cat <<EOF
TestingTorNetwork 1
DataDirectory $1
PidFile $1/pid
RunAsDaemon 1
Log notice file $1/notice.log
Address 127.0.0.1
OutboundBindAddress 127.0.0.1
ConnLimit 60
SafeLogging 0
AssumeReachable 1
ClientUseIPv6 0
UseDefaultFallbackDirs 0
PathsNeededToBuildCircuits 0.25
SocksPort 0
ExitPolicy accept 127.0.0.1:*
ExitPolicyRejectPrivate 0
ExitRelay 1
V3AuthVotingInterval 20
V3AuthVoteDelay 4
V3AuthDistDelay 4
V3AuthNIntervalsValid 2
TestingV3AuthInitialVotingInterval 20
TestingV3AuthInitialVoteDelay 4
TestingV3AuthInitialDistDelay 4
TestingDirAuthVoteExit *
TestingDirAuthVoteGuard *
TestingDirAuthVoteHSDir *
EOF
}

start() {
  stop || true
  rm -rf "${NET:?}"; mkdir -p "$NET"
  local dirauths="" i
  # Authority identities first: every torrc needs all of the DirAuthority lines.
  for i in 0 1 2; do
    local d="$NET/auth$i"; mkdir -p "$d/keys"; chmod 700 "$d"
    (cd "$d/keys"; printf 'x\n' | "$GENCERT" --create-identity-key -m 12 -a "127.0.0.1:$((DIR_BASE+i))" --passphrase-fd 0 >/dev/null 2>&1)
    { common "$d"; echo "Nickname auth$i"; echo "ORPort 127.0.0.1:$((OR_BASE+i))"; echo "DirPort 127.0.0.1:$((DIR_BASE+i))"; } >"$d/torrc-nodir"
    local fp v3
    fp=$("$TOR" --list-fingerprint --hush -f "$d/torrc-nodir" --DirAuthority "x orport=1 no-v2 v3ident=0000000000000000000000000000000000000000 127.0.0.1:1 0000000000000000000000000000000000000000" | awk '{print $2 $3 $4 $5 $6 $7 $8 $9 $10 $11}' | tail -1)
    v3=$(awk '/^fingerprint/{print $2}' "$d/keys/authority_certificate")
    dirauths+="DirAuthority auth$i orport=$((OR_BASE+i)) no-v2 v3ident=$v3 127.0.0.1:$((DIR_BASE+i)) $fp"$'\n'
  done
  printf '%s' "$dirauths" >"$NET/dirauths"
  for i in 0 1 2; do
    local d="$NET/auth$i"
    { cat "$d/torrc-nodir"; printf '%s' "$dirauths"; echo "AuthoritativeDirectory 1"; echo "V3AuthoritativeDirectory 1"; echo "ContactInfo private-net"; } >"$d/torrc"
    "$TOR" -f "$d/torrc" --hush
  done
  for i in 3 4; do
    local d="$NET/relay$i"; mkdir -p "$d"; chmod 700 "$d"
    { common "$d"; printf '%s' "$dirauths"; echo "Nickname relay$i"; echo "ORPort 127.0.0.1:$((OR_BASE+i))"; } >"$d/torrc"
    "$TOR" -f "$d/torrc" --hush
  done
  { printf '%s' "$dirauths"
    cat <<EOF
TestingTorNetwork 1
UseDefaultFallbackDirs 0
PathsNeededToBuildCircuits 0.25
ClientUseIPv6 0
DataDirectory RAM:tordata
Log notice file SYS:tor-notices.log
SocksPort 127.0.0.1:19050
ControlPort 127.0.0.1:19051
CookieAuthentication 0
ConnLimit 32
SafeSocks 0
EOF
  } >"$NET/guest-torrc"
  echo "private network started in $NET"
}

stop() {
  local p
  for p in "$NET"/*/pid; do [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; done
  return 0
}

"$CMD"
