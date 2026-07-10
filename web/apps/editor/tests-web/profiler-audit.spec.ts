// React re-render audit (perf follow-up loose end A).
//
// Drives the live mock app (vite dev, MockBridge) under a set of scripted
// interactions and reads per-component React <Profiler> commit counts from the
// DEV-only window.__profilerAudit seam (src/dev/profiler-audit.ts), which wraps
// the five #532 components at their App.tsx / PanelLayout.tsx mount sites.
//
// Goal: a RANKED table of remaining re-render sources — not a pass/fail gate.
// The spec asserts only that the harness itself works (each interaction produces
// the commits it must, so a broken measurement fails loudly); it does NOT assert
// rankings. Results are attached + console-logged; the ranked table is transcribed
// by hand into tasks/2026-07-07-react-profiler-audit-plan.md.
//
// Metric note (soundness-1): a per-id count is a SUBTREE commit count — onRender
// fires when that Profiler's subtree commits, which includes descendants. Rank by
// count; treat it as "this region re-rendered", strongest for the small-subtree
// components (Toolbar, StatusBar). StrictMode (soundness-2) may inflate DEV counts;
// we report raw counts and rank relatively, and the baseline-mount step below makes
// any uniform doubling visible rather than assumed.

import { test, expect } from "@playwright/test";

type Aggregate = {
  commits: number;
  mounts: number;
  updates: number;
  totalActualMs: number;
  distinctCommitTimes: number;
};
type Dump = Record<string, Aggregate>;

const PROFILED_IDS = [
  "Toolbar",
  "StatusBar",
  "EmitterTree",
  "CurveEditorPanel",
  "AtlasPickerPanel",
] as const;

declare global {
  interface Window {
    __profilerAudit?: {
      record: (...a: unknown[]) => void;
      reset: () => void;
      rows: () => unknown[];
      dump: () => Dump;
      emitCursor: (x: number, y: number, z: number) => void;
      emitStats: (s: {
        fps: number;
        emitters: number;
        particles: number;
        instances: number;
        overload: boolean;
      }) => void;
    };
    __atlasTest?: { seedAtlas: (opts?: { textureSize?: number }) => void };
  }
}

/** Drain scheduled work until the collected row count is stable across two
 *  rAF+macrotask cycles (bounded) — so trailing commits are counted and don't
 *  leak across a reset() boundary. */
async function settle(page: import("@playwright/test").Page): Promise<void> {
  await page.evaluate(async () => {
    const raf = () => new Promise<void>((r) => requestAnimationFrame(() => r()));
    const macro = () => new Promise<void>((r) => setTimeout(() => r(), 0));
    let prev = -1;
    let cur = window.__profilerAudit!.rows().length;
    for (let i = 0; i < 30 && cur !== prev; i++) {
      prev = cur;
      await raf();
      await raf();
      await macro();
      cur = window.__profilerAudit!.rows().length;
    }
  });
}

/** One measured interaction: drain prior work, reset, run `action`, drain again,
 *  then dump — so the count is complete and isolated from neighbouring rows. */
async function measure(
  page: import("@playwright/test").Page,
  action: () => Promise<void>,
): Promise<Dump> {
  await settle(page);
  await page.evaluate(() => window.__profilerAudit!.reset());
  await action();
  await settle(page);
  return page.evaluate(() => window.__profilerAudit!.dump());
}

