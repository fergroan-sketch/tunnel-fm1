#!/bin/sh
# The GitHub Pages site (the gh-pages branch): fm1-base's site (front page, web installer, firmware) and play/, the
# firmware in the browser through fm1-emu. Run from the folder that holds fm1-base/ and tunnel-fm1/, after a release
# build (python3 fm1-base/tools/build.py tunnel-fm1/app --release X.Y):
#   sh tunnel-fm1/tools/make_pages.sh X.Y        -> tunnel-fm1/site/
# fm1-base's make_site.py also wants the Emscripten emulator (build.py --web); without Emscripten a stand-in lets it
# run, and play/ (fm1-emu) stands in for the emulator.
set -e
V="$1"
[ -n "$V" ] || { echo "usage: sh tunnel-fm1/tools/make_pages.sh X.Y"; exit 2; }
cd "$(dirname "$0")/../.."
PY=fm1-base/.venv/bin/python
[ -x "$PY" ] || PY=python3
EMU=tunnel-fm1/app/build/emu
STUB=
if [ ! -f "$EMU/app.wasm" ]; then
    mkdir -p "$EMU" && : > "$EMU/app.wasm" && STUB=1
fi
"$PY" fm1-base/tools/make_site.py tunnel-fm1/app "tunnel-fm1/app/build/tunnelfm1-$V.fwsc" "$V" tunnel-fm1/site
if [ -n "$STUB" ]; then
    rm -rf tunnel-fm1/site/emu "$EMU"
fi
mkdir -p tunnel-fm1/site/play
sed "s/^const VERSION = \".*\";/const VERSION = \"$V\";/" tunnel-fm1/app/site/play.html > tunnel-fm1/site/play/index.html
echo "pages: tunnel-fm1/site (play/ plays tunnelfm1-$V.fwsc)"
