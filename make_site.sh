#!/bin/sh
# Assembles the static site (no game files) from web/ and build_web/www: site/index.html + site/build/recomp.{js,wasm}.
# Serve it with COOP/COEP headers (threads need SharedArrayBuffer): python3 web/serve.py   or   python3 site_serve.py in site/.
set -e
R=$(cd "$(dirname "$0")" && pwd)
rm -rf "$R/site"; mkdir -p "$R/site/build"
cp "$R"/web/index.html "$R"/web/loader.js "$R"/web/audio-worklet.js "$R"/web/netplay.js "$R/site/"
cp "$R"/build_web/www/recomp.js "$R"/build_web/www/recomp.wasm "$R/site/build/"
cp "$R/web/serve.py" "$R/site/serve.py"
python3 - "$R/site/serve.py" <<'P'
import sys
p=sys.argv[1]; s=open(p).read()
s=s.replace('BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")','BUILD = os.path.join(HERE, "build")')
open(p,'w').write(s)
P
echo "site/ ready: cd site && python3 serve.py   (then open http://localhost:8007/)"
