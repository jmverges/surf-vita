#!/bin/bash
# Surf backend for the Vita client: viewport 960x544, bitrate sized for the Vita's 2.4 GHz Wi-Fi.
cd "$(dirname "$0")"
export SURF_HOME="$PWD/home" SURF_SERVER_NAME="UM880" PORT=18080 VW=960 VH=544 STREAM_BITRATE=4000
export SURF_PUBLIC_ADDRESS="${SURF_PUBLIC_ADDRESS:-192.168.1.63:18080}"
export CHROME="${CHROME:-/home/jmverges/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome}"
pgrep -f "Xvfb :97" >/dev/null || (Xvfb :97 -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &); sleep 1
export DISPLAY=:97
# PoC only: Ubuntu 24.04 blocks the Chromium sandbox (AppArmor userns); the proper fix is an AppArmor profile.
export CHROME_NO_SANDBOX=1
exec ./surf-0.17.0-linux-amd64/surf serve
