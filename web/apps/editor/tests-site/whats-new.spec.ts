import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";

// What's New page (site/whats-new.html) — the returning-modder surface. Structure per
// docs/superpowers/specs/2026-07-20-whats-new-tab-design.md: pitch half (five capability
// wins, each reusing a landing clip) + "Coming back from the GlyphX editor?" departures
// table. No new media: stems below are the landing set minus hero.
const STEMS = ["faith", "f04", "spawner", "f02-reorder", "f02"];

// The departures table's full row content (all 3 cells: "In the old editor" / "Here" / "Why"),
// in order — every row is proven against fork point 1222c13 (Mike.NL's GlyphX Particle Editor
// v1.5) on both sides (see .superpowers/sdd/fix-findings.md F1). This page previously shipped
// three false claims about the legacy editor, and in every case the falsehood was in the
// "Here"/"Why" cells — so pinning only the first cell would not have caught them. Asserting all
// three cells (and thus the 3-cells-per-row shape) means a silent content swap back to the old,
// unverified rows — in ANY column — fails this test.
const DEPARTURE_ROWS = [
  [
    "Seven curve tabs — you saw one channel at a time",
    "One canvas: tick the channels you want and they draw together, with the focused one editable",
    "Compare alpha against scale without flipping tabs. The Y axis is shared, so turning on a large-range channel rescales the view.",
  ],
  [
    "Typing in a numeric field changed the effect on every keystroke",
    "The value applies when you press Enter or leave the field",
    "Each change is a round-trip to the render host, and committing per keystroke floods it. Arrows, wheel, and drag still apply instantly.",
  ],
  [
    "Delete removed the emitter and everything under it, immediately",
    "A leaf still goes straight away; deleting a parent or a multi-selection asks first",
    "Deleting a parent takes its whole subtree with it, which the row you selected does not show.",
  ],
  [
    "Copy put the emitter on the Windows clipboard, so it survived into a second editor window",
    "Copy and paste work within the running editor only",
    "The clipboard now lives in the app rather than the OS. Cross-track and cross-emitter paste in one session are unaffected.",
  ],
  [
    "A colour swatch set the background, full stop",
    "The colour is the fallback; a selected game skydome takes over, and choosing a colour clears the dome",
    "Backgrounds can now be a real in-game dome, so the two settings are mutually exclusive.",
  ],
];

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => { (window as any).__MEDIA_BASE__ = "/media-local/"; });
});

test("whats-new structure: h1, five features, departures table; no uncaught JS errors", async ({ page }) => {
  const jsErrors: string[] = [];
  page.on("pageerror", (e) => jsErrors.push(String(e)));
  await page.goto("/whats-new.html");
  await expect(page.locator("h1")).toHaveCount(1);
  await expect(page.locator("h1")).toContainText("GlyphX");
  await expect(page.locator("section.feature")).toHaveCount(5);
  // Departures table: real table semantics, EXACTLY 5 rows (not "at least"), each pinned to all
  // 3 cells (not just the first) so a rewritten/reordered/added row — or a row missing a cell —
  // fails instead of silently passing.
  const table = page.locator("table.compare-table");
  await expect(table).toHaveCount(1);
  await expect(table.locator("thead")).toHaveCount(1);
  const rows = table.locator("tbody tr");
  await expect(rows).toHaveCount(DEPARTURE_ROWS.length);
  const cells = await rows.evaluateAll((trs) =>
    trs.map((tr) => [...tr.querySelectorAll("td")].map((td) => td.textContent!.trim())));
  expect(cells).toEqual(DEPARTURE_ROWS);
  // Lineage claim pinned exactly — "GlyphX" alone would also match a sentence that doesn't
  // actually name the source editor and version.
  await expect(page.locator("main")).toContainText("Mike.NL's GlyphX Particle Editor v1.5");
  expect(jsErrors, jsErrors.join("\n")).toHaveLength(0);
});

test("whats-new clips: exact stems in order, standard contract, posters resolve", async ({ page }) => {
  await page.goto("/whats-new.html");
  const vids = page.locator("video.clip-video");
  const clips = await vids.evaluateAll((els) => els.map((el) => el.getAttribute("data-clip")));
  expect(clips).toEqual(STEMS.map((s) => `${s}.mp4`));
  const n = await vids.count();
  for (let i = 0; i < n; i++) {
    const v = vids.nth(i);
    await expect(v).toHaveJSProperty("loop", true);
    await expect(v).toHaveJSProperty("muted", true);
    await expect(v).toHaveJSProperty("playsInline", true);
    await expect(v).toHaveAttribute("width", "1280");
    await expect(v).toHaveAttribute("height", "960");
    await expect.poll(() => v.evaluate((el: HTMLVideoElement) => el.poster)).not.toBe("");
  }
});

