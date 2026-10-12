// Keeps Surf's capture surface the size the client asked for.
//
// Surf sizes Chromium's window to the client's viewport (960x544), but headless
// Chromium still lays out its tab strip and toolbar (87 px, plus 56 px when an
// infobar shows) inside that window, so the page area is 960x457 or 960x401 and
// the video reaches the Vita letterboxed. Every second this measures the real
// page area (a screenshot is the compositor surface, which Surf's emulation
// doesn't hide) and grows the window by what's missing.
//
// Usage: node fit-window.cjs <SURF_HOME> [width] [height]
const fs = require('fs');
const path = require('path');
const { chromium } = require('/home/jmverges/BusyNoggin/GandyInk/Sites/go-gear-app/node_modules/playwright');

const home = process.argv[2];
const want = { w: Number(process.argv[3] || 960), h: Number(process.argv[4] || 544) };
const sleep = ms => new Promise(r => setTimeout(r, ms));

function pngSize(buf) { return { w: buf.readUInt32BE(16), h: buf.readUInt32BE(20) }; }

async function session() {
  const port = fs.readFileSync(path.join(home, 'profile', 'DevToolsActivePort'), 'utf8').split('\n')[0];
  const browser = await chromium.connectOverCDP('http://127.0.0.1:' + port);
  for (;;) {
    const pages = browser.contexts().flatMap(c => c.pages());
    const page = pages.find(p => !p.url().startsWith('chrome-extension:')) || pages[0];
    if (page) {
      const cdp = await page.context().newCDPSession(page);
      try {
        const shot = await cdp.send('Page.captureScreenshot', { format: 'png', clip: undefined });
        const real = pngSize(Buffer.from(shot.data, 'base64'));
        const { windowId, bounds } = await cdp.send('Browser.getWindowForTarget');
        if (bounds.width === want.w && real.h < want.h) {
          const height = bounds.height + (want.h - real.h);
          await cdp.send('Browser.setWindowBounds', { windowId, bounds: { height } });
          console.log(new Date().toISOString(), `page ${real.w}x${real.h}: window ${bounds.height} -> ${height}`);
        }
      } finally {
        await cdp.detach().catch(() => {});
      }
    }
    await sleep(1000);
  }
}

(async () => {
  for (;;) { // reconnect when Surf restarts Chromium
    try { await session(); } catch (e) { console.log(new Date().toISOString(), 'retry:', e.message.split('\n')[0]); }
    await sleep(3000);
  }
})();
