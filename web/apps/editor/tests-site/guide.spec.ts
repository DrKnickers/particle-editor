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
];

const TUTORIAL_MEDIA_COUNTS = new Map([
  ["01-make-a-hardpoint-damage-effect-obvious", 5],
  ["02-polish-hardpoint-damage-smoke", 5],
  ["03-build-a-laser-shot-and-muzzle-flash", 7],
  ["04-recolor-and-orient-a-shield-impact", 5],
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
    await expect(page.locator(".guide-sidebar .side-group")).toHaveCount(3);
    await expect(page.locator(".guide-sidebar a")).toHaveCount(11);

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

  await page.goto("/guide/game-concepts-glossary.html");
  await expect(page.locator(".guide-pager .pager-prev")).toHaveCount(1);
  await expect(page.locator(".guide-pager .pager-next")).toHaveCount(0);

  await page.goto("/guide/03-build-a-laser-shot-and-muzzle-flash.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./02-polish-hardpoint-damage-smoke.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./04-recolor-and-orient-a-shield-impact.html");
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

test("guide tutorial media anchors: generated media comments are retained", async ({ page }) => {
  for (const [slug, count] of TUTORIAL_MEDIA_COUNTS) {
    const response = await page.request.get(guidePath(slug));
    expect(response.status(), slug + " should load").toBe(200);
    const html = await response.text();
    expect(html.includes("<!-- Media:"), slug + " should contain media comments").toBe(true);
    expect(html.match(/<!-- Media:/g)?.length ?? 0, slug + " media comment count").toBe(count);
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
