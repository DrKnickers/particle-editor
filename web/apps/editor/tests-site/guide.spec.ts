import { test, expect } from "@playwright/test";
import AxeBuilder from "@axe-core/playwright";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

// Derive the guide's pages, order, and section count from nav.json — the SAME single
// source of truth build-guide.mjs renders the sidebar from — so this spec can't silently
// drift when a page is added or removed (as it did between #593 and #599: 15 → 19 pages).
const NAV = JSON.parse(
  readFileSync(
    resolve(dirname(fileURLToPath(import.meta.url)), "../../../../site/guide-src/nav.json"),
    "utf8",
  ),
) as { sections: { pages: { slug: string }[] }[] };
const GUIDE_SLUGS = NAV.sections.flatMap((s) => s.pages.map((p) => p.slug));
const SECTION_COUNT = NAV.sections.length;

// After Phase 2 each `<!-- Media: id -->` anchor is expanded (by build-guide.mjs, from the
// wiki-media manifest) into a <video>/<img> embed — except the one manual in-game-proof shot,
// which stays an inert comment. The EXACT ordered filenames below (not just counts) are the
// contract: they lock each page's ids AND their order, so a duplicated/swapped/renamed anchor
// can't pass on count alone. NOTE: this map covers what each page RENDERS, which is now a
// deliberate SUBSET of the manifest — tutorials 2 and 4 have had their clips withdrawn (see
// their entries below), so the manifest still describes clips that no page embeds. That is
// intended; nothing fails on an unreferenced manifest item.
const RELEASE_BASE = "https://github.com/DrKnickers/particle-editor/releases/download/site-media/";
const GUIDE_MEDIA = new Map([
  ["coming-from-the-old-glyphx-editor", {
    clips: [
      "ref-curve-visibility.mp4",
      "ref-render-order.mp4",
      "tutorial-05-sparks-children.mp4",
      "f04.mp4",
      "faith.mp4",
      "tutorial-03-spawner-direction.mp4",
    ],
    stills: [
      "ref-returning-import-emitters.png",
      "ref-returning-texture-palette.png",
      "ref-returning-atlas-frame-picker.png",
      "ref-returning-skydome-picker.png",
      "ref-returning-reference-gizmo.png",
    ],
    manualComment: 0,
  }],
  ["01-make-a-hardpoint-damage-effect-obvious", {
    clips: ["tutorial-01-open-override.mp4", "tutorial-01-green-color-edit.mp4", "tutorial-01-save-override.mp4"],
    stills: ["tutorial-01-opening-result.png"],
    manualComment: 1,
  }],
  // Tutorials 2 and 4 ship NO clips pre-release: the recordings were withdrawn pending
  // re-record, leaving each page its opening still only. Their nav.json entries carry
  // `status: "videos-pending"`, which the sidebar renders as a badge. Empty `clips` here is
  // therefore the intended state, not a drop to be "fixed" — restoring a clip means
  // re-adding its `<!-- Media: id -->` anchor AND its filename to the array below.
  ["02-polish-hardpoint-damage-smoke", {
    clips: [],
    stills: ["tutorial-02-opening-result.png"],
    manualComment: 0,
  }],
  ["03-build-a-laser-shot-and-muzzle-flash", {
    clips: ["tutorial-03-opening-result.mp4", "tutorial-03-projectile-core.mp4", "tutorial-03-inherit-parent-speed.mp4",
      "tutorial-03-spawner-direction.mp4", "tutorial-03-muzzle-flash.mp4", "tutorial-03-no-parent-speed.mp4", "tutorial-03-final-preview.mp4"],
    stills: ["tutorial-03-glow-layers.png", "tutorial-03-muzzle-glow-props.png"],
    manualComment: 0,
  }],
  // Its opening media is now a STILL: the manifest item was converted clip → image and points
  // at the poster frame the clip already published, so the page keeps a visual definition of
  // "a clear purple hit effect" for the subjective checks at §0 and §6.
  ["04-recolor-and-orient-a-shield-impact", {
    clips: [],
    stills: ["tutorial-04-opening-result-poster.jpg"],
    manualComment: 0,
  }],
  ["05-build-an-explosion", {
    // §4 shows BOTH fireball constructions: -index is Option A (flipbook), -layered is Option B
    // (layered additive). §9 ends by showing the finished explosion BOTH ways: -final-preview
    // (flipbook) then -final-preview-layered (layered).
    clips: ["tutorial-05-opening-result.mp4", "tutorial-05-flash-burst.mp4", "tutorial-05-smoke-render-order.mp4",
      "tutorial-05-fireball-build.mp4", "tutorial-05-fireball-index.mp4", "tutorial-05-fireball-layered.mp4",
      "tutorial-05-sparks-children.mp4", "tutorial-05-final-preview.mp4", "tutorial-05-final-preview-layered.mp4"],
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
    await expect(page.locator(".guide-sidebar .side-group")).toHaveCount(SECTION_COUNT);
    await expect(page.locator(".guide-sidebar a")).toHaveCount(GUIDE_SLUGS.length);

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
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./coming-from-the-old-glyphx-editor.html");

  // Returning GlyphX users get their own Start Here route immediately after Guide Home,
  // before the course setup and beginner material.
  await page.goto("/guide/coming-from-the-old-glyphx-editor.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./home.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./setup.html");

  // game-concepts-glossary is the last page in reading order (final entry of Reference),
  // so it exposes a prev but no next.
  await page.goto("/guide/game-concepts-glossary.html");
  await expect(page.locator(".guide-pager .pager-prev")).toHaveCount(1);
  await expect(page.locator(".guide-pager .pager-next")).toHaveCount(0);

  // The "Concepts Before You Build" bridge sits between Tutorial 2 and Tutorial 3, so Tutorial 3's
  // prev is the bridge (not Tutorial 2). Assert both to guard the bridge's place in reading order.
  await page.goto("/guide/concepts-before-you-build.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./02-polish-hardpoint-damage-smoke.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./03-build-a-laser-shot-and-muzzle-flash.html");

  await page.goto("/guide/03-build-a-laser-shot-and-muzzle-flash.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./concepts-before-you-build.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./04-recolor-and-orient-a-shield-impact.html");

  // basic-controls now lives mid-"Start Here" (setup → basic-controls → primer). Assert its
  // pager wiring so a regression that drops the newly-promoted page from reading order is caught.
  await page.goto("/guide/basic-controls.html");
  await expect(page.locator(".guide-pager .pager-prev"))
    .toHaveAttribute("href", "./setup.html");
  await expect(page.locator(".guide-pager .pager-next"))
    .toHaveAttribute("href", "./particle-authoring-primer.html");
});

// Pages flagged `status: "videos-pending"` in nav.json render a muted badge but MUST stay
// ordinary links: four other pages (setup, home, concepts-before-you-build, tutorial 1) treat
// them as existing — in setup's case prerequisite — curriculum, so a dead or hidden entry would
// make the guide contradict itself. Derived from nav.json so adding/clearing a flag updates the
// expectation with the manifest, not by hand.
const PENDING_SLUGS = (NAV as { sections: { pages: { slug: string; status?: string }[] }[] })
  .sections.flatMap((s) => s.pages)
  .filter((p) => p.status === "videos-pending")
  .map((p) => p.slug);

test("guide videos-pending: flagged pages badge but stay clickable links", async ({ page }) => {
  expect(PENDING_SLUGS.length, "nav.json should flag at least one videos-pending page").toBeGreaterThan(0);

  await page.goto(guidePath("home"));
  for (const slug of PENDING_SLUGS) {
    const link = page.locator(`.guide-sidebar a[href="./${slug}.html"]`);
    await expect(link, slug + " keeps a real sidebar link").toHaveCount(1);
    await expect(link, slug + " is flagged pending").toHaveClass(/is-pending/);
    // Bracketed so the badge reads as an aside, not as part of the tutorial's title it sits
    // flush against — the unbracketed form was easy to miss.
    await expect(link.locator(".side-pending"), slug + " badge text").toHaveText("(Videos pending)");
  }
  // Unflagged pages must NOT sprout a badge.
  const badges = await page.locator(".guide-sidebar .side-pending").count();
  expect(badges, "exactly one badge per flagged page").toBe(PENDING_SLUGS.length);

  // On a flagged page itself both states apply at once — a single class attribute must carry
  // BOTH, or the active highlight silently disappears on exactly these pages.
  await page.goto(guidePath(PENDING_SLUGS[0]));
  const self = page.locator('.guide-sidebar a[aria-current="page"]');
  await expect(self, "flagged page keeps its own active highlight").toHaveClass(/active/);
  await expect(self, "flagged page keeps its pending class").toHaveClass(/is-pending/);
  // The badge is intentionally inside the accessible NAME — screen-reader users get the same
  // warning sighted users do. toHaveAccessibleName (not toContainText) so an aria-hidden badge
  // — visually present but silently dropped from the name — fails here.
  await expect(self).toHaveAccessibleName(/\(Videos pending\)/);
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

test("guide media: anchors expand to the exact manifest clips/stills, in order", async ({ page }) => {
  for (const [slug, want] of GUIDE_MEDIA) {
    await page.goto(guidePath(slug));

    // Exact ordered data-clip filenames — locks id, order, AND count in one assertion, so a
    // duplicated or swapped anchor (which would still pass a bare count) is caught.
    const clips = await page.locator("video.clip-video")
      .evaluateAll((els) => els.map((el) => el.getAttribute("data-clip")));
    expect(clips, slug + " ordered clip filenames").toEqual(want.clips);

    // Exact ordered still data-poster filenames (same reasoning; empty on page 03).
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

test("guide setup responsive layout: rails step down before they crush the article", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  const desktopResponse = await page.goto("/guide/setup.html");
  expect(desktopResponse?.ok()).toBe(true);
  const desktop = await page.evaluate(() => ({
    clientWidth: document.documentElement.clientWidth,
    scrollWidth: document.documentElement.scrollWidth,
    columns: getComputedStyle(document.querySelector(".guide-layout")!).gridTemplateColumns,
    layoutWidth: document.querySelector(".guide-layout")!.getBoundingClientRect().width,
    sidebarLeft: document.querySelector(".guide-sidebar")!.getBoundingClientRect().left,
    mainLeft: document.querySelector(".guide-main")!.getBoundingClientRect().left,
    mainPaddingLeft: getComputedStyle(document.querySelector(".guide-main")!).paddingLeft,
    mainPaddingRight: getComputedStyle(document.querySelector(".guide-main")!).paddingRight,
  }));
  expect(desktop.scrollWidth).toBeLessThanOrEqual(desktop.clientWidth + 1);
  expect(gridTrackCount(desktop.columns), desktop.columns).toBe(3);
  expect(desktop.layoutWidth, "guide uses the wider shared site frame").toBeGreaterThan(1100);
  expect(desktop.sidebarLeft, "desktop sidebar stays left of the article").toBeLessThan(desktop.mainLeft);
  expect(desktop.mainPaddingLeft, "guide main does not inherit the landing page gutter").toBe("0px");
  expect(desktop.mainPaddingRight, "guide main does not inherit the landing page gutter").toBe("0px");

  await page.setViewportSize({ width: 1024, height: 800 });
  const tablet = await page.evaluate(() => ({
    columns: getComputedStyle(document.querySelector(".guide-layout")!).gridTemplateColumns,
    tocDisplay: getComputedStyle(document.querySelector(".toc")!).display,
    sidebarLeft: document.querySelector(".guide-sidebar")!.getBoundingClientRect().left,
    mainLeft: document.querySelector(".guide-main")!.getBoundingClientRect().left,
    articleWidth: document.querySelector(".guide-article")!.getBoundingClientRect().width,
  }));
  expect(gridTrackCount(tablet.columns), tablet.columns).toBe(2);
  expect(tablet.tocDisplay).toBe("none");
  expect(tablet.sidebarLeft, "tablet keeps the guide index beside the article").toBeLessThan(tablet.mainLeft);
  expect(tablet.articleWidth, "tablet article keeps a useful reading width").toBeGreaterThan(600);

  await page.setViewportSize({ width: 390, height: 844 });
  const mobile = await page.evaluate(() => ({
    clientWidth: document.documentElement.clientWidth,
    scrollWidth: document.documentElement.scrollWidth,
    columns: getComputedStyle(document.querySelector(".guide-layout")!).gridTemplateColumns,
    tocDisplay: getComputedStyle(document.querySelector(".toc")!).display,
    mainTop: document.querySelector(".guide-main")!.getBoundingClientRect().top,
    sidebarTop: document.querySelector(".guide-sidebar")!.getBoundingClientRect().top,
    firstChildClass: document.querySelector(".guide-layout")!.firstElementChild?.className,
  }));
  expect(mobile.scrollWidth).toBeLessThanOrEqual(mobile.clientWidth + 1);
  expect(gridTrackCount(mobile.columns) === 1 || mobile.tocDisplay === "none", mobile.columns)
    .toBe(true);
  expect(mobile.firstChildClass, "article is first in DOM and reading order").toContain("guide-main");
  expect(mobile.mainTop, "article appears before the full guide index").toBeLessThan(mobile.sidebarTop);
});

// The "Quick Comparison" section on the returning-users guide page carries the verified remains
// of the retired What's New page. Every row is proven against fork point
// 1222c13 (Mike.NL's GlyphX Particle Editor v1.5) on both sides. The What's New page once
// shipped three FALSE claims about the legacy editor, and in every case the falsehood was in
// the middle/right cells — so all 3 cells are pinned, not just the first: a silent content
// swap back to unverified rows, in ANY column, fails here.
const DEPARTURE_ROWS = [
  [
    "Seven curve tabs — you saw one channel at a time",
    "One canvas: tick the channels you want and they draw together, with the focused one editable",
    "Compare alpha against scale without flipping tabs. The Y axis is shared, so turning on a large-range channel rescales the view.",
  ],
  [
    "Typing in a numeric field changed the effect on every keystroke",
    "The value applies when you press Enter or leave the field",
    "Finish typing before the preview updates. Arrows, wheel, and drag still apply instantly.",
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

test("guide returning GlyphX users: old-to-new workflows and verified departures", async ({ page }) => {
  const response = await page.goto("/guide/coming-from-the-old-glyphx-editor.html");
  expect(response?.status()).toBe(200);
  await expect(page.locator("h1")).toHaveText("Coming from the Old GlyphX Editor?");
  await expect(page.locator(".guide-article")).toContainText("Mike.NL's GlyphX Particle Editor v1.5");
  const opening = page.locator(".guide-article > p").filter({ hasText: "Each workflow starts" });
  await expect(opening.getByRole("link", { name: "Setup", exact: true })).toHaveAttribute(
    "href",
    "./setup.html",
  );

  const h2Ids = await page.locator(".guide-article h2").evaluateAll((headings) =>
    headings.map((heading) => heading.id));
  expect(h2Ids).toEqual([
    "key-workflow-changes",
    "quick-comparison",
    "authoring",
    "assets-and-mods",
    "scene-context",
    "testing-and-safety",
    "everyday-conveniences",
    "where-to-go-next",
  ]);

  const h3Ids = await page.locator(".guide-article h3").evaluateAll((headings) =>
    headings.map((heading) => heading.id));
  expect(h3Ids).toEqual([
    "curves-one-canvas-visibility-checkboxes",
    "organize-emitters-in-the-tree",
    "duplicate-and-link-repeated-variants",
    "import-emitters-from-another-particle",
    "load-assets-from-your-mod-stack",
    "reuse-frequently-used-textures",
    "pick-atlas-frames-visually",
    "choose-a-skydome-ground-and-lighting",
    "place-and-move-a-reference-object",
    "launch-effects-with-the-spawner",
    "pause-and-step-the-preview",
    "undo-recover-and-catch-overloads",
  ]);

  // The rejected orientation page and the marketing-shaped capability list stay retired.
  await expect(page.locator("#take-the-short-route")).toHaveCount(0);
  await expect(page.locator("#where-to-find-the-newer-tools")).toHaveCount(0);
  await expect(page.locator("#what-it-adds")).toHaveCount(0);
  await expect(page.locator("#what-behaves-differently")).toHaveCount(0);

  const keyWorkflowChanges = await page.locator("#key-workflow-changes").evaluate((heading) => {
    const parts: string[] = [];
    const hrefs: string[] = [];
    let next = heading.nextElementSibling;
    while (next && next.tagName !== "H2") {
      parts.push(next.textContent?.trim() ?? "");
      hrefs.push(...[...next.querySelectorAll("a")].map((link) => link.getAttribute("href") ?? ""));
      next = next.nextElementSibling;
    }
    return { text: parts.join(" "), hrefs };
  });
  expect(keyWorkflowChanges.text).toContain("Set the editor's asset order");
  expect(keyWorkflowChanges.text).toContain("Learn the shared curve canvas");
  expect(keyWorkflowChanges.text).toContain("Import instead of opening two editor windows");
  expect(keyWorkflowChanges.text).toContain("Know the safety controls");
  expect(keyWorkflowChanges.hrefs).toEqual([
    "#load-assets-from-your-mod-stack",
    "#curves-one-canvas-visibility-checkboxes",
    "#import-emitters-from-another-particle",
    "#pause-and-step-the-preview",
    "#undo-recover-and-catch-overloads",
  ]);

  const comparison = page.locator("#quick-comparison");
  const departureTable = comparison.locator("xpath=following-sibling::table[1]");
  await expect(departureTable).toHaveCount(1);
  const departureHeaders = ["In the old editor", "Here", "What changes in practice"];
  await expect(departureTable.locator("thead th")).toHaveText(departureHeaders);
  const rows = departureTable.locator("tbody tr");
  await expect(rows).toHaveCount(DEPARTURE_ROWS.length);
  const cells = await rows.evaluateAll((trs) =>
    trs.map((tr) => [...tr.querySelectorAll("td")].map((td) => td.textContent!.trim())));
  expect(cells).toEqual(DEPARTURE_ROWS);
  const labels = await rows.first().locator("td").evaluateAll((tds) =>
    tds.map((td) => td.getAttribute("data-label")));
  expect(labels).toEqual(departureHeaders);

  const workflows = [
    ["curves-one-canvas-visibility-checkboxes", ["Curve channels", "Snap to grid", "Scale"], ["ref-curve-visibility.mp4"]],
    ["organize-emitters-in-the-tree", ["multi-select", "reparent", "subtree"], ["ref-render-order.mp4"]],
    ["duplicate-and-link-repeated-variants", ["Increment Index", "Set Link Group", "shared"], ["tutorial-05-sparks-children.mp4"]],
    ["import-emitters-from-another-particle", ["Import Emitters", "Windows clipboard"], ["ref-returning-import-emitters.png"]],
    ["load-assets-from-your-mod-stack", ["Active load order", "top layer", "MODPATH"], ["f04.mp4"]],
    ["reuse-frequently-used-textures", ["Frequently-used textures", "Pinned", "Recent"], ["ref-returning-texture-palette.png"]],
    ["pick-atlas-frames-visually", ["Atlas Frames", "Index"], ["ref-returning-atlas-frame-picker.png"]],
    ["choose-a-skydome-ground-and-lighting", ["Game dome", "Ground", "Lighting"], ["ref-returning-skydome-picker.png"]],
    ["place-and-move-a-reference-object", ["gizmo", "Lock object", "Snap to grid"], ["faith.mp4", "ref-returning-reference-gizmo.png"]],
    ["launch-effects-with-the-spawner", ["Spawner", "Manual", "Auto"], ["tutorial-03-spawner-direction.mp4"]],
    ["pause-and-step-the-preview", ["Pause", "Step", "Step 10"], []],
    ["undo-recover-and-catch-overloads", ["Undo", "autosave", "overload"], []],
  ] as const;
  for (const [id, expectedTerms, expectedMedia] of workflows) {
    const heading = page.locator(`#${id}`);
    const section = await heading.evaluate((h) => {
      const elements: Element[] = [];
      let next = h.nextElementSibling;
      while (next && next.tagName !== "H2" && next.tagName !== "H3") {
        elements.push(next);
        next = next.nextElementSibling;
      }
      return {
        text: elements.map((el) => el.textContent?.trim() ?? "").join(" "),
        firstTags: elements.slice(0, 3).map((el) => el.tagName),
        firstTexts: elements.slice(0, 3).map((el) => el.textContent?.trim() ?? ""),
        figureCount: elements.filter((el) => el.tagName === "FIGURE").length,
        figureMedia: elements
          .filter((el) => el.tagName === "FIGURE")
          .map((figure) => {
            const media = figure.querySelector("video[data-clip], img[data-poster]");
            return media?.getAttribute("data-clip") ?? media?.getAttribute("data-poster");
          }),
      };
    });
    expect(section.firstTags, `${id} starts with three migration paragraphs`).toEqual(["P", "P", "P"]);
    expect(section.firstTexts[0], `${id} starts with the old workflow`).toMatch(/^Old editor:/);
    expect(section.firstTexts[1], `${id} follows with the current workflow`).toMatch(/^This editor:/);
    expect(section.firstTexts[2], `${id} gives the exact current location`).toMatch(/^Where to find it:/);
    for (const term of expectedTerms)
      expect(section.text, `${id} covers ${term}`).toContain(term);
    expect(section.figureCount, `${id} has the intended visual count`).toBe(expectedMedia.length);
    expect(section.figureMedia, `${id} uses its intended visual`).toEqual(expectedMedia);
  }

  const media = page.locator(".guide-article .guide-media");
  await expect(media).toHaveCount(11);
  const mediaCaptions = page.locator(".guide-article .guide-media-caption");
  await expect(mediaCaptions).toHaveCount(11);
  for (let i = 0; i < 11; i++)
    await expect(media.nth(i).locator("xpath=following-sibling::*[1]")).toHaveClass("guide-media-caption");

  const safetyText = await page.locator("#undo-recover-and-catch-overloads").evaluate((heading) => {
    const parts: string[] = [];
    let next = heading.nextElementSibling;
    while (next && next.tagName !== "H2") {
      parts.push(next.textContent?.trim() ?? "");
      next = next.nextElementSibling;
    }
    return parts.join(" ");
  });
  expect(safetyText).toContain("failed save leaves the previous .alo intact");
  expect(safetyText).toContain("Restore recent");
  expect(safetyText).toContain("Restore stable");

  const conveniences = page.locator("#everyday-conveniences")
    .locator("xpath=following-sibling::ul[1]");
  await expect(conveniences.locator(":scope > li")).toHaveCount(3);
  await expect(conveniences).toContainText("Help → Keyboard Shortcuts…");
  await expect(page.locator(".guide-article table")).toHaveCount(1);

  await page.setViewportSize({ width: 390, height: 844 });
  await page.reload();
  const mobileComparison = await departureTable.evaluate((table) => {
    const row = table.querySelector("tbody tr")!;
    const cell = row.querySelector("td")!;
    return {
      rowDisplay: getComputedStyle(row).display,
      cellDisplay: getComputedStyle(cell).display,
      tableWidth: table.getBoundingClientRect().width,
      articleWidth: document.querySelector(".guide-article")!.getBoundingClientRect().width,
    };
  });
  expect(mobileComparison.rowDisplay).toBe("block");
  expect(mobileComparison.cellDisplay).toBe("grid");
  expect(mobileComparison.tableWidth).toBeLessThanOrEqual(mobileComparison.articleWidth + 1);

  // App UI Quick Reference stays a control lookup instead of carrying a second audience's
  // orientation section at the bottom.
  await page.goto("/guide/app-ui-quick-reference.html");
  await expect(page.locator("#changed-from-the-old-glyphx-editor")).toHaveCount(0);
});

test("guide home points returning modders at their Start Here page", async ({ page }) => {
  await page.goto("/guide/home.html");
  const pointer = page.locator(
    '.guide-article a[href="./coming-from-the-old-glyphx-editor.html"]');
  await expect(pointer).toHaveCount(1);
  await expect(pointer.locator("xpath=parent::p")).toContainText(
    "If the editor and your mod stack already work, use Coming from the Old GlyphX Editor? as your migration map",
  );
});

test("a11y: no critical/serious axe violations in the returning-users article", async ({ page }) => {
  // The retired What's New page carried the site's only axe scan of a comparison table; the
  // table moved into this page, so its accessibility coverage follows it rather than
  // disappearing. Scoped to .guide-article — the article is the transferred coverage; the
  // sidebar/topbar chrome is shared furniture. The settle wait below is load-bearing: mid-fade,
  // axe measures opacity-blended colours and reports phantom contrast failures (settled, the
  // whole page is clean — verified). Guide-article links carry a rest-state underline
  // (guide.css) so they pass link-in-text-block without relying on colour.
  await page.goto("/guide/coming-from-the-old-glyphx-editor.html");
  // Wait out the guide-up entrance fade (guide.css animates .guide-layout > * from opacity 0)
  // or axe measures opacity-blended colors and reports phantom contrast failures.
  await page.waitForFunction(() =>
    [...document.querySelectorAll(".guide-layout > *")]
      .every((el) => getComputedStyle(el).opacity === "1"),
    null, { timeout: 5000 });
  const results = await new AxeBuilder({ page }).include(".guide-article")
    .withTags(["wcag2a", "wcag2aa"]).analyze();
  const bad = results.violations.filter((v) => ["critical", "serious"].includes(v.impact ?? ""));
  expect(bad, JSON.stringify(bad.map((v) => v.id))).toHaveLength(0);
});
