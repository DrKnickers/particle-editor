import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";

// ---- placeholder media (gitignored, never committed) -------------------------------
// ESM context (package is "type":"module") → use import.meta.url, NOT __dirname.
// Stems must match the data-clip/data-poster names in site/index.html (and global-setup.mjs).
const SITE = fileURLToPath(new URL("../../../../site", import.meta.url));
const MEDIA = resolve(SITE, "media-local");
const STEMS = ["hero", "faith", "f02-reorder", "f02", "f04"];
const REQUIRED = STEMS.flatMap((s) => [`${s}.mp4`, `${s}-poster.jpg`]).map((f) => resolve(MEDIA, f));
const HAS_MEDIA = REQUIRED.every((p) => existsSync(p));

// Point MEDIA_BASE at the locally-served placeholder media, before any page script runs.
test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => { (window as any).__MEDIA_BASE__ = "/media-local/"; });
});

test("structure: hero, 4 features, footer; no uncaught JS errors", async ({ page }) => {
  const jsErrors: string[] = [];
  page.on("pageerror", (e) => jsErrors.push(String(e))); // uncaught JS only — resource
                                                          // errors don't count
  await page.goto("/");
  await expect(page.locator("h1")).toHaveText("Effects, previewed in game context.");
  await expect(page.locator("section.feature")).toHaveCount(4);
  await expect(page.locator("header.topbar")).toBeVisible();
  await expect(page.locator("footer.site-footer")).toBeVisible();
  expect(jsErrors, jsErrors.join("\n")).toHaveLength(0);
});

test("clip slots: loop/muted/playsinline + reserved 4:3 dims + poster set", async ({ page }) => {
  await page.goto("/");
  const vids = page.locator("video.clip-video");
  await expect(vids).toHaveCount(STEMS.length); // hero + 4 features
  const n = await vids.count();
  for (let i = 0; i < n; i++) {
    const v = vids.nth(i);
    await expect(v).toHaveJSProperty("loop", true);
    await expect(v).toHaveJSProperty("muted", true);
    await expect(v).toHaveJSProperty("playsInline", true);
    await expect(v).toHaveAttribute("width", "1280");
    await expect(v).toHaveAttribute("height", "960");
    // poster is set from data-poster by main.js (reserves a real frame)
    await expect.poll(() => v.evaluate((el: HTMLVideoElement) => el.poster)).not.toBe("");
  }
});

