// Playwright specs for the AtlasPickerPanel click-to-assign flow
// against the real native bridge inside ParticleEditor.exe --test-host.
// Same CDP-attach harness as sibling specs (track-editor.spec.ts).
//
// What the specs cover end-to-end:
//   1. Auto-open: with an atlas-eligible emitter selected (the seeded
//      root emitter defaults to textureSize=64 → an 8×8 grid, with
//      colorTexture "p_particle_master.tga"), focusing the Index
//      channel and then selecting a key on it slides the Atlas Frames
//      panel into the right dock (autoOpenReducer's armed→keySelected
//      edge). Clicking a grid cell writes that frame to the selected
//      key via emitters/set-track-key — verified through
//      emitters/get-tracks.
//   2. Confirm modal: with TWO selected index keys whose current frames
//      differ (selection.frame === null), clicking a cell surfaces
//      AtlasConfirmModal instead of committing; "Set all" applies the
//      frame to both keys.
//
// Host-data dependency: the cell grid mounts only when
// textures/get-preview can decode the seed texture (game data reachable
// through the host's FileManager). Each test probes that first and
// runtime-skips (canvas-architecture.spec.ts precedent) with a clear
// reason instead of cascading selector timeouts when the texture is
// unavailable. Guard against the skip becoming a PERMANENT silent no-op
// (this file is the only e2e click-to-assign coverage): set
// PE_REQUIRE_GAME_TEXTURES=1 on a machine where game data is expected —
// the probe failure then FAILS the test instead of skipping.
//
// jsdom coverage of the same surface: AtlasPickerPanel.assign.test.tsx
// (assign + confirm semantics against the mock bridge) and
// use-atlas-autoopen.test.tsx (reducer edges). These specs lock the
// composed path: real curve-editor clicks → atlas context → dock →
// real host track mutation.

import { test, expect, chromium, type Page, type Browser, type Locator } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));

  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

// The picker mounts as a docked ToolPanel: role="dialog" with the title
// as aria-label (ToolPanel.tsx), same shape tools.spec.ts asserts for
// "Lighting".
const ATLAS_PANEL = '[role="dialog"][aria-label="Atlas Frames"]';

// Helper — select the seeded root emitter and reset its blendMode to
// Additive (1, the seed default; a property-tabs spec earlier in the
// run leaves BLEND_BUMP). Additive is not alpha-gated, so the picker's
// dead-cell detection stays off and no cell click can be blocked by an
// empty-alpha frame.
async function selectFirstEmitter(): Promise<number> {
  return page.evaluate(async () => {
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
    await bridge.request({
      kind: "emitters/set-properties",
      params: { id, patch: { blendMode: 1 } },
    });
    return id;
  });
}

// Helper — skip (or fail, under PE_REQUIRE_GAME_TEXTURES) when the host
// can't decode the seed atlas texture; the grid only mounts on a
// status:"ok" preview.
function skipUnlessTextureOk(previewStatus: string) {
  const reason =
    `textures/get-preview returned '${previewStatus}' for p_particle_master.tga — ` +
    "the atlas grid cannot mount against this host (game texture data unreachable)";
  if (previewStatus !== "ok" && process.env.PE_REQUIRE_GAME_TEXTURES) {
    throw new Error(`PE_REQUIRE_GAME_TEXTURES is set but ${reason}`);
  }
  test.skip(previewStatus !== "ok", reason);
}

// Helper — probe whether the host can actually decode the seed atlas
// texture; the grid only mounts on a status:"ok" preview.
async function seedTexturePreviewStatus(): Promise<string> {
  return page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    const ask = async () =>
      (await bridge!.request({
        kind: "textures/get-preview",
        params: { filename: "p_particle_master.tga", flattenAlpha: true },
      })) as { status: string };

    // Preview encoding is ASYNC: a cache miss answers `pending` and the real
    // result lands later (the host's PreviewEncodeWorker posts back and the
    // next request is a cache hit). Returning that first `pending` straight to
    // the caller made it read as "game texture data unreachable" — so with
    // PE_REQUIRE_GAME_TEXTURES newly set, these specs failed on a machine whose
    // textures were perfectly reachable, just not decoded YET (2026-07 audit,
    // found while fixing an-audit-finding).
    //
    // Poll to a terminal answer. `error` still means genuinely unavailable,
    // which is what the skip is for.
    let r = await ask();
    const deadline = Date.now() + 5000;
    while (r.status === "pending" && Date.now() < deadline) {
      await new Promise((res) => setTimeout(res, 100));
      r = await ask();
    }
    return r.status;
  });
}