test("react re-render audit: per-component commit counts under scripted interactions", async ({
  page,
}, testInfo) => {
  const results: Record<string, Dump> = {};

  await page.goto("/");
  await page.waitForFunction(() => typeof window.__profilerAudit?.dump === "function");
  // window.bridge is set by AppShell (synchronously in useMemo + corrected in an
  // effect); wait for it so the synthetic-event drivers have a live bridge.
  await page.waitForFunction(() => typeof (window as { bridge?: unknown }).bridge !== "undefined");
  // Mock boots roots 0/3/5 selected on 0; wait for the tree to actually render.
  await page.locator('[data-testid^="emitter-row:"]').first().waitFor();

  // --- baseline mount: what the initial load commits (StrictMode calibration) ---
  results["baseline-mount"] = await page.evaluate(() => window.__profilerAudit!.dump());

  // --- StatusBar: synthesized cursor/position-3d storm, ONE emit per animation
  //     frame so React batching can't collapse N events into one commit
  //     (assert distinct commitTimes). This is the real ~30 Hz driver the mock
  //     never emits organically. ---
  results["statusbar-cursor-x30"] = await measure(page, async () => {
    await page.evaluate(async () => {
      const raf = () => new Promise<void>((r) => requestAnimationFrame(() => r()));
      for (let i = 0; i < 30; i++) {
        window.__profilerAudit!.emitCursor(i, i * 2, 0);
        await raf();
      }
    });
  });

  // --- StatusBar: synthesized stats/tick storm (4 Hz secondary driver) ---
  results["statusbar-stats-x8"] = await measure(page, async () => {
    await page.evaluate(async () => {
      const raf = () => new Promise<void>((r) => requestAnimationFrame(() => r()));
      for (let i = 0; i < 8; i++) {
        window.__profilerAudit!.emitStats({
          fps: 60 - i,
          emitters: 3,
          particles: 100 + i,
          instances: 5,
          overload: false,
        });
        await raf();
      }
    });
  });

  // --- Toolbar/broadcast fan-out: one pause toggle → engine/state/changed → which
  //     of the five profiled regions re-render on a single broadcast? ---
  results["toolbar-pause-toggle"] = await measure(page, async () => {
    const btn = page.locator('[aria-label="Pause"], [aria-label="Play"]').first();
    await btn.click();
    await page.waitForTimeout(50);
  });

  // --- EmitterTree: selection storm (click each root row) → tree re-render +
  //     emitters/selected → CurveEditorPanel refetch. ---
  results["emitter-tree-selection"] = await measure(page, async () => {
    const rows = page.locator('[data-testid^="emitter-row:"]');
    const n = Math.min(await rows.count(), 6);
    for (let i = 0; i < n; i++) {
      await rows.nth(i).click();
      await page.waitForTimeout(30);
    }
  });

  // --- AtlasPickerPanel: open via the atlas seam, then hover several frames.
  //     Post-#572 the grid is a single <canvas> (no per-cell DOM); hover is
  //     imperative (rAF canvas repaint) — expected to NOT re-render the panel;
  //     this confirms the #532 protection. Required: seedAtlas must exist and the
  //     grid must mount, so a 0 here is a real finding, not an absence. ---
  let atlasFramesHovered = 0;
  const atlasSeedable = await page.evaluate(
    () => typeof window.__atlasTest?.seedAtlas === "function",
  );
  expect(atlasSeedable, "atlas seam present (seedAtlas)").toBe(true);
  await page.evaluate(() => window.__atlasTest!.seedAtlas({ textureSize: 64 }));
  const atlasCanvas = page.locator('[data-testid="atlas-canvas"]');
  await atlasCanvas.first().waitFor();
  const atlasGeom = await atlasCanvas.evaluate((el) => ({
    cols: Number(el.getAttribute("data-atlas-cols")),
    cell: Number(el.getAttribute("data-atlas-cell")),
    gap: Number(el.getAttribute("data-atlas-gap")),
  }));
  results["atlas-hover"] = await measure(page, async () => {
    const step = atlasGeom.cell + atlasGeom.gap;
    for (let f = 0; f < 8; f++) {
      const x = (f % atlasGeom.cols) * step + atlasGeom.cell / 2;
      const y = Math.floor(f / atlasGeom.cols) * step + atlasGeom.cell / 2;
      await atlasCanvas.hover({ position: { x, y } });
      await page.waitForTimeout(20);
      atlasFramesHovered++;
    }
  });

  // --- CurveEditorPanel: BEST-EFFORT key drag (the liveDrag per-pointer-move path).
  //     Guarded — only runs if curve key markers are present, so a missing target
  //     is logged as "not measured" instead of flaking the spec. ---
  const keyMarkers = page.locator(".curve-key-marker");
  const curveMeasured = (await keyMarkers.count()) > 0;
  if (curveMeasured) {
    results["curve-key-drag"] = await measure(page, async () => {
      const box = await keyMarkers.first().boundingBox();
      if (box) {
        await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
        await page.mouse.down();
        for (let dx = 4; dx <= 40; dx += 4) {
          await page.mouse.move(box.x + box.width / 2 + dx, box.y + box.height / 2, { steps: 1 });
          await page.waitForTimeout(16);
        }
        await page.mouse.up();
      }
    });
  }

  // (A synthetic EmitterTree reorder drag was trialled but did not reliably hit a
  // drop target under the FLIP-glide drag controller, yielding an all-zero row that
  // measures nothing — omitted rather than ship a misleading result. EmitterTree's
  // reorder re-render surface is covered by the jsdom EmitterTree.multidrag test.)

  // --- Diagnostic: is the synthetic-emit path wired? (window.bridge is the live
  //     MockBridge; one emit should produce a StatusBar Profiler row.) ---
  const diag = await page.evaluate(async () => {
    /* eslint-disable @typescript-eslint/no-explicit-any */
    const b = window.bridge as any;
    const cursorListeners = b?.listeners?.get?.("cursor/position-3d")?.size ?? null;
    const statsListeners = b?.listeners?.get?.("stats/tick")?.size ?? null;
    window.__profilerAudit!.reset();
    window.__profilerAudit!.emitCursor(7, 8, 9);
    await new Promise((r) => requestAnimationFrame(() => r(null)));
    await new Promise((r) => setTimeout(r, 60));
    return {
      hasBridge: !!window.bridge,
      bridgeCtor: b?.constructor?.name ?? null,
      emitType: typeof b?.emit,
      cursorListeners,
      statsListeners,
      rowsAfterOneCursorEmit: window.__profilerAudit!.rows(),
    };
    /* eslint-enable @typescript-eslint/no-explicit-any */
  });

  // --- Emit the ranked table ---
  const ranked = Object.entries(results).map(([interaction, dump]) => {
    const row: Record<string, number | string> = { interaction };
    for (const id of PROFILED_IDS) row[id] = dump[id]?.commits ?? 0;
    return row;
  });
  // eslint-disable-next-line no-console
  console.log("\n=== React re-render audit (commits per Profiled region) ===");
  // eslint-disable-next-line no-console
  console.table(ranked);
  // eslint-disable-next-line no-console
  console.log("[diag]", JSON.stringify(diag));
  if (!curveMeasured) {
    // eslint-disable-next-line no-console
    console.log("[skip] curve-key-drag: no .curve-key-marker present — not measured");
  }
  await testInfo.attach("profiler-audit.json", {
    body: JSON.stringify({ results, ranked, diag }, null, 2),
    contentType: "application/json",
  });

  // --- Harness-sanity assertions: prove the MEASUREMENT worked (not the rankings,
  //     which are data). A dead bridge, a missing interaction, or a silently-absent
  //     region must fail loudly rather than masquerade as a "0 re-renders" finding. ---

  // The synthetic-emit path is live: one cursor emit produced a StatusBar row.
  expect(diag.hasBridge, "window.bridge present").toBe(true);
  expect(diag.emitType, "window.bridge.emit is callable").toBe("function");
  expect(
    diag.cursorListeners ?? 0,
    "StatusBar subscribed to cursor/position-3d on window.bridge",
  ).toBeGreaterThan(0);
  expect(
    diag.rowsAfterOneCursorEmit.some((r) => (r as { id?: string }).id === "StatusBar"),
    "one cursor emit re-rendered StatusBar",
  ).toBe(true);

  // Every required interaction ran (optional curve-key-drag is exempt).
  for (const key of [
    "baseline-mount",
    "statusbar-cursor-x30",
    "statusbar-stats-x8",
    "toolbar-pause-toggle",
    "emitter-tree-selection",
    "atlas-hover",
  ]) {
    expect(results[key], `interaction "${key}" ran`).toBeTruthy();
  }

  // StatusBar's synthetic cursor storm: ~1 commit per emit (30), each in a distinct
  // frame — mechanically backing §9's "not batched" claim (soundness-2).
  const cur = results["statusbar-cursor-x30"]["StatusBar"];
  expect(cur?.commits ?? 0, "cursor storm re-rendered StatusBar ~30×").toBeGreaterThanOrEqual(25);
  expect(
    cur?.distinctCommitTimes ?? 0,
    "cursor commits landed in distinct frames (no batching)",
  ).toBeGreaterThanOrEqual(25);
  expect(
    results["statusbar-stats-x8"]["StatusBar"]?.commits ?? 0,
    "stats storm re-rendered StatusBar",
  ).toBeGreaterThanOrEqual(6);

  // DOM-driven interactions that MUST re-render did.
  expect(
    results["emitter-tree-selection"]["CurveEditorPanel"]?.commits ?? 0,
    "selection re-rendered CurveEditorPanel",
  ).toBeGreaterThan(0);
  expect(
    results["emitter-tree-selection"]["EmitterTree"]?.commits ?? 0,
    "selection re-rendered EmitterTree",
  ).toBeGreaterThan(0);

  // AtlasPickerPanel was genuinely measured (grid mounted), so its 0-on-hover row
  // is a real finding, not an absent measurement.
  expect(atlasFramesHovered, "atlas grid mounted (canvas hovered)").toBeGreaterThan(0);

  // Toolbar was exercised (pause toggle and/or selection re-rendered it).
  const toolbarSeen =
    (results["toolbar-pause-toggle"]["Toolbar"]?.commits ?? 0) +
    (results["emitter-tree-selection"]["Toolbar"]?.commits ?? 0);
  expect(toolbarSeen, "Toolbar re-rendered under pause/selection").toBeGreaterThan(0);
});
