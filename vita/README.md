# Surf for Vita

A [Surf](https://github.com/seg6/surf) client for the PS Vita: a remote Chromium
running on your computer, streamed to the Vita as H.264 and decoded by the
Vita's hardware AVC decoder. Touch, stick, buttons and the system keyboard are
sent back as input.

**Status: untested on hardware.** It builds; the app shell and the stub session
have not run on a real Vita yet, and the hardware decoder path can only be
validated on a real console.

## Run the backend

On the computer (from a Surf host release, https://github.com/seg6/surf/releases/latest):

```sh
./surf serve        # listens on TCP 18080 by default
surf pair           # in another terminal: prints a 6-digit code and 6 words
```

Allow inbound TCP 18080 through the firewall. On the Vita, enter
`<computer-ip>:18080`, then the 6-digit code, and check that the 6 words on
the Vita match the ones `surf pair` shows before pressing X. Each `surf pair`
invitation accepts one device.

## Build

Uses the `vitasdk/vitasdk:latest` image. vita2d, libpng, libjpeg-turbo, zlib
and mbedtls are vdpm packages that already ship in that image (checked with
`vdpm list`), so nothing is vendored or installed at build time.

```sh
# stub session: fake status + a looped sample H.264 stream, no backend needed
docker run --rm -v "$PWD/..":/w -w /w/vita vitasdk/vitasdk:latest \
  bash -c 'cmake -S . -B build -DSV_STUB_SESSION=ON && cmake --build build -j8'

# real session (src/session.c + src/session_*.c + upstream client/core)
docker run --rm -v "$PWD/..":/w -w /w/vita vitasdk/vitasdk:latest \
  bash -c 'cmake -S . -B build-real -DSV_STUB_SESSION=OFF && cmake --build build-real -j8'
```

Output: `build/surf_vita.vpk` (title ID `SURF00001`). The build directory is
owned by root when built in Docker; `chown -R $(id -u):$(id -g) build` if needed.

In the stub build, pairing code `000000` fails, any other code succeeds;
navigating to `error` or `pause` fakes those states.

## Install

Copy the VPK to the Vita (VitaShell: press SELECT to start its FTP server) and
install it from VitaShell. Needs HENkaku/h-encore with unsafe homebrew enabled.

## Dev workflow

- **vitacompanion** (https://github.com/devnoname120/vitacompanion): a taiHEN
  plugin with an FTP server on port 1337 and a command server on port 1338.
  Install the VPK once with VitaShell, then iterate by replacing the eboot:

  ```sh
  VITA=192.168.1.x
  echo 'quit SURF00001' | nc $VITA 1338
  curl -T build/surf_vita.self ftp://$VITA:1337/ux0:/app/SURF00001/eboot.bin
  echo 'launch SURF00001' | nc $VITA 1338
  ```

  vitacompanion has no VPK install command; LiveArea assets or param.sfo
  changes still need a VPK reinstall from VitaShell. It can also inject input
  (`press cross`, `press front-touch 0 960 544`, `press left-stick 128 0`).
- **Logs**: the app logs with `printf`. PrincessLog (in
  https://github.com/TeamFAPS/PSVita-RE-tools) captures stdout of homebrew
  and sends it over the network to a listener on your computer.
- **Vita3K** (https://github.com/Vita3K/Vita3K): fine for quick checks of the
  pairing screens, overlay and keyboard flow. The hardware decoder,
  physically contiguous memory and real touch/network timing must be tested
  on a real Vita.

## Controls

| Input | Action |
|---|---|
| Front touch | tap / drag on the page (moves sent at ~30 Hz) |
| Left stick | scroll, speed by tilt (~30 Hz) |
| D-pad up / down | scroll 120 px |
| Triangle | address bar (system keyboard, prefilled with the current URL) |
| Square | type text into the focused field |
| Cross | Enter (on error screens: retry) |
| Circle | Backspace |
| L / R | back / forward |
| START | reload |
| SELECT | toggle the info bar (title, URL, loading, state, decoder counters) |

## Limits

- Viewport fixed at 960x544; streams larger than that are dropped.
- Video only, no audio. No tabs, file dialogs, clipboard or downloads.
- If the decoder falls behind or a decode fails, frames are dropped until the
  next IDR. The session contract has no "request keyframe" call, so recovery
  waits for the backend's next IDR.
- Text input goes through the system keyboard (English layout by default).