// Helper — seed an interior key on the index track via the bridge
// (mirrors the track-editor.spec.ts seeding pattern) and return the
// actually-inserted time (the host epsilon-bumps duplicates).
async function addIndexKey(id: number, time: number, value: number): Promise<number> {
  return page.evaluate(async (args: { id: number; time: number; value: number }) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    const r = await bridge!.request({
      kind: "emitters/add-track-key",
      params: { id: args.id, track: "index", time: args.time, value: args.value },
    }) as { time: number };
    return r.time;
  }, { id, time, value });
}

// Helper — read the index-track key value at (approximately) `time`.
async function indexKeyValueAt(id: number, time: number): Promise<number | null> {
  return page.evaluate(async (args: { id: number; time: number }) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    const tracks = await bridge!.request({
      kind: "emitters/get-tracks",
      params: { id: args.id },
    }) as { tracks: { name: string; keys: { time: number; value: number }[] }[] };
    const idx = tracks.tracks.find((t) => t.name === "index");
    const key = idx?.keys.find((k) => Math.abs(k.time - args.time) < 1e-3);
    return key ? key.value : null;
  }, { id, time });
}

// Helper — focus the Index channel with a FRESH focus edge: the
// auto-open reducer only (re)arms on a focusOther→focusIndex
// transition, so we land on Red first (a no-op if already focused),
// then click the Index row (which also solos it — only the focus
// channel renders interactive curve-key circles).
async function focusIndexChannelFresh() {
  const panel = page.locator('[data-testid="curve-editor-panel"]');
  await expect(panel).toBeVisible({ timeout: 5_000 });
  await panel.locator('[data-testid="curve-channel-row-red"]').click();
  await expect(panel).toHaveAttribute("data-focus-channel", "red", { timeout: 5_000 });
  await panel.locator('[data-testid="curve-channel-row-index"]').click();
  await expect(panel).toHaveAttribute("data-focus-channel", "index", { timeout: 5_000 });
  return panel;
}

// Helper — cleanup: delete the seeded interior keys, hand focus back to
// Red (fires the reducer's focusOther → restore command, sliding the
// auto-opened dock back to whatever it displaced), and deselect.
async function cleanup(id: number, times: number[]) {
  await page.evaluate(async (args: { id: number; times: number[] }) => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    await bridge!.request({
      kind: "emitters/delete-track-keys",
      params: { id: args.id, track: "index", times: args.times },
    });
  }, { id, times });
  const panel = page.locator('[data-testid="curve-editor-panel"]');
  await panel.locator('[data-testid="curve-channel-row-red"]').click();
  await expect(panel).toHaveAttribute("data-focus-channel", "red", { timeout: 5_000 });
  await page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    await bridge!.request({ kind: "emitters/select", params: { id: null } });
  });
}

// Helper — click frame N on the single <canvas> grid (post-#572: no per-cell
// DOM). Reads the grid geometry the canvas publishes (data-atlas-cols/-cell/
// -gap), computes frame N's center relative to the canvas top-left, and clicks
// there — the inverse of the component's frameFromEvent hit-test.
async function clickAtlasFrame(atlas: Locator, frame: number): Promise<void> {
  const canvas = atlas.locator('[data-testid="atlas-canvas"]');
  await expect(canvas).toBeVisible({ timeout: 10_000 }); // preview fetch + decode
  const pos = await canvas.evaluate((el, f) => {
    const cols = Number(el.getAttribute("data-atlas-cols"));
    const cell = Number(el.getAttribute("data-atlas-cell"));
    const gap = Number(el.getAttribute("data-atlas-gap"));
    const step = cell + gap;
    return { x: (f % cols) * step + cell / 2, y: Math.floor(f / cols) * step + cell / 2 };
  }, frame);
  await canvas.click({ position: pos });
}