test("topbar: both top-level pages carry Download / Guide / What's New / Source", async ({ page }) => {
  for (const path of ["/", "/whats-new.html"]) {
    await page.goto(path);
    const nav = page.locator("header.topbar nav.topnav");
    await expect(nav.locator('a[href*="releases/latest"]'), path + " download").toHaveCount(1);
    await expect(nav.locator('a[href="guide/home.html"]'), path + " guide").toHaveCount(1);
    await expect(nav.locator('a[href="whats-new.html"]'), path + " whats-new").toHaveCount(1);
    await expect(nav.locator('a[href="https://github.com/DrKnickers/particle-editor"]'), path + " source").toHaveCount(1);
    await expect(nav.locator("#motion-toggle"), path + " motion toggle").toHaveCount(1);
  }
});

test("topbar: guide pages carry the same 4-item nav, hrefs relative to /guide/", async ({ page }) => {
  // Guards F2 from regressing: build-guide.mjs's page template must keep emitting the
  // What's New link between Guide and Source, or styles.css's pixel-identical-topbar view
  // transition (which hard-cuts the topbar out of the cross-fade) snaps on landing -> guide.
  // Hrefs are relative to /guide/, NOT the top-level pages' hrefs above: Guide is "./home.html"
  // (the guide's own first page) and What's New is "../whats-new.html" (one directory up).
  await page.goto("/guide/home.html");
  const nav = page.locator("header.topbar nav.topnav");
  await expect(nav.locator('a[href*="releases/latest"]'), "guide download").toHaveCount(1);
  await expect(nav.locator('a[href="./home.html"]'), "guide guide-link").toHaveCount(1);
  await expect(nav.locator('a[href="../whats-new.html"]'), "guide whats-new").toHaveCount(1);
  await expect(nav.locator('a[href="https://github.com/DrKnickers/particle-editor"]'), "guide source").toHaveCount(1);
});

test("a11y: no critical/serious axe violations on the What's New page (only page with a <table> outside the guide)", async ({ page }) => {
  await page.goto("/whats-new.html");
  // wait for the CSS entrance stagger to finish so axe sees the settled page
  await page.waitForFunction(() =>
    [...document.querySelectorAll(".wn-hero > *, .feature > *, .wn-departures > *")]
      .every((el) => getComputedStyle(el).opacity === "1"),
    null, { timeout: 5000 });
  const results = await new AxeBuilder({ page }).withTags(["wcag2a", "wcag2aa"]).analyze();
  const bad = results.violations.filter((v) => ["critical", "serious"].includes(v.impact ?? ""));
  expect(bad, JSON.stringify(bad.map((v) => v.id))).toHaveLength(0);
});

test("guide home points returning modders at whats-new", async ({ page }) => {
  await page.goto("/guide/home.html");
  // Scoped to the article body: the topbar ALSO links "../whats-new.html" since F2 added the
  // nav item there, so an unscoped locator now matches twice (topbar + this prose pointer).
  const pointer = page.locator('.guide-article a[href="../whats-new.html"]');
  await expect(pointer).toHaveCount(1);
  const resp = await page.request.get("/whats-new.html");
  expect(resp.status()).toBe(200);
});

// The topbar is `flex-wrap:nowrap` with every child `white-space:nowrap`, and the <=520px block
// pins .topnav to `flex:0 0 auto` — so the bar has a fixed intrinsic width that cannot shrink.
// Before `flex-wrap:wrap` at <=400px, a fourth nav item overflowed the body at 390px (a very
// common device width) and any FUTURE nav item would silently do it again at some width. The
// departures table has the same shape: three prose columns cannot fit 320px, so it scrolls
// inside .table-scroll rather than pushing the body. Assert the invariant, not the thresholds.
const NARROW_WIDTHS = [320, 360, 390];

test("responsive: the body never scrolls horizontally; the table absorbs its own overflow", async ({ page }) => {
  for (const path of ["/", "/whats-new.html", "/guide/home.html"]) {
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
  // At the narrowest width the scroll container — not the page — takes the overflow.
  await page.setViewportSize({ width: 320, height: 800 });
  await page.goto("/whats-new.html");
  const scroller = page.locator(".table-scroll");
  await expect(scroller).toHaveCount(1);
  const tableScrolls = await scroller.evaluate((el) => el.scrollWidth > el.clientWidth);
  expect(tableScrolls, "the table, not the body, must absorb the overflow at 320px").toBe(true);
});
