#!/bin/bash
# Headless Chromium assumes an 800x600 screen and squeezes the 960x544 window
# (the page rendered 960x401 and the Vita got black bars). Give it a larger screen.
exec /home/jmverges/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome "$@" --ozone-override-screen-size=1920,1080
