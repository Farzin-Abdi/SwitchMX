#!/bin/bash
# Sends SwitchMX.nro to a console running SwitchMX in developer mode and checks it. It runs the next
# time the app is opened (pass "exit" as the second argument to close the app now).
#   SWITCHMX_DEV="<ip>:8080 <token>" tools/devdeploy.sh [pin]
set -e
cd "$(dirname "$0")/.."
# Only a build that is newer than every source file is sent.
[ -z "$(find source -newer SwitchMX.nro -name '*.[ch]*' | head -1)" ] || { echo "SwitchMX.nro is older than the sources: build first"; exit 1; }
read HOST TOKEN <<< "$SWITCHMX_DEV"
PIN=${1:-$(curl -s -H "X-Dev-Token: $TOKEN" "http://$HOST/dev/status" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("webPin",""))')}
[ -n "$PIN" ] || { echo "no PIN (pass it as the first argument)"; exit 1; }
DIR=sdmc%3A%2Fswitch%2FSwitchMX%2F
curl -s --fail --max-time 900 -T SwitchMX.nro -H "X-Pin: $PIN" "http://$HOST/api/upload?path=$DIR&name=SwitchMX.nro&exists=replace" >/dev/null
L=$(md5 -q SwitchMX.nro)
R=$(curl -s --max-time 600 -H "X-Pin: $PIN" "http://$HOST/api/file?path=${DIR}SwitchMX.nro" | md5 -q)
[ "$L" = "$R" ] || { echo "md5 mismatch: $R"; exit 1; }
echo "uploaded ($L)"
if [ "$2" = exit ]; then curl -s -X POST -H "X-Dev-Token: $TOKEN" "http://$HOST/dev/exit"; echo; fi
