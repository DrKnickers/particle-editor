import { test, expect } from "@playwright/test";

// Cross-page topbar invariants. styles.css hard-cuts the topbar out of the cross-document
// view-transition fade, which only looks right if every page renders pixel-identical nav
// geometry — same links, same order, and exactly one .motion-toggle slot per page. These
// tests survived the What's New page (whats-new.html, retired after #655): the invariants
// they pin belong to the site, not to any one page.

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => { (window as any).__MEDIA_BASE__ = "/media-local/"; });
});

test("topbar: landing and guide render the identical ordered nav; What's New is gone", async ({ page }) => {
  for (const { path, guideHref } of [
    { path: "/", guideHref: "guide/home.html" },
    { path: "/guide/home.html", guideHref: "./home.html" },
  ]) {
    await page.goto(path);
    const nav = page.locator("header.topbar nav.topnav");
    // The pixel-identical contract is about the nav's EXACT ordered children — presence
    // counts alone would pass a reordered or extended bar that snaps mid-transition. Pin
    // the full sequence: same elements, same order, nothing extra.
    const children = await nav.evaluate((el) =>
      [...el.children].map((c) =>
        c.tagName === "A"
          ? "a:" + c.getAttribute("href")
          : c.tagName.toLowerCase() + ":" + (c as HTMLElement).className.trim()));
    expect(children, path + " ordered nav children").toEqual([
      "a:https://github.com/DrKnickers/particle-editor/releases/latest",
      "a:" + guideHref,
      "a:https://github.com/DrKnickers/particle-editor",
      "button:motion-toggle",
    ]);
    // The retired page must not resurface under ANY href — match by visible text too.
    await expect(nav.getByText(/what.?s new/i), path + " no whats-new text").toHaveCount(0);
  }
  // Only the landing's slot is the LIVE control (it has clips to pause); the guide's is an
  // invisible placeholder (asserted inert below).
  await page.goto("/");
  await expect(page.locator("nav.topnav #motion-toggle"), "landing keeps the live toggle").toHaveCount(1);
});

test("motion-toggle slot: the guide's placeholder is completely inert", async ({ page }) => {
  // The slot exists for topbar view-transition geometry, but the guide has no clips to pause,
  // so its slot must be invisible, unfocusable, unannounced, and without the live control's
  // id — or a dead Pause button ships.
  await page.goto("/guide/home.html");
  const slot = page.locator("nav.topnav .motion-toggle");
  await expect(slot, "has the slot").toHaveCount(1);
  await expect(slot, "hidden").toHaveCSS("visibility", "hidden");
  await expect(slot, "unfocusable").toHaveAttribute("tabindex", "-1");
  await expect(slot, "unannounced").toHaveAttribute("aria-hidden", "true");
  expect(await slot.getAttribute("id"), "must not carry the live control's id").toBeNull();
});

// The topbar is `flex-wrap:nowrap` with every child `white-space:nowrap`, and the <=520px
// block pins .topnav to `flex:0 0 auto` — the bar has a fixed intrinsic width that cannot
// shrink, so any future nav item can silently overflow the body at some width. The <=400px
// `flex-wrap:wrap` rule is the escape valve. Assert the invariant, not the thresholds.
const NARROW_WIDTHS = [320, 360, 390];

test("responsive: neither the body nor the topbar scrolls horizontally on narrow screens", async ({ page }) => {
  for (const path of ["/", "/guide/home.html", "/guide/app-ui-quick-reference.html"]) {
    for (const width of NARROW_WIDTHS) {
      await page.setViewportSize({ width, height: 800 });
      await page.goto(path);
      const overflows = await page.evaluate(() =>
        document.documentElement.scrollWidth > document.documentElement.clientWidth);
      expect(overflows, `${path} must not scroll horizontally at ${width}px`).toBe(false);
      const bar = page.locator("header.topbar");
      const barOverflows = await bar.evaluate((el) => el.scrollWidth > el.clientWidth);
      expect(barOverflows, `${path} topbar must not overflow at ${width}px`).toBe(false);
    }
  }
});
