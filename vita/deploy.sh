#!/bin/bash
# Quit the app on the Vita, upload the new eboot and launch it (needs vitacompanion).
set -e
VITA=${VITA:-192.168.1.66}
cd "$(dirname "$0")"
echo "quit SURF00001" | nc -q1 "$VITA" 1338 || true
sleep 2
curl -sS -T build-real/surf_vita.self "ftp://$VITA:1337/ux0:/app/SURF00001/eboot.bin"
echo "launch SURF00001" | nc -q1 "$VITA" 1338
