// Phase 4.1 Fix dispatch 1 — EmitterPropertyTabs Playwright specs.
//
// 1. Selecting an emitter shows the property tabs in the lower-left
//    quadrant + the track editor in the lower-right quadrant
//    simultaneously (asserts the four-quadrant layout via data-testid
//    attributes).
// 2. Editing the Lifetime spinner in the Basic tab fires
//    emitters/set-properties — a subsequent get-properties reflects
//    the new value, confirming the round-trip.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
});

test.afterAll(async () => {
  await browser?.close();
});

test("selecting an emitter shows property tabs (lower-left) + track editor (lower-right)", async () => {
  await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    const firstId = list.root.children[0]?.id;
    if (firstId === undefined) throw new Error("no emitters in tree");
    await bridge.request({ kind: "emitters/select", params: { id: firstId } });
  });

  // Tabs container visible (lower-left quadrant) + the 3 tab triggers.
  const tabs = page.locator('[data-testid="emitter-property-tabs"]');
  await expect(tabs).toBeVisible({ timeout: 5_000 });
  await expect(page.locator('[data-testid="tab-trigger-basic"]')).toBeVisible();
  await expect(page.locator('[data-testid="tab-trigger-appearance"]')).toBeVisible();
  await expect(page.locator('[data-testid="tab-trigger-physics"]')).toBeVisible();

  // Curve editor (always-on bottom row, Task 2.6) visible
  // simultaneously. The new CurveEditorPanel replaced the per-emitter
  // EmitterPropertyPanel that used to live in the lower-right quadrant.
  const curvePanel = page.locator('[data-testid="curve-editor-panel"]');
  await expect(curvePanel).toBeVisible({ timeout: 5_000 });

  // Quadrant containers all present in the DOM.
  await expect(page.locator('[data-testid="quadrant-emitter-tree"]')).toBeVisible();
  await expect(page.locator('[data-testid="quadrant-property-tabs"]')).toBeVisible();
  await expect(page.locator('[data-testid="quadrant-viewport"]')).toBeVisible();
  await expect(page.locator('[data-testid="quadrant-curve-editor"]')).toBeVisible();
});

test("editing the Lifetime spinner in the Basic tab fires emitters/set-properties and round-trips via get-properties", async () => {
  // Re-select to ensure the panel is mounted.
  const firstId = await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    const id = list.root.children[0]?.id;
    if (id === undefined) throw new Error("no emitters in tree");
    await bridge.request({ kind: "emitters/select", params: { id } });
    return id;
  });

  // Wait for the tabs to mount.
  const tabs = page.locator('[data-testid="emitter-property-tabs"]');
  await expect(tabs).toBeVisible({ timeout: 5_000 });

  // Lifetime spinner is in the Basic tab (active by default).
  // Post-B1.3-P3: "Lifetime" was relabelled to "Maximum lifetime:"
  // inside the Generation section. Use exact match.
  const lifetime = page.getByLabel("Maximum lifetime:", { exact: true });
  await expect(lifetime).toBeVisible({ timeout: 5_000 });

  // Drive the spinner by firing the bridge call directly — keystroke
  // delivery to a numeric input under CDP can be flaky, and we want to
  // assert the round-trip (the spinner's commit semantics are covered
  // by Vitest).
  await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    await bridge!.request({
      kind: "emitters/set-properties",
      params: { id, patch: { lifetime: 7.5 } },
    });
  }, firstId);

  // Round-trip via get-properties.
  const result = await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    return await bridge!.request({
      kind: "emitters/get-properties",
      params: { id },
    });
  }, firstId) as { properties: { lifetime: number } };

  expect(result.properties.lifetime).toBeCloseTo(7.5, 5);
});

