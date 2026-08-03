// motion-coverage.spec.ts — regression fence for the 2026-07-18 design pass
// ("no hard cuts"). Two layers:
//
//   1. INVARDS (computed-style invariants): the high-traffic interactive
//      classes carry transitions; every Radix menu/select content node wears
//      its animate class. Hard cuts can't silently return.
//   2. BEHAVIOR: rapid open/close of the animated Radix surfaces leaves no
//      stuck data-state / orphan portal and returns focus to the trigger;
//      reduced-motion and [data-recording] both make the motion inert.
//
// Runs against the MOCK app on the Vite dev server (playwright.web.config.ts)
// — real Chromium, no native host.
import { test, expect, type Page } from "@playwright/test";

async function boot(page: Page) {
  await page.goto("/");
  await page.waitForSelector('[data-testid="emitter-tree"]');
}

test("interactive chrome carries hover/press transitions (no hard cuts)", async ({ page }) => {
  await boot(page);
  const bare = await page.evaluate(() => {
    const sels = [".tb-btn", ".panel-section-header"];
    const out: string[] = [];
    for (const sel of sels) {
      const el = document.querySelector(sel);
      if (!el) { out.push(`${sel}: MISSING`); continue; }
      if (parseFloat(getComputedStyle(el).transitionDuration) === 0) out.push(`${sel}: no transition`);
    }
    // App-wide: enabled buttons with a hover: style change must transition.
    // (Disabled-only variants legitimately have no hover state.)
    const bareButtons = [...document.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")]
      .filter((b) => /hover:/.test(b.className) && parseFloat(getComputedStyle(b).transitionDuration) === 0)
      .map((b) => b.getAttribute("aria-label") ?? b.className.slice(0, 40));
    return { out, bareButtons };
  });
  expect(bare.out).toEqual([]);
  expect(bare.bareButtons).toEqual([]);
});

test("menubar menus animate, survive rapid toggling, and return focus", async ({ page }) => {
  await boot(page);
  // Open File → content wears popover-animate and plays the entrance.
  await page.click('[role="menubar"] [role="menuitem"]:has-text("File")');
  const menu = page.locator('[role="menu"]');
  await expect(menu).toBeVisible();
  const anim = await menu.evaluate((el) => ({
    cls: /popover-animate/.test(el.className),
    name: getComputedStyle(el).animationName,
  }));
  expect(anim.cls).toBe(true);
  expect(anim.name).toBe("popover-pop-in");
  await page.keyboard.press("Escape");
  await expect(menu).toHaveCount(0);

  // Rapid toggle ×10 → exactly zero or one portal, no stuck closed-state node.
  for (let i = 0; i < 10; i += 1) {
    await page.click('[role="menubar"] [role="menuitem"]:has-text("File")');
    await page.waitForTimeout(15);
  }
  await page.waitForTimeout(300);
  const stuck = await page.evaluate(() => ({
    menus: document.querySelectorAll('[role="menu"]').length,
    closedGhosts: document.querySelectorAll('[role="menu"][data-state="closed"]').length,
  }));
  expect(stuck.closedGhosts).toBe(0);
  expect(stuck.menus).toBeLessThanOrEqual(1);
  await page.keyboard.press("Escape");
  await expect(page.locator('[role="menu"]')).toHaveCount(0);
  // Focus back on the trigger after Escape.
  const focused = await page.evaluate(() => document.activeElement?.textContent);
  expect(focused).toBe("File");
});

test("select dropdowns wear the entrance-only class (Radix Select can't animate exits)", async ({ page }) => {
  await boot(page);
  // Use the inspector's Appearance tab — its FieldSelects (e.g. blend mode)
  // are enabled once an emitter is selected.
  await page.click('[data-testid="emitter-row:0"]');
  await page.click('[data-testid="tab-trigger-appearance"]');
  const trigger = page.locator('[data-testid="tab-appearance-content"] [role="combobox"]').first();
  await trigger.click();
  const content = page.locator("[data-radix-select-viewport]").first();
  await expect(content).toBeVisible();
  const cls = await content.evaluate((el) => {
    const host = el.closest('[class*="popover-animate-in"]');
    return host !== null;
  });
  expect(cls).toBe(true);
  await page.keyboard.press("Escape");
});

test("reduced-motion makes the design-pass motion inert", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await boot(page);
  await page.click('[role="menubar"] [role="menuitem"]:has-text("File")');
  const anim = await page.locator('[role="menu"]').evaluate((el) => getComputedStyle(el).animationName);
  expect(anim).toBe("none");
  const btn = await page.locator(".tb-btn").first().evaluate((el) => getComputedStyle(el).transitionDuration);
  expect(parseFloat(btn)).toBe(0);
  await page.keyboard.press("Escape");
});

test("[data-recording] kill-switch holds all pass animation inert", async ({ page }) => {
  await boot(page);
  await page.evaluate(() => document.documentElement.setAttribute("data-recording", ""));
  await page.click('[role="menubar"] [role="menuitem"]:has-text("File")');
  const anim = await page.locator('[role="menu"]').evaluate((el) => getComputedStyle(el).animationName);
  expect(anim).toBe("none");
  const probes = await page.evaluate(() => ({
    btn: getComputedStyle(document.querySelector(".tb-btn")!).transitionDuration,
    tab: getComputedStyle(document.querySelector('[role="tabpanel"][data-state="active"]')!).animationName,
  }));
  expect(parseFloat(probes.btn)).toBe(0);
  expect(probes.tab).toBe("none");
});

test("theme flip runs the scoped cross-fade class and settles clean", async ({ page }) => {
  await boot(page);
  const result = await page.evaluate(async () => {
    const root = document.documentElement;
    const seen = { cls: false };
    const mo = new MutationObserver(() => {
      if (root.classList.contains("theme-transition")) seen.cls = true;
    });
    mo.observe(root, { attributes: true, attributeFilter: ["class"] });
    const themeMod = await import("/src/lib/theme.ts");
    const prev = root.dataset.theme;
    themeMod.applyMode(prev === "dark" ? "light" : "dark");
    await new Promise((r) => setTimeout(r, 350));
    mo.disconnect();
    const settled = !root.classList.contains("theme-transition");
    themeMod.applyMode(prev === "dark" ? "dark" : "light"); // restore
    return { seen: seen.cls, settled };
  });
  expect(result.seen).toBe(true);
  expect(result.settled).toBe(true);
});

test("record-cursor can resolve a REAL emitter-tree row target (frozen contract)", async ({ page }) => {
  await boot(page);
  // The --record pipeline targets rows by `testid:emitter-row:<id>` and by
  // their accessible name; both must resolve on the live tree DOM (the
  // synthetic-fixture spec record-cursor-targeting.spec.ts does not cover
  // this). Guards the B2 restructure and any future row surgery.
  const row = page.locator('[data-testid="emitter-row:0"]');
  await expect(row).toHaveCount(1);
  const box = await row.boundingBox();
  expect(box).not.toBeNull();
  expect(box!.width).toBeGreaterThan(50);
  // The row's stable data contract for cursor resolution:
  const attrs = await row.evaluate((el) => ({
    emitterId: el.getAttribute("data-emitter-id"),
    focusable: (el as HTMLElement).tabIndex,
  }));
  expect(attrs.emitterId).toBe("0");
  expect([0, -1]).toContain(attrs.focusable);
  // Eye toggle: separate, real button with its frozen testid + name.
  const eye = page.locator('[data-testid="emitter-vis-0"]');
  await expect(eye).toHaveCount(1);
  await expect(eye).toHaveAttribute("aria-label", /Hide emitter|Show emitter/);
});
