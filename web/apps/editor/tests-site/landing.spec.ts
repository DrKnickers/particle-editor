import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";

// ---- placeholder media (gitignored, never committed) -------------------------------
// ESM context (package is "type":"module") → use import.meta.url, NOT __dirname.
const SITE = fileURLToPath(new URL("../../../../site", import.meta.url));
const MEDIA = resolve(SITE, "media-local");
const CLIPS = ["hero.mp4", "faithful.mp4"];
const POSTERS = ["hero-poster.jpg", "faithful-poster.jpg", "preview-poster.jpg", "workspace-poster.jpg"];
const REQUIRED = [...CLIPS, ...POSTERS].map((f) => resolve(MEDIA, f));
const HAS_MEDIA = REQUIRED.every((p) => existsSync(p));

// Point MEDIA_BASE at the locally-served placeholder media, before any page script runs.
test.beforeEach(async ({ page }) => {
  await page.route(/fonts\.(googleapis|gstatic)\.com/, (r) => r.abort());
  await page.addInitScript(() => { (window as any).__MEDIA_BASE__ = "/media-local/"; });
});

test("structure: hero, 3 features, footer; no uncaught JS errors", async ({ page }) => {
  const jsErrors: string[] = [];
  page.on("pageerror", (e) => jsErrors.push(String(e))); // uncaught JS only — font/CDN
                                                          // resource errors don't count
  await page.goto("/");
  await expect(page.locator("h1")).toHaveText("Effects, rendered faithfully.");
  await expect(page.locator("section.feature")).toHaveCount(3);
  await expect(page.locator("header.topbar")).toBeVisible();
  await expect(page.locator("footer.site-footer")).toBeVisible();
  expect(jsErrors, jsErrors.join("\n")).toHaveLength(0);
});

test("clip slots: loop/muted/playsinline + reserved dims + poster set", async ({ page }) => {
  await page.goto("/");
  const vids = page.locator("video.clip-video");
  const n = await vids.count();
  expect(n).toBeGreaterThanOrEqual(2); // hero + 01
  for (let i = 0; i < n; i++) {
    const v = vids.nth(i);
    await expect(v).toHaveJSProperty("loop", true);
    await expect(v).toHaveJSProperty("muted", true);
    await expect(v).toHaveJSProperty("playsInline", true);
    await expect(v).toHaveAttribute("width", "1280");
    await expect(v).toHaveAttribute("height", "720");
    // poster is set from data-poster by main.js (reserves a real frame)
    await expect.poll(() => v.evaluate((el: HTMLVideoElement) => el.poster)).not.toBe("");
  }
});

test("links: download + source resolve; internal anchors exist", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator(".site-footer a.btn-primary")).toHaveAttribute("href", /releases\/latest/);
  await expect(page.locator('header a[href*="github.com/DrKnickers/particle-editor"]')).toHaveCount(1);
  const hrefs = await page.locator("a[href]").evaluateAll((els) =>
    els.map((e) => e.getAttribute("href") || ""));
  for (const h of hrefs) {
    if (h.startsWith("#")) await expect(page.locator(h)).toHaveCount(1); // anchor target exists
    else expect(h, `non-anchor href not absolute: ${h}`).toMatch(/^https?:\/\//);
  }
});

test("a11y: no critical/serious axe violations", async ({ page }) => {
  await page.goto("/");
  await page.waitForFunction(() =>
    [...document.querySelectorAll(".reveal")].every((el) => getComputedStyle(el).opacity === "1"),
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

test("poster-only 02/03 render <img> and never request video", async ({ page }) => {
  const badReq: string[] = [];
  page.on("request", (r) => {
    if (/(?:preview|workspace)[^/]*\.(?:mp4|webm)(?:[?#]|$)/.test(r.url())) badReq.push(r.url());
  });
  await page.goto("/");
  const imgs = page.locator("figure.clip img.clip-img");
  await expect.poll(() => imgs.nth(0).evaluate((im: HTMLImageElement) => im.getAttribute("src") || "")).toContain("preview-poster");
  await expect(imgs).toHaveCount(2);
  for (let i = 0; i < 2; i++) await expect(imgs.nth(i)).toHaveAttribute("alt", /.+/);
  if (HAS_MEDIA) {
    for (let i = 0; i < 2; i++) {
      await expect.poll(() => imgs.nth(i).evaluate((im: HTMLImageElement) => im.naturalWidth))
        .toBeGreaterThan(0); // poster image actually loaded
    }
  }
  expect(badReq, badReq.join("\n")).toHaveLength(0); // no poster-only slot pulled video
});

test("pause control: toggles playback, flips label, blocks lazy load while paused", async ({ page }) => {
  test.skip(!HAS_MEDIA, "ffmpeg unavailable");
  const lazyReqs: string[] = [];
  page.on("request", (r) => { if (/faithful[^/]*\.(?:mp4|webm)(?:[?#]|$)/.test(r.url())) lazyReqs.push(r.url()); });
  await page.goto("/");
  const btn = page.locator("#motion-toggle");
  await expect(btn).toBeVisible();
  await btn.focus();
  await expect(btn).toBeFocused();
  const hero = page.locator("figure.clip-hero video");
  await expect.poll(() => hero.evaluate((v: HTMLVideoElement) => !v.paused), { timeout: 8000 }).toBe(true);
  await btn.click();
  await expect(btn).toHaveText("Play motion");
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
  await expect(page.locator("#motion-toggle")).toHaveText("Play motion");
});

test("asset rule: no media binary tracked under site/", () => {
  const tracked = execFileSync("git", ["ls-files", "site/"], { cwd: SITE + "/.." })
    .toString().trim().split("\n").filter(Boolean);
  const leaked = tracked.filter((f) => /\.(mp4|webm|jpe?g|png|webp|gif)$/i.test(f));
  expect(leaked, `tracked media leaked: ${leaked.join(", ")}`).toHaveLength(0);
});
