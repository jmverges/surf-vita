// Keeps Surf's capture surface the size the client asked for.
//
// Surf sizes Chromium's window to the client's viewport (960x544), but headless
// Chromium still lays out its tab strip and toolbar (87 px, 143 px with an
// infobar) inside that window. The tab Surf emulates at 960x544 is fine, but a
// tab without the emulation (one a link opened, for example) gets the real
// 960x401 area and reaches the Vita letterboxed. So: when any tab in a window
// is shorter than the viewport, grow the window by what's missing. Only the
// window's selected tab follows the window size: a tab that didn't grow after
// a resize is in the background and is ignored until its size changes. The
// window never grows past the viewport + MAX_EXTRA.
//
// Usage: node fit-window.cjs <SURF_HOME> [width] [height]
const fs = require('fs');
const path = require('path');
const { chromium } = require('/home/jmverges/BusyNoggin/GandyInk/Sites/go-gear-app/node_modules/playwright');

const home = process.argv[2];
const want = { w: Number(process.argv[3] || 960), h: Number(process.argv[4] || 544) };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const MAX_EXTRA = 200;
const ignored = new Map(); // targetId -> inner height it kept after a resize
const log = msg => console.log(new Date().toISOString(), msg);

async function session() {
  const port = fs.readFileSync(path.join(home, 'profile', 'DevToolsActivePort'), 'utf8').split('\n')[0];
  const browser = await chromium.connectOverCDP('http://127.0.0.1:' + port);
  let lastGrow = null; // {targetId, inner} of the tab that triggered the last resize
  for (;;) {
    // Every page: a tab a link opened can live in its own window.
    const pages = browser.contexts().flatMap(c => c.pages()).filter(p => !p.url().startsWith('chrome-extension:'));
    const windows = new Map(); // windowId -> {bounds, shortest, targetId}
    for (const page of pages) {
      const cdp = await page.context().newCDPSession(page);
      try {
        const { windowId, bounds } = await cdp.send('Browser.getWindowForTarget');
        const { targetInfo } = await cdp.send('Target.getTargetInfo');
        if (bounds.width !== want.w || bounds.windowState !== 'normal') continue;
        const inner = await page.evaluate(() => [innerWidth, innerHeight]);
        if (inner[0] !== want.w) continue;
        const id = targetInfo.targetId;
        if (lastGrow && lastGrow.targetId === id) { // did the resize reach it?
          if (inner[1] === lastGrow.inner) ignored.set(id, inner[1]);
          lastGrow = null;
        }
        if (ignored.has(id)) {
          if (ignored.get(id) === inner[1]) continue;
          ignored.delete(id);
        }
        const w = windows.get(windowId) || { bounds, shortest: Infinity, targetId: null };
        if (inner[1] < w.shortest) { w.shortest = inner[1]; w.targetId = id; }
        windows.set(windowId, w);
      } catch (e) {
        if (!/closed|detached|Target|context/.test(e.message)) throw e;
      } finally {
        await cdp.detach().catch(() => {});
      }
    }
    for (const [windowId, w] of windows) {
      if (w.shortest >= want.h) continue;
      const height = Math.min(w.bounds.height + (want.h - w.shortest), want.h + MAX_EXTRA);
      if (height <= w.bounds.height) continue;
      const cdp = await pages[0].context().newCDPSession(pages[0]);
      try {
        await cdp.send('Browser.setWindowBounds', { windowId, bounds: { height } });
        lastGrow = { targetId: w.targetId, inner: w.shortest };
        log(`window ${windowId}: a tab is ${want.w}x${w.shortest}, window ${w.bounds.height} -> ${height}`);
      } catch (e) {
        if (!/closed|detached|Target|context/.test(e.message)) throw e;
      } finally {
        await cdp.detach().catch(() => {});
      }
    }
    await sleep(500);
  }
}

(async () => {
  for (;;) { // reconnect when Surf restarts Chromium
    try { await session(); } catch (e) { log('retry: ' + e.message.split('\n')[0]); }
    await sleep(3000);
  }
})();
