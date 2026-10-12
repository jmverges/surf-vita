#!/bin/bash
# The headless screen is the Vita's screen, so element fullscreen (a YouTube
# video maximized) fills exactly the 960x544 stream. Normal windows are kept at
# a 960x544 page area by fit-window.cjs.
exec /home/jmverges/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome "$@" --ozone-override-screen-size=960,544
