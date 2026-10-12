#!/bin/bash
# The headless screen is the Vita's screen, so element fullscreen (a YouTube
# video maximized) fills exactly the 960x544 stream.
# No smooth scrolling: the Vita sends finger drags as frequent wheel steps,
# and each animated step cut the previous one short (jerky, distances lost).
exec /home/jmverges/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome "$@" --ozone-override-screen-size=960,544 --disable-smooth-scrolling
