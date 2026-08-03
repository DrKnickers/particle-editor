// Detects the intermittent WHITE FLASH when navigating between the landing page ("/") and
// the guide ("/guide/home.html") under cross-document View Transitions.
//
// Why pixels, not computed styles: getComputedStyle can report the correct dark background
// while Chromium still briefly paints a white canvas during a skipped/degraded transition.
// So we sample the ACTUAL painted top-left region on a tight loop across each navigation and
// assert it never goes light. Stress (CPU throttle + delayed styles.css + cache off) makes the
// otherwise-intermittent flash reproducible — the flash tracks whether styles.css (which is
// where @view-transition + the dark backstop live) has arrived before the swap.
//
// Run:  pnpm --filter @particle-editor/editor test:site -- view-transition-flash.spec.ts
// Knobs (env): VT_ITERATIONS, VT_STYLE_DELAY_MS, VT_CPU_THROTTLE, VT_MAX_LUMA
import { test, expect, type Page } from "@playwright/test";
import { inflateSync } from "node:zlib";

// Default sized for the routine site lane (~15s); crank VT_ITERATIONS (e.g. 24+) with
// VT_STYLE_DELAY_MS/VT_CPU_THROTTLE for a dedicated flash hunt.
const ITERATIONS   = Number(process.env.VT_ITERATIONS ?? 6);
const STYLE_DELAY  = Number(process.env.VT_STYLE_DELAY_MS ?? 160);
const CPU_THROTTLE = Number(process.env.VT_CPU_THROTTLE ?? 4);
const MAX_LUMA     = Number(process.env.VT_MAX_LUMA ?? 170); // dark bg #0b0d12 ≈ luma 13; white = 255
const SAMPLE_MS    = 8;
const POST_NAV_MS  = 400;
const CLIP = { x: 0, y: 0, width: 64, height: 64 };