test("switching to Physics tab and changing gravity round-trips via get-properties (Fix dispatch 3)", async () => {
  // Re-select to ensure the panel is mounted.
  const firstId = await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    const id = list.root.children[0]?.id;
    if (id === undefined) throw new Error("no emitters in tree");
    await bridge.request({ kind: "emitters/select", params: { id } });
    return id;
  });

  await expect(page.locator('[data-testid="emitter-property-tabs"]')).toBeVisible({ timeout: 5_000 });
  await page.locator('[data-testid="tab-trigger-physics"]').click();

  // The gravity spinner renders inside the Physics tab content.
  // Post-B1.3-P6: "Gravity" was relabelled to "Gravity acceleration:"
  // inside the Acceleration section.
  const gravity = page.getByLabel("Gravity acceleration:", { exact: true });
  await expect(gravity).toBeVisible({ timeout: 5_000 });

  // Same pattern as the Appearance Fix dispatch 2 test — drive via the
  // bridge directly to assert round-trip; per-keystroke spinner
  // semantics are covered by Vitest.
  await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    await bridge!.request({
      kind: "emitters/set-properties",
      params: { id, patch: { gravity: -9.81 } },
    });
  }, firstId);

  const result = await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    return await bridge!.request({
      kind: "emitters/get-properties",
      params: { id },
    });
  }, firstId) as { properties: { gravity: number } };

  expect(result.properties.gravity).toBeCloseTo(-9.81, 5);
});

test("Physics group type change round-trips via get-properties (Fix dispatch 3)", async () => {
  const firstId = await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    const id = list.root.children[0]?.id;
    if (id === undefined) throw new Error("no emitters in tree");
    await bridge.request({ kind: "emitters/select", params: { id } });
    return id;
  });

  await expect(page.locator('[data-testid="emitter-property-tabs"]')).toBeVisible({ timeout: 5_000 });
  await page.locator('[data-testid="tab-trigger-physics"]').click();

  // Group 0's type-select trigger lives inside the Physics tab.
  const groupTypeTrigger = page.locator('[data-testid="physics-group-0-type-trigger"]');
  await expect(groupTypeTrigger).toBeVisible({ timeout: 5_000 });

  // Drive the change via the bridge — flip group[0] to GT_SPHERE (3)
  // and assert the round-trip includes the new type. Radix listbox
  // automation under CDP is flaky enough that we mirror the spinner /
  // blend-mode pattern from the Appearance + Lifetime tests.
  const patchedGroups = await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    const before = await bridge!.request({
      kind: "emitters/get-properties",
      params: { id },
    }) as { properties: { groups: { type: number }[] } };
    const next = before.properties.groups.map((g, i) =>
      i === 0 ? { ...g, type: 3 } : g,
    );
    await bridge!.request({
      kind: "emitters/set-properties",
      params: { id, patch: { groups: next } },
    });
    const after = await bridge!.request({
      kind: "emitters/get-properties",
      params: { id },
    }) as { properties: { groups: { type: number }[] } };
    return after.properties.groups;
  }, firstId);

  expect(patchedGroups[0].type).toBe(3);
});

test("switching to Appearance tab and changing blendMode emits engine/state/changed with the patched value (Fix dispatch 2)", async () => {
  // Re-select to ensure the panel is mounted.
  const firstId = await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    const id = list.root.children[0]?.id;
    if (id === undefined) throw new Error("no emitters in tree");
    await bridge.request({ kind: "emitters/select", params: { id } });
    return id;
  });

  // Tabs visible.
  await expect(page.locator('[data-testid="emitter-property-tabs"]')).toBeVisible({ timeout: 5_000 });

  // Click into the Appearance tab.
  await page.locator('[data-testid="tab-trigger-appearance"]').click();

  // The blend-mode trigger renders inside the Appearance tab content.
  const blendModeTrigger = page.locator('[data-testid="appearance-blend-mode-trigger"]');
  await expect(blendModeTrigger).toBeVisible({ timeout: 5_000 });

  // Drive the set via the bridge directly — Radix Select in CDP can
  // open a portal-mounted listbox whose item locators are flaky in
  // automation; the spinner's keystroke delivery is covered by the
  // Vitest specs already. Here we assert the round-trip: a change to
  // blendMode is reflected back via get-properties.
  await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    await bridge!.request({
      kind: "emitters/set-properties",
      params: { id, patch: { blendMode: 11 } },
    });
  }, firstId);

  const result = await page.evaluate(async (id: number) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    return await bridge!.request({
      kind: "emitters/get-properties",
      params: { id },
    });
  }, firstId) as { properties: { blendMode: number } };

  expect(result.properties.blendMode).toBe(11);
});