// ── 1. Auto-open + single-key click-to-assign ────────────────────────

test("selecting an index key on an atlas-eligible emitter auto-opens the picker; clicking a cell assigns the frame", async () => {
  const firstId = await selectFirstEmitter();

  skipUnlessTextureOk(await seedTexturePreviewStatus());

  // Seed one interior index key so the click target sits mid-canvas
  // (border keys hug the plot edges).
  const keyTime = await addIndexKey(firstId, 50, 2);

  // Cleanup in finally: a mid-test failure must not leak the seeded key,
  // index focus, or selection into later specs of the shared CDP session.
  try {
    const panel = await focusIndexChannelFresh();

    // Clicking the key is the selection rising edge that fires the
    // reducer's keySelected → open command.
    const key = panel.locator(
      `[data-testid="curve-key"][data-channel-id="index"][data-key-time="${keyTime}"]`,
    );
    await expect(key).toBeVisible({ timeout: 5_000 });
    await key.click();

    const atlas = page.locator(ATLAS_PANEL);
    await expect(atlas).toBeVisible({ timeout: 5_000 });

    // Click frame 3 in the grid — assigns it to the selected key (single
    // key → selection.frame is non-null → direct commit, no confirm).
    await clickAtlasFrame(atlas, 3);

    await expect
      .poll(() => indexKeyValueAt(firstId, keyTime), { timeout: 5_000 })
      .toBe(3);
  } finally {
    await cleanup(firstId, [keyTime]);
  }
});

// ── 2. Multi-key differing frames → AtlasConfirmModal ────────────────

test("two selected index keys with differing frames surface AtlasConfirmModal; Set all applies to both", async () => {
  const firstId = await selectFirstEmitter();

  skipUnlessTextureOk(await seedTexturePreviewStatus());

  // Two interior keys with DIFFERING frame floors (1 vs 2) so the
  // published selection.frame collapses to null.
  const t1 = await addIndexKey(firstId, 30, 1);
  const t2 = await addIndexKey(firstId, 60, 2);

  // Cleanup in finally — see test 1.
  try {
    const panel = await focusIndexChannelFresh();

    // Select the first key (auto-open fires on this rising edge), then
    // Ctrl+click the second to grow the selection additively.
    const key1 = panel.locator(
      `[data-testid="curve-key"][data-channel-id="index"][data-key-time="${t1}"]`,
    );
    await expect(key1).toBeVisible({ timeout: 5_000 });
    await key1.click();

    const atlas = page.locator(ATLAS_PANEL);
    await expect(atlas).toBeVisible({ timeout: 5_000 });

    const key2 = panel.locator(
      `[data-testid="curve-key"][data-channel-id="index"][data-key-time="${t2}"]`,
    );
    await expect(key2).toBeVisible({ timeout: 5_000 });
    await key2.click({ modifiers: ["Control"] });

    // Clicking a cell with a mixed-frame multi-selection must NOT commit —
    // it opens the confirm modal.
    await clickAtlasFrame(atlas, 5);

    const modal = page
      .locator('[role="dialog"][data-state="open"]')
      .filter({ hasText: "Set frame for all selected keys" });
    await expect(modal).toBeVisible({ timeout: 2_000 });
    await expect(modal).toContainText("These 2 keys have different frames");
    // Neither key committed while the modal is up.
    expect(await indexKeyValueAt(firstId, t1)).toBeCloseTo(1, 3);
    expect(await indexKeyValueAt(firstId, t2)).toBeCloseTo(2, 3);

    // Confirm — applies frame 5 to BOTH selected keys.
    await modal.getByRole("button", { name: "Set all" }).click();
    await expect(modal).toHaveCount(0);
    await expect
      .poll(async () => [
        await indexKeyValueAt(firstId, t1),
        await indexKeyValueAt(firstId, t2),
      ], { timeout: 5_000 })
      .toEqual([5, 5]);
  } finally {
    await cleanup(firstId, [t1, t2]);
  }
});