// Minimal PNG → RGBA decoder (Node built-ins only; no new dep). Handles 8-bit truecolor
// (colorType 2/6) with the five standard scanline filters, which is what Chromium emits.
function pngLuma(png: Buffer): number {
  if (png.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a") throw new Error("not a png");
  let off = 8, width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat: Buffer[] = [];
  while (off < png.length) {
    const len = png.readUInt32BE(off); off += 4;
    const type = png.subarray(off, off + 4).toString("ascii"); off += 4;
    const data = png.subarray(off, off + len); off += len + 4; // +crc
    if (type === "IHDR") { width = data.readUInt32BE(0); height = data.readUInt32BE(4); bitDepth = data[8]; colorType = data[9]; }
    else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
  }
  if (bitDepth !== 8 || ![2, 6].includes(colorType)) throw new Error(`unsupported png bd=${bitDepth} ct=${colorType}`);
  const ch = colorType === 6 ? 4 : 3, stride = width * ch;
  const raw = inflateSync(Buffer.concat(idat)), rec = Buffer.alloc(height * stride);
  let src = 0, maxLuma = 0;
  for (let y = 0; y < height; y++) {
    const filter = raw[src++], row = y * stride;
    for (let x = 0; x < stride; x++) {
      const left = x >= ch ? rec[row + x - ch] : 0;
      const up = y > 0 ? rec[row - stride + x] : 0;
      const ul = y > 0 && x >= ch ? rec[row - stride + x - ch] : 0;
      const v = raw[src++];
      let p = 0;
      if (filter === 1) p = left;
      else if (filter === 2) p = up;
      else if (filter === 3) p = (left + up) >> 1;
      else if (filter === 4) { const pa = Math.abs(up - ul), pb = Math.abs(left - ul), pc = Math.abs(left + up - 2 * ul); p = pa <= pb && pa <= pc ? left : pb <= pc ? up : ul; }
      else if (filter !== 0) throw new Error(`bad filter ${filter}`);
      rec[row + x] = (v + p) & 255;
    }
  }
  for (let i = 0; i < rec.length; i += ch) {
    const l = 0.2126 * rec[i] + 0.7152 * rec[i + 1] + 0.0722 * rec[i + 2];
    if (l > maxLuma) maxLuma = l;
  }
  return maxLuma;
}

// Sample the painted top-left region continuously while `action` (a navigation) runs.
// Returns the worst luma seen AND how many samples succeeded — the caller asserts a floor
// on the count so an environment where every screenshot fails can't false-green the test.
async function sampleDuring(page: Page, action: () => Promise<unknown>): Promise<{ worst: number; samples: number }> {
  let stop = false, worstLuma = 0, samples = 0;
  const loop = (async () => {
    const deadline = Date.now() + 6000;
    while (!stop && Date.now() < deadline) {
      try {
        worstLuma = Math.max(worstLuma, pngLuma(await page.screenshot({ clip: CLIP, scale: "css", animations: "allow", timeout: 2000 })));
        samples++;
      }
      catch { /* navigation race — ignore */ }
      await page.waitForTimeout(SAMPLE_MS).catch(() => {});
    }
  })();
  try { await action(); await page.waitForTimeout(POST_NAV_MS); }
  finally { stop = true; await loop; }
  return { worst: worstLuma, samples };
}

test("cross-page view transition never paints a light frame", async ({ page, context }) => {
  test.setTimeout(Math.max(120_000, ITERATIONS * 4000));

  // Prove the VT path is actually exercised (else the test is vacuous).
  const vtActivated: boolean[] = [];
  await page.exposeFunction("__vtSeen", (had: boolean) => vtActivated.push(had));
  await page.addInitScript(() => {
    addEventListener("pagereveal", (e: any) => (window as any).__vtSeen?.(!!e.viewTransition));
  });

  // Stress: disable cache + throttle CPU so the transition path is fragile and the flash surfaces.
  const cdp = await context.newCDPSession(page);
  await cdp.send("Network.enable");
  await cdp.send("Network.setCacheDisabled", { cacheDisabled: true });
  await cdp.send("Emulation.setCPUThrottlingRate", { rate: CPU_THROTTLE });

  let delayStyles = false;
  await page.route(/\/styles\.css(?:[?#]|$)/, async (r) => {
    if (delayStyles) await new Promise((res) => setTimeout(res, STYLE_DELAY));
    await r.continue();
  });

  await page.setViewportSize({ width: 1280, height: 720 });
  await page.goto("/", { waitUntil: "load" });
  await expect(page.locator("header.topbar")).toBeVisible();
  delayStyles = true; // only slow navigations, not the cold boot

  let worst = 0, totalSamples = 0;
  const offenders: string[] = [];
  for (let i = 0; i < ITERATIONS; i++) {
    const toGuide = await sampleDuring(page, () => Promise.all([
      page.waitForURL("**/guide/home.html"),
      page.locator('header.topbar a[href="guide/home.html"]').click(),
    ]));
    const toHome = await sampleDuring(page, () => Promise.all([
      // Glob, not a hardcoded port — the suite's port is SITE_PORT-overridable (see
      // playwright.site.config.ts) so concurrent worktrees don't share a server.
      page.waitForURL(/\/(index\.html)?$/),
      page.goBack({ waitUntil: "load" }),
    ]));
    worst = Math.max(worst, toGuide.worst, toHome.worst);
    totalSamples += toGuide.samples + toHome.samples;
    if (toGuide.worst > MAX_LUMA) offenders.push(`iter ${i} home→guide: luma ${toGuide.worst.toFixed(0)}`);
    if (toHome.worst > MAX_LUMA) offenders.push(`iter ${i} guide→home: luma ${toHome.worst.toFixed(0)}`);
  }

  const activated = vtActivated.filter(Boolean).length;
  console.log(`[vt-flash] worst top-left luma across ${ITERATIONS}×2 navs: ${worst.toFixed(0)} (threshold ${MAX_LUMA}); ` +
    `${totalSamples} pixel samples; VT active on ${activated}/${vtActivated.length} reveals`);
  // Floor on successful samples: if screenshots silently fail, worst stays 0 and the test
  // would false-green — require at least a few samples per navigation on average.
  expect(totalSamples, "pixel sampler captured too few frames to be meaningful").toBeGreaterThan(ITERATIONS * 4);
  expect(vtActivated.some(Boolean), "cross-document View Transition must activate at least once").toBe(true);
  expect(offenders, `light frames detected:\n${offenders.join("\n")}`).toEqual([]);
});
