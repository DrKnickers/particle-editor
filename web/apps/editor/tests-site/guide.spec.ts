import { test, expect } from "@playwright/test";

const GUIDE_SLUGS = [
  "home",
  "setup",
  "particle-authoring-primer",
  "01-make-a-hardpoint-damage-effect-obvious",
  "02-polish-hardpoint-damage-smoke",
  "03-build-a-laser-shot-and-muzzle-flash",
  "04-recolor-and-orient-a-shield-impact",
  "app-ui-quick-reference",
  "file-structure",
  "where-particles-are-used-in-game",
  "game-concepts-glossary",
  "basic-controls",
  "stacking-emitters-and-children",
  "blend-modes",
  "weather-particles",
  "generation-types",
];

// After Phase 2 each `<!-- Media: id -->` anchor is expanded (by build-guide.mjs, from the
// wiki-media manifest) into a <video>/<img> embed — except the one manual in-game-proof shot,
// which stays an inert comment. The EXACT ordered filenames below (not just counts) are the
// contract: they sum to the 22 manifest items (19 clips + 2 stills + 1 manual) and lock each
// page's ids AND their order, so a duplicated/swapped/renamed anchor can't pass on count alone.
const RELEASE_BASE = "https://github.com/DrKnickers/particle-editor/releases/download/site-media/";
const TUTORIAL_MEDIA = new Map([
  ["01-make-a-hardpoint-damage-effect-obvious", {
    clips: ["tutorial-01-open-override.mp4", "tutorial-01-green-color-edit.mp4", "tutorial-01-save-override.mp4"],
    stills: ["tutorial-01-opening-result.png"],
    manualComment: 1,
  }],
  ["02-polish-hardpoint-damage-smoke", {
    clips: ["tutorial-02-color-warm-tint.mp4", "tutorial-02-alpha-fade.mp4", "tutorial-02-scale-growth.mp4", "tutorial-02-final-preview.mp4"],
    stills: ["tutorial-02-opening-result.png"],
    manualComment: 0,
  }],
  ["03-build-a-laser-shot-and-muzzle-flash", {
    clips: ["tutorial-03-opening-result.mp4", "tutorial-03-projectile-core.mp4", "tutorial-03-inherit-parent-speed.mp4",
      "tutorial-03-spawner-direction.mp4", "tutorial-03-muzzle-flash.mp4", "tutorial-03-no-parent-speed.mp4", "tutorial-03-final-preview.mp4"],
    stills: [],
    manualComment: 0,
  }],
  ["04-recolor-and-orient-a-shield-impact", {
    clips: ["tutorial-04-opening-result.mp4", "tutorial-04-open-override.mp4", "tutorial-04-recolor-purple.mp4",
      "tutorial-04-orient-preview.mp4", "tutorial-04-final-preview.mp4"],
    stills: [],
    manualComment: 0,
  }],
]);

function guidePath(slug: string): string {
  return "/guide/" + slug + ".html";
}

function gridTrackCount(value: string): number {
  return value.trim().split(/\s+/).filter(Boolean).length;
}

test("guide structure: pages, sidebar, active nav, kicker, toc", async ({ page }) => {
  for (const slug of GUIDE_SLUGS) {
    const path = guidePath(slug);
    const response = await page.goto(path);
    expect(response?.status(), path + " should load").toBe(200);

    await expect(page.locator("h1")).toHaveCount(1);
    await expect(page.locator(".guide-sidebar")).toHaveCount(1);
    await expect(page.locator(".guide-sidebar .side-group")).toHaveCount(4);
    await expect(page.locator(".guide-sidebar a")).toHaveCount(16);

    const current = page.locator('.guide-sidebar a[aria-current="page"]');
    await expect(current).toHaveCount(1);
    const currentHref = await current.getAttribute("href");
    expect(new URL(currentHref ?? "", page.url()).pathname, slug + " active sidebar link").toBe(path);

    await expect(page.locator(".guide-kicker")).toHaveCount(1);
    if (await page.locator("h2").count())
      await expect(page.locator(".toc")).toHaveCount(1);
  }
});

