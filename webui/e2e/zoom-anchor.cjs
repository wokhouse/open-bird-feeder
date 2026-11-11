// Zoom-reflow anchor e2e (PLAN 6k). Requires the ingest app serving the
// built SPA at http://127.0.0.1:8077 (open-bird-feeder-ingest unit) and
// the system Chromium:
//   cd webui && npm i -D playwright-core && node e2e/zoom-anchor.cjs
// Checks A/B: viewport 850<->400 keeps the anchor card at its viewport
// offset (drift must be 0px). C: back-to-back crossings run one restorer.
// D: a user wheel inside the correction window must stick (loop yields).
// Regression checks for the review fixes:
// A/B: both directions keep the anchor card (same as both.js).
// C: two breakpoint crossings back-to-back -> one restorer, anchor kept.
// D: user wheel inside the correction window -> user scroll sticks (loop yields).
const { chromium } = require('playwright-core')

const topCard = () =>
  [...document.querySelectorAll('[data-frame-path]')]
    .map((el) => ({ path: el.dataset.framePath, top: el.getBoundingClientRect().top }))
    .filter((c) => c.top >= -4 && c.top < window.innerHeight)
    .sort((a, b) => a.top - b.top)[0] || null

const at = (path) => {
  const el = document.querySelector(`[data-frame-path="${CSS.escape(path)}"]`)
  return el ? Math.round(el.getBoundingClientRect().top) : null
}

async function open(browser, w) {
  const page = await browser.newPage({ viewport: { width: w, height: 900 } })
  await page.goto('http://127.0.0.1:8077/?view=all', { waitUntil: 'networkidle' })
  await page.waitForSelector('[data-frame-path]', { timeout: 15000 })
  await page.evaluate(() => window.scrollTo(0, 12000))
  await page.waitForTimeout(900)
  return page
}

async function dir(browser, from, to) {
  const page = await open(browser, from)
  const before = await page.evaluate(topCard)
  const beforeTop = Math.round(before.top)
  await page.setViewportSize({ width: to, height: 900 })
  await page.waitForTimeout(1500)
  const r = await page.evaluate(
    ([p, t]) => {
      const el = document.querySelector(`[data-frame-path="${CSS.escape(p)}"]`)
      return el ? { mounted: true, drift: Math.abs(Math.round(el.getBoundingClientRect().top) - t) } : { mounted: false, drift: null }
    },
    [before.path, beforeTop],
  )
  await page.close()
  return { dir: `${from}->${to}`, ...r }
}

async function doubleCross(browser) {
  const page = await open(browser, 850)
  const before = await page.evaluate(topCard)
  const beforeTop = Math.round(before.top)
  await page.setViewportSize({ width: 400, height: 900 })
  await page.waitForTimeout(250) // first restore loop mid-flight
  await page.setViewportSize({ width: 620, height: 900 }) // second crossing
  await page.waitForTimeout(1800)
  const r = await page.evaluate(
    ([p, t]) => {
      const el = document.querySelector(`[data-frame-path="${CSS.escape(p)}"]`)
      return el ? { mounted: true, drift: Math.abs(Math.round(el.getBoundingClientRect().top) - t) } : { mounted: false, drift: null }
    },
    [before.path, beforeTop],
  )
  await page.close()
  return { check: 'double-crossing', ...r }
}

async function userScrollWins(browser) {
  const page = await open(browser, 850)
  const before = await page.evaluate(topCard)
  const keptTop = Math.round(before.top)
  await page.setViewportSize({ width: 400, height: 900 })
  await page.waitForTimeout(250) // correction loop running
  await page.mouse.wheel(0, -400) // user scrolls up mid-window
  await page.waitForTimeout(1200)
  const r = await page.evaluate(
    ([p, t]) => {
      const el = document.querySelector(`[data-frame-path="${CSS.escape(p)}"]`)
      return el ? { mounted: true, top: Math.round(el.getBoundingClientRect().top), kept: t } : { mounted: false, kept: t }
    },
    [before.path, keptTop],
  )
  await page.close()
  const want = keptTop + 400 // wheel(-400) scrolls up: content moves down
  return {
    check: 'user-scroll-during-window',
    ...r,
    expectedTop: want,
    userIntentHonored: r.mounted && Math.abs(r.top - want) <= 25,
  }
}

;(async () => {
  const browser = await chromium.launch({ executablePath: '/usr/bin/chromium', args: ['--no-sandbox'] })
  console.log(JSON.stringify(await dir(browser, 850, 400)))
  console.log(JSON.stringify(await dir(browser, 400, 850)))
  console.log(JSON.stringify(await doubleCross(browser)))
  console.log(JSON.stringify(await userScrollWins(browser)))
  await browser.close()
})()