test("links: download + source resolve; internal links + anchors exist", async ({ page }) => {
  await page.goto("/");
  // all three download CTAs (nav, hero, footer) point straight at the release
  for (const sel of ["header a.btn-primary", ".hero a.btn-primary", ".site-footer a.btn-primary"])
    await expect(page.locator(sel)).toHaveAttribute("href", /releases\/latest/);
  await expect(page.locator('header a[href="https://github.com/DrKnickers/particle-editor"]')).toHaveCount(1); // source
  const hrefs = await page.locator("a[href]").evaluateAll((els) =>
    els.map((e) => e.getAttribute("href") || ""));
  for (const h of hrefs) {
    if (h.startsWith("#")) await expect(page.locator(h)).toHaveCount(1); // anchor target exists
    else if (/^https?:\/\//.test(h)) continue;                           // external — not fetched
    else {
      // relative internal link (e.g. the Guide) must actually resolve on the served site
      const res = await page.request.get(new URL(h, page.url()).href);
      expect(res.status(), `internal link 404s: ${h}`).toBe(200);
    }
  }
});

test("a11y: no critical/serious axe violations", async ({ page }) => {
  await page.goto("/");
  // wait for the CSS entrance stagger to finish so axe sees the settled page
  await page.waitForFunction(() =>
    [...document.querySelectorAll(".hero > *, .feature > *, .site-footer > *")]
      .every((el) => getComputedStyle(el).opacity === "1"),
    null, { timeout: 5000 });
  const results = await new AxeBuilder({ page }).withTags(["wcag2a", "wcag2aa"]).analyze();
  const bad = results.violations.filter((v) => ["critical", "serious"].includes(v.impact ?? ""));
  expect(bad, JSON.stringify(bad.map((v) => v.id))).toHaveLength(0);
});

test("hero clip actually plays (best-effort autoplay)", async ({ page }) => {
  test.skip(!HAS_MEDIA, "ffmpeg unavailable — cannot generate placeholder media");
  await page.goto("/");
  const hero = page.locator("figure.clip-hero video");
  await expect.poll(() => hero.evaluate((v: HTMLVideoElement) =>
    !v.paused && v.readyState >= 2 /* HAVE_CURRENT_DATA */ && v.currentTime > 0),
    { timeout: 8000 }).toBe(true);
});

test("lazy loading: below-fold clips request nothing until scrolled in", async ({ page }) => {
  const clipReqs: string[] = [];
  page.on("request", (r) => {
    if (/\/(?:faith|f02-reorder|f02|f04)[^/]*\.mp4(?:[?#]|$)/.test(r.url())) clipReqs.push(r.url());
  });
  await page.goto("/");
  await page.waitForTimeout(600); // hero (eager) may load; below-fold must not
  expect(clipReqs, clipReqs.join("\n")).toHaveLength(0);
  // scrolling one in (motion on by default) triggers its lazy load via IntersectionObserver
  const last = page.locator('video[data-clip="f04.mp4"]');
  await last.scrollIntoViewIfNeeded();
  await expect.poll(() => last.evaluate((v: HTMLVideoElement) => v.currentSrc)).toContain("f04");
});

test("pause control: toggles playback, flips label, blocks lazy load while paused", async ({ page }) => {
  test.skip(!HAS_MEDIA, "ffmpeg unavailable");
  const lazyReqs: string[] = [];
  page.on("request", (r) => { if (/\/f04[^/]*\.mp4(?:[?#]|$)/.test(r.url())) lazyReqs.push(r.url()); });
  await page.goto("/");
  const btn = page.locator("#motion-toggle");
  await expect(btn).toBeVisible();
  await btn.focus();
  await expect(btn).toBeFocused();
  const hero = page.locator("figure.clip-hero video");
  await expect.poll(() => hero.evaluate((v: HTMLVideoElement) => !v.paused), { timeout: 8000 }).toBe(true);
  await btn.click();
  await expect(btn).toHaveText("Play"); // compact visible label; aria-label carries "Play motion"
  await expect(btn).toHaveAttribute("aria-label", "Play motion");
  await expect(hero).toHaveJSProperty("paused", true);
  // the below-fold lazy clip, scrolled in WHILE paused, must not load (empty currentSrc)
  const lazy = page.locator("video.clip-video").last();
  await lazy.scrollIntoViewIfNeeded();
  await page.waitForTimeout(400);
  expect(await lazy.evaluate((v: HTMLVideoElement) => v.currentSrc)).toBe("");
  expect(lazyReqs, lazyReqs.join()).toHaveLength(0);
});

test("reduced motion: no autoplay, no media request, fade-up off, control offers play", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  const media: string[] = [];
  page.on("request", (r) => { if (r.url().endsWith(".mp4")) media.push(r.url()); });
  await page.goto("/");
  for (const f of await page.locator("section.feature, footer").all())
    await f.scrollIntoViewIfNeeded();
  await page.waitForTimeout(500);
  const hero = page.locator("figure.clip-hero video");
  await expect(hero).toHaveJSProperty("paused", true);
  expect(await hero.evaluate((v: HTMLVideoElement) => v.currentSrc)).toBe("");
  expect(media, media.join("\n")).toHaveLength(0);
  const anim = await page.locator(".hero h1").evaluate((el) => getComputedStyle(el).animationName);
  expect(anim === "none" || anim === "").toBeTruthy();
  await expect(page.locator("#motion-toggle")).toHaveText("Play");
  await expect(page.locator("#motion-toggle")).toHaveAttribute("aria-label", "Play motion");
});

test("asset rule: no media binary tracked under site/", () => {
  const tracked = execFileSync("git", ["ls-files", "site/"], { cwd: SITE + "/.." })
    .toString().trim().split("\n").filter(Boolean);
  const leaked = tracked.filter((f) => /\.(mp4|webm|jpe?g|png|webp|gif)$/i.test(f));
  expect(leaked, `tracked media leaked: ${leaked.join(", ")}`).toHaveLength(0);
});