test("guide links: same-origin links resolve and fragment anchors exist", async ({ page }) => {
  const checked = new Map<string, number>();

  for (const slug of GUIDE_SLUGS) {
    await page.goto(guidePath(slug));
    const pageOrigin = new URL(page.url()).origin;
    const hrefs = await page.locator("a[href]").evaluateAll((els) =>
      els.map((el) => el.getAttribute("href") || ""));

    for (const href of hrefs) {
      if (href.startsWith("#")) {
        const id = decodeURIComponent(href.slice(1));
        expect(id, slug + " has an empty fragment link").not.toBe("");
        const hasTarget = await page.evaluate((targetId) =>
          Boolean(document.getElementById(targetId)), id);
        expect(hasTarget, slug + " missing anchor target " + href).toBe(true);
        continue;
      }

      const resolved = new URL(href, page.url());
      if (/^https?:\/\//i.test(href) && resolved.origin !== pageOrigin) continue;
      if (resolved.origin !== pageOrigin) continue;

      resolved.hash = "";
      const url = resolved.href;
      if (!checked.has(url)) {
        const response = await page.request.get(url);
        checked.set(url, response.status());
      }
      expect(checked.get(url), slug + " internal link should resolve: " + href).toBe(200);
    }
  }
});

test("guide pager: first, middle, and final pages expose expected directions", async ({ page }) => {
  await page.goto("/guide/home.html");
  await expect(page.locator(".guide-pager .pager-prev")).toHaveCount(0);
  await expect(page.locator(".guide-pager .pager-next")).toHaveCount(1);

  await page.goto("/guide/generation-types.html");
  await expect(page.locator(".guide-pager .pager-prev")).toHaveCount(1);
  await expect(page.locator(".guide-pager .pager-next")).toHaveCount(0);

  await page.goto("/guide/03-build-a-laser-shot-and-muzzle-flash.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./02-polish-hardpoint-damage-smoke.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./04-recolor-and-orient-a-shield-impact.html");

  // game-concepts-glossary used to be the final page (no next); the "Coming Soon" section
  // now follows it, so it must expose both directions — this is the actual behavior change
  // the new nav section introduces, not just a shifted final-page slug.
  await page.goto("/guide/game-concepts-glossary.html");
  await expect(page.locator(".guide-pager .pager-prev")).toHaveCount(1);
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./basic-controls.html");
});

test("guide home tables: structured tables with body links", async ({ page }) => {
  await page.goto("/guide/home.html");
  const tables = page.locator("table");
  await expect(tables).toHaveCount(2);

  for (let i = 0; i < await tables.count(); i++) {
    const table = tables.nth(i);
    await expect(table.locator("thead")).toHaveCount(1);
    await expect(table.locator("tbody")).toHaveCount(1);
    expect(await table.locator("tbody td a").count(), "table " + (i + 1) + " should contain body links")
      .toBeGreaterThan(0);
  }
});

test("guide tutorial media: anchors expand to the exact manifest clips/stills, in order", async ({ page }) => {
  for (const [slug, want] of TUTORIAL_MEDIA) {
    await page.goto(guidePath(slug));

    // Exact ordered data-clip filenames — locks id, order, AND count in one assertion, so a
    // duplicated or swapped anchor (which would still pass a bare count) is caught.
    const clips = await page.locator("video.clip-video")
      .evaluateAll((els) => els.map((el) => el.getAttribute("data-clip")));
    expect(clips, slug + " ordered clip filenames").toEqual(want.clips);

    // Exact ordered still data-poster filenames (same reasoning; empty on pages 03/04).
    const stills = await page.locator("img.clip-img")
      .evaluateAll((els) => els.map((el) => el.getAttribute("data-poster")));
    expect(stills, slug + " ordered still filenames").toEqual(want.stills);

    // Every guide clip exposes a pause/replay control (WCAG 2.2.2) + loops muted inline.
    const count = await page.locator("video.clip-video").count();
    for (let i = 0; i < count; i++) {
      const v = page.locator("video.clip-video").nth(i);
      await expect(v, slug + " clip " + i + " controls").toHaveJSProperty("controls", true);
      await expect(v).toHaveJSProperty("loop", true);
      await expect(v).toHaveJSProperty("muted", true);
    }

    // No raw anchor survives un-expanded; the manual shot alone stays a comment.
    const html = await (await page.request.get(guidePath(slug))).text();
    expect(html.includes("<!-- Media:"), slug + " has an un-expanded anchor").toBe(false);
    expect(html.match(/<!-- Media \(manual/g)?.length ?? 0, slug + " manual comment count")
      .toBe(want.manualComment);
  }
});

test("guide media resolver: clips and stills join MEDIA_BASE at runtime", async ({ page }) => {
  await page.addInitScript(() => { (window as any).__MEDIA_BASE__ = "/media-local/"; });
  await page.goto(guidePath("01-make-a-hardpoint-damage-effect-obvious"));

  // Posters/still src are set synchronously from data-poster for the whole page.
  const firstVideo = page.locator("video.clip-video").first();
  await expect.poll(() => firstVideo.evaluate((el: HTMLVideoElement) => el.poster))
    .toContain("/media-local/");
  const firstStill = page.locator("img.clip-img").first();
  await expect.poll(() => firstStill.evaluate((el: HTMLImageElement) => el.getAttribute("src") || ""))
    .toContain("/media-local/tutorial-01-opening-result.png");

  // The in-view clip lazily loads its src from MEDIA_BASE (guide-media.js IntersectionObserver).
  await firstVideo.scrollIntoViewIfNeeded();
  await expect.poll(() => firstVideo.evaluate((el: HTMLVideoElement) => el.getAttribute("src") || ""))
    .toContain("/media-local/tutorial-01-open-override.mp4");
});

test("guide media resolver: falls back to the site-media release URL by default", async ({ page }) => {
  // No __MEDIA_BASE__ override and no ?media= param → guide-media.js must use the SHIPPED
  // default release URL. Guards against a typo in that literal (the override-injecting test
  // above can't catch it because it bypasses the default branch entirely).
  await page.goto(guidePath("01-make-a-hardpoint-damage-effect-obvious"));
  await expect.poll(() => page.locator("video.clip-video").first().evaluate((el: HTMLVideoElement) => el.poster))
    .toContain(RELEASE_BASE);
  await expect.poll(() => page.locator("img.clip-img").first().evaluate((el: HTMLImageElement) => el.getAttribute("src") || ""))
    .toContain(RELEASE_BASE);
});

test("guide media figures never overflow the article column", async ({ page }) => {
  // The media-heaviest page (7 clips) at the narrowest supported width is the worst case
  // for a figure ballooning past the column (the landing .clip's 100vw width would do this).
  for (const width of [390, 768, 1280]) {
    await page.setViewportSize({ width, height: 844 });
    await page.goto(guidePath("03-build-a-laser-shot-and-muzzle-flash"));
    const { clientWidth, scrollWidth } = await page.evaluate(() => ({
      clientWidth: document.documentElement.clientWidth,
      scrollWidth: document.documentElement.scrollWidth,
    }));
    expect(scrollWidth, "horizontal overflow at " + width + "px").toBeLessThanOrEqual(clientWidth + 1);
  }
});

test("guide setup responsive layout: desktop rails collapse on narrow screens", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/guide/setup.html");
  const desktop = await page.evaluate(() => ({
    clientWidth: document.documentElement.clientWidth,
    scrollWidth: document.documentElement.scrollWidth,
    columns: getComputedStyle(document.querySelector(".guide-layout")!).gridTemplateColumns,
  }));
  expect(desktop.scrollWidth).toBeLessThanOrEqual(desktop.clientWidth + 1);
  expect(gridTrackCount(desktop.columns), desktop.columns).toBe(3);

  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/guide/setup.html");
  const mobile = await page.evaluate(() => ({
    clientWidth: document.documentElement.clientWidth,
    scrollWidth: document.documentElement.scrollWidth,
    columns: getComputedStyle(document.querySelector(".guide-layout")!).gridTemplateColumns,
    tocDisplay: getComputedStyle(document.querySelector(".toc")!).display,
  }));
  expect(mobile.scrollWidth).toBeLessThanOrEqual(mobile.clientWidth + 1);
  expect(gridTrackCount(mobile.columns) === 1 || mobile.tocDisplay === "none", mobile.columns)
    .toBe(true);
});
