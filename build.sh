#!/bin/sh
# Build TUNNEL FM-1 on a Mac or Linux: run from the FM1 folder that holds fm1-base/ and tunnel-fm1/.
#   sh tunnel-fm1/build.sh          the firmware: tunnel-fm1/app/build/tunnelfm1.fwsc
#   sh tunnel-fm1/build.sh host     the host simulator, then its checks
set -e
cd "$(dirname "$0")/.."
export PATH="$HOME/.docker/bin:$PATH"
export AC79_SDK="${AC79_SDK:-$PWD/fm1-base/.toolchain/ac79-sdk-full}"
PY=fm1-base/.venv/bin/python
[ -x "$PY" ] || PY=python3
if [ "$1" = host ]; then
    "$PY" fm1-base/tools/build.py tunnel-fm1/app --host
    for t in basic features; do
        tunnel-fm1/app/build/host/tunnelfm1_host "tunnel-fm1/app/tests/$t.txt" /tmp/tunnelout
    done
else
    "$PY" fm1-base/tools/build.py tunnel-fm1/app
fi
