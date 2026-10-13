# Surf for Vita

A PS Vita client for [Surf](https://github.com/seg6/surf): Chromium runs on a
computer and streams to the Vita as H.264, with touch, buttons, text input,
tabs and audio. Personal project, tested on a Vita 1000 (Ensō, HENkaku). Clone with `--recursive`
(Surf v0.17.0 is the `upstream` submodule).


## Use it

1. Download from [Releases](https://github.com/jmverges/surf-vita/releases): the
   server for your computer (Windows or Linux) and `SurfVita-*.vpk`.
2. Install the VPK on the Vita with VitaShell (HENkaku or Ensō needed).
3. Start the server on the computer (`surf.exe`, or `./surf serve` on Linux) and
   allow inbound TCP port 18080 in its firewall.
4. Pair: run `surf pair` (Windows: Paired Devices > Pair device), open Surf for
   Vita, enter the computer's `address:18080` and the six-digit code, and check
   the six words match on both screens.

Controls: tap = click, drag = scroll, triangle = address, square = type text,
L/R = back/forward, START = reload, SELECT = info bar and tabs.

## Build

- `vita/`: the client (C, VitaSDK). Build in Docker:
  `docker run --rm -v $PWD:/w -w /w/vita vitasdk/vitasdk:latest bash -c 'cmake -S . -B build-real -DSV_STUB_SESSION=OFF && cmake --build build-real'`.
  `vita/deploy.sh` installs a new build over FTP and relaunches it (needs vitacompanion on the Vita);
  `vita/stats-listen.py` receives the app's once-a-second debug counters (UDP 18999).
- `server/`: runs Surf 0.17.0 with the patches in `server/patches/` (fullscreen
  zoom, letterboxing from headless Chromium's toolbar, Vita defaults) for a 960x544 client.
  Build the patched Surf: `git -C upstream am ../server/patches/*.patch`, then in
  `upstream` with Go 1.26: `make surf-binary SURF_GOOS=linux SURF_GOARCH=amd64`
  (or windows/amd64, linux/arm64). `server/run.sh` is the developer setup on the
  author's machine. `server/apparmor-surf-chromium`
  lets the Playwright Chromium keep its sandbox on Ubuntu 24.04.
- Pair: `surf --home server/home pair` on the computer, then enter the
  address and code on the Vita and compare the six words.

Not included: `server/home` (server identity, paired devices, browser profile)
and `vita/test-home` (the Linux test client's key).

## License

MIT (see `LICENSE`). The Vita client compiles Surf's C client core
(`upstream/client/core`, MIT, Copyright (c) 2026 seg6) and links mbedTLS
(Apache-2.0) and vita2d (MIT) from VitaSDK.
