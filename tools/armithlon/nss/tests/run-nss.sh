#!/bin/bash
# Runs nsstest in the QRX OS One 64 guest: crypto, then TLS to a local HTTPS server on the host
# (a fresh self-signed P-256 certificate, Python's ssl on 127.0.0.1:8443), which the guest reaches
# through the board's network door; the certificate goes in as T:cert.pem for the verification
# part. Usage: run-nss.sh <nsstest>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; W=$(mktemp -d)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout $W/key.pem -out $W/cert.pem \
  -days 2 -subj "/CN=localhost/O=QRX OS One test" 2>/dev/null
cat > $W/srv.py <<'PY'
import http.server, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[1] + "/cert.pem", sys.argv[1] + "/key.pem")
httpd = http.server.HTTPServer(("127.0.0.1", 8443), http.server.SimpleHTTPRequestHandler)
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
httpd.serve_forever()
PY
(cd $W && exec python3 -I srv.py $W) & SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf $W' EXIT
sleep 1
QRX_FILES=$W/cert.pem QRX_IMG_MB=64 QRX_ARGS="localhost 8443 T:cert.pem" \
  bash "$HERE/../../rust/tests/run-guest.sh" "$1" 1000
