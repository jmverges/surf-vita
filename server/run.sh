#!/bin/bash
# Surf backend for the Vita client: viewport 960x544, bitrate sized for the Vita's 2.4 GHz Wi-Fi.
cd "$(dirname "$0")"
export SURF_HOME="$PWD/home" SURF_SERVER_NAME="UM880" PORT=18080 VW=960 VH=544 STREAM_BITRATE=4000
export SURF_PUBLIC_ADDRESS="${SURF_PUBLIC_ADDRESS:-192.168.1.63:18080}"
export CHROME="${CHROME:-$PWD/chrome-wrapper.sh}"
# Chromium runs headless; its toolbar still takes part of the window, so keep
# the page area at the Vita's 960x544 (see fit-window.cjs).
pgrep -f "node fit-window.cjs" >/dev/null || (setsid node fit-window.cjs "$SURF_HOME" 960 544 >> fit-window.log 2>&1 &)
# Surf 0.17.0 + vita-fixes (upstream/ branch vita-fixes): unrotated landscape, view re-applied on fullscreen.
exec ./surf-vita-patched serve
