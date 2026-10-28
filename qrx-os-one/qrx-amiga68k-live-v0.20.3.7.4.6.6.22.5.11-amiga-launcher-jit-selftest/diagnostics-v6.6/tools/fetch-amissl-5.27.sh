#!/usr/bin/env bash
set -Eeuo pipefail
VER=5.27; CACHE=${QRX_AMISSL_CACHE:-/opt/qrx-os-one/cache/amissl}; mkdir -p "$CACHE"
need(){ command -v "$1" >/dev/null || { echo "AmiSSL setup needs $1" >&2; exit 2; }; }
need python3
python3 - "$VER" "$CACHE" <<'PY2'
import json,sys,urllib.request,hashlib,pathlib
v,cache=sys.argv[1],pathlib.Path(sys.argv[2]); api=f'https://api.github.com/repos/jens-maus/amissl/releases/tags/{v}'
req=urllib.request.Request(api,headers={'User-Agent':'QRX-OS-AmiSSL-bootstrap'})
with urllib.request.urlopen(req,timeout=30) as r: data=json.load(r)
want=['AmiSSL-'+v+'-SDK.lha','AmiSSL-'+v+'-AmigaOS3.lha']
for name in want:
 a=next((x for x in data['assets'] if x['name']==name),None)
 if not a: raise SystemExit('missing official AmiSSL asset: '+name)
 dest=cache/name
 if not dest.exists():
  req=urllib.request.Request(a['browser_download_url'],headers={'User-Agent':'QRX-OS-AmiSSL-bootstrap'})
  with urllib.request.urlopen(req,timeout=120) as r: dest.write_bytes(r.read())
 digest='sha256:'+hashlib.sha256(dest.read_bytes()).hexdigest()
 published=a.get('digest')
 if published and published!=digest: raise SystemExit(f'digest mismatch {name}: {digest} != {published}')
 print(name,digest,'github-digest='+str(published))
PY2
cat <<EOF
AmiSSL 5.27 official archives cached in $CACHE.
Deployment intentionally does not silently install unverified files into AROS.
The packaging stage must extract SDK/runtime, compile the TLS adapter, and prove amisslmaster.library + CA store in final ISO before HTTPS is advertised.
EOF
