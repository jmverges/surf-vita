# Surf for Vita

A PS Vita client for [Surf](https://github.com/seg6/surf): Chromium runs on a
computer and streams to the Vita as H.264, with touch, buttons, text input,
tabs and audio. Personal project, working on a Vita 1000 (Ensō, HENkaku).

- `vita/`: the client (C, VitaSDK). Build in Docker:
  `docker run --rm -v $PWD:/w -w /w/vita vitasdk/vitasdk:latest bash -c 'cmake -S . -B build-real -DSV_STUB_SESSION=OFF && cmake --build build-real'`.
  `vita/deploy.sh` installs a new build over FTP and relaunches it (needs vitacompanion on the Vita);
  `vita/stats-listen.py` receives the app's once-a-second debug counters (UDP 18999).
- `server/`: runs Surf 0.17.0 with the patches in `server/patches/` (fullscreen
  zoom, letterboxing from headless Chromium's toolbar) for a 960x544 client.
  Build the patched Surf from seg6/surf `v0.17.0` + the patches
  (`golang:1.26`, `CGO_ENABLED=0 go build ./cmd/surf` in `backend/`), save it as
  `server/surf-vita-patched`, then `server/run.sh`. `server/apparmor-surf-chromium`
  lets the Playwright Chromium keep its sandbox on Ubuntu 24.04.
- Pair: `surf --home server/home pair` on the computer, then enter the
  address and code on the Vita and compare the six words.

Not included: `server/home` (server identity, paired devices, browser profile)
and `vita/test-home` (the Linux test client's key).
