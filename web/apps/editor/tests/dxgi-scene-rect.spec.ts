// Phase 3 Stage 5 T7 — DXGI scene-rect transform gate.
//
// Stage 5 wires LayoutBroker.SetSceneRect (driven by React's
// `layout/scene-rect` bridge dispatch) into:
//   1. Compositor::SetEngineVisualTransform — queues a DComp clip
//      update; applied at the end of the next CompositeEngineFrame
//      (sub-plan T6 deferred-clip mechanism) so swapchain pixels +
//      DComp clip arrive on the same DWM cycle.
//   2. Engine::SetSceneViewport — updates m_sceneViewport state +
//      recomputes m_projection at per-pixel-FoV (45° × sceneH /
//      RT_H) + pushes to device + recomputes m_viewProjection.
//
// This spec asserts that the bridge → Compositor seam fires on
// scene-rect dispatch, emitting `[COMP-engine-transform] clip=(L,T,
// R,B) (absolute host-client)` log lines that follow the expected
// rectangle coords.
//
// What this spec does NOT assert:
//   - Engine::SetSceneViewport itself emits via OutputDebugString +
//     printf, NOT host.log. run-native-tests.mjs silences child
//     stdout via stdio:"ignore", so the engine-side log line isn't
//     reachable from here. The [COMP-engine-transform] line is
//     sufficient evidence that LayoutBroker dispatched into the
//     composition-mode path — Compositor + Engine calls are gated
//     on the same m_dcompCompositor presence check (LayoutBroker R9
//     mitigation c).
//   - Visual correctness of the rendered output — Playwright cannot
//     screenshot DComp content (CDP captures DOM only). Manual smoke
//     at T6 is the irreducible visual gate; this spec is the
//     log-evidence regression gate for the wiring path.
//
// Skip behaviour: each test no-ops with a clear annotation when
// ALO_HOSTING_MODE == "legacy" (composition mode inactive). HWND-mode baseline runs
// silently skip.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const COMPOSITION_MODE = process.env.ALO_HOSTING_MODE !== "legacy" /* */;
const HOST_LOG_PATH = process.env.LOCALAPPDATA
  ? join(process.env.LOCALAPPDATA, "AloParticleEditor", "host.log")
  : "";

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

test.beforeEach(({}, testInfo) => {
  if (!COMPOSITION_MODE) {
    testInfo.annotations.push({
      type: "skip-reason",
      description:
        "ALO_HOSTING_MODE == 'legacy' (composition mode inactive) — scene-rect transform " +
        "gate is composition-mode-only (LayoutBroker.SetSceneRect's " +
        "new wiring is gated on m_dcompCompositor presence per R9 " +
        "mitigation c).",
    });
    test.skip();
  }
  if (!HOST_LOG_PATH) {
    testInfo.annotations.push({
      type: "skip-reason",
      description: "LOCALAPPDATA env var not set — can't locate host.log.",
    });
    test.skip();
  }
});

function readHostLog(): string {
  return readFileSync(HOST_LOG_PATH, "utf8");
}

// Parse all [COMP-engine-transform] lines into structured tuples.
// Format (post-T6): `[COMP-engine-transform] clip=(L,T,R,B) (absolute host-client)`
function extractTransforms(log: string): Array<{ l: number; t: number; r: number; b: number }> {
  return [...log.matchAll(/\[COMP-engine-transform\] clip=\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)/g)].map(
    (m) => ({
      l: parseInt(m[1], 10),
      t: parseInt(m[2], 10),
      r: parseInt(m[3], 10),
      b: parseInt(m[4], 10),
    }),
  );
}

test("[COMP-engine-transform] boot seed fired with non-degenerate clip", () => {
  // The HostWindow attach-time seed (after AttachEngineVisual succeeds)
  // calls SetEngineVisualTransform with immediate=true. Either:
  //   - With a cached scene-rect from React if dispatched pre-attach
  //     (uncommon — React typically mounts after the DComp tree comes
  //     up), OR
  //   - With (0, 0, clientW, clientH) seed if no cache (the common case).
  // Either way, at least one [COMP-engine-transform] line exists in
  // the log with positive width + height (right > left, bottom > top).
  const log = readHostLog();
  const transforms = extractTransforms(log);
  expect(transforms.length).toBeGreaterThan(0);

  const first = transforms[0];
  expect(first.r).toBeGreaterThan(first.l);
  expect(first.b).toBeGreaterThan(first.t);
});

test("layout/scene-rect dispatch produces a matching [COMP-engine-transform] line", async () => {
  const targetX = 100;
  const targetY = 50;
  const targetW = 800;
  const targetH = 600;
  const expectedL = targetX;
  const expectedT = targetY;
  const expectedR = targetX + targetW;
  const expectedB = targetY + targetH;

  await page.evaluate(async ({ x, y, w, h }) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "layout/scene-rect", params: { x, y, w, h } });
  }, { x: targetX, y: targetY, w: targetW, h: targetH });

  // The deferred-clip mechanism (Compositor.cpp Stage 5 T6) queues
  // the transform; it applies in the NEXT CompositeEngineFrame after
  // the dispatch. Engine renders at ~70-100 fps, so the transform
  // applies within ~10-15ms. Wait 250ms for slack (some CI hosts
  // may run slower; the dxgi-resize-stress spec uses 50ms after each
  // dispatch + 1500ms tail — we just need the one event flushed).
  await new Promise((r) => setTimeout(r, 250));

  const log = readHostLog();
  const transforms = extractTransforms(log);
  const found = transforms.find(
    (t) => t.l === expectedL && t.t === expectedT && t.r === expectedR && t.b === expectedB,
  );
  expect(
    found,
    `Expected [COMP-engine-transform] with clip=(${expectedL},${expectedT},${expectedR},${expectedB}) ` +
    `after layout/scene-rect dispatch (x=${targetX} y=${targetY} w=${targetW} h=${targetH}). ` +
    `Recent transforms: ${JSON.stringify(transforms.slice(-5))}`,
  ).toBeDefined();
});

test("three sequential scene-rect dispatches produce three transform lines in order", async () => {
  // Three deliberately-distinct dispatches with non-overlapping clip
  // tuples so identical-args idempotence in SetEngineVisualTransform
  // doesn't suppress any of them.
  const dispatches = [
    { x: 200, y: 80,  w: 1000, h: 700 },   // clip = (200, 80, 1200, 780)
    { x: 250, y: 100, w: 1100, h: 720 },   // clip = (250, 100, 1350, 820)
    { x: 220, y: 90,  w: 1050, h: 710 },   // clip = (220, 90, 1270, 800)
  ];

  const transformsBefore = extractTransforms(readHostLog()).length;

  for (const d of dispatches) {
    await page.evaluate(async (rect) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge;
      await b.request({ kind: "layout/scene-rect", params: rect });
      // Tiny per-dispatch settle so each transform gets queued + applied
      // before the next dispatch overwrites the pending queue.
      await new Promise((res) => setTimeout(res, 80));
    }, d);
  }

  // Settle for the final CompositeEngineFrame to flush.
  await new Promise((r) => setTimeout(r, 300));

  const log = readHostLog();
  const transforms = extractTransforms(log);
  const newTransforms = transforms.slice(transformsBefore);

  // Each of the three expected (L, T, R, B) tuples must appear at
  // least once in the new transforms.
  for (const d of dispatches) {
    const expectedL = d.x;
    const expectedT = d.y;
    const expectedR = d.x + d.w;
    const expectedB = d.y + d.h;
    const found = newTransforms.find(
      (t) => t.l === expectedL && t.t === expectedT && t.r === expectedR && t.b === expectedB,
    );
    expect(
      found,
      `Expected transform clip=(${expectedL},${expectedT},${expectedR},${expectedB}) ` +
      `for dispatch ${JSON.stringify(d)}. Got transforms: ${JSON.stringify(newTransforms)}`,
    ).toBeDefined();
  }

  // Ordering check — the transforms should appear in the same order
  // as the dispatches (the deferred-clip mechanism preserves order
  // because each CompositeEngineFrame applies the LATEST pending
  // transform; the per-dispatch settle ensures one composite cycle
  // per dispatch).
  const expectedTuples = dispatches.map((d) => `${d.x},${d.y},${d.x + d.w},${d.y + d.h}`);
  const newTuples = newTransforms.map((t) => `${t.l},${t.t},${t.r},${t.b}`);
  // Find each expected tuple's first index in newTuples; verify indices
  // are strictly increasing.
  const indices = expectedTuples.map((tup) => newTuples.indexOf(tup));
  for (let i = 1; i < indices.length; i++) {
    expect(
      indices[i],
      `Transform ordering broken: ${expectedTuples[i]} appeared at index ${indices[i]}, ` +
      `expected AFTER ${expectedTuples[i - 1]} at index ${indices[i - 1]}. ` +
      `New transforms: ${JSON.stringify(newTuples)}`,
    ).toBeGreaterThan(indices[i - 1]);
  }
});

test("no [COMP-engine-fail] lines emitted by Stage 5 scene-rect path", () => {
  // Stage 5's SetEngineVisualTransform has multiple failure paths
  // (SetOffsetX, SetOffsetY, SetClip, Commit). If any fired during
  // the prior tests' dispatches, [COMP-engine-fail] would be present.
  const log = readHostLog();
  const failLines = log
    .split("\n")
    .filter((l) => l.includes("[COMP-engine-fail]") && l.includes("SetEngineVisualTransform"));
  // Note: ApplyTransform fires from Impl::ApplyTransform — its failure
  // messages include "ApplyTransform" prefix, NOT "SetEngineVisualTransform".
  // Catch both.
  const applyFailLines = log
    .split("\n")
    .filter((l) => l.includes("[COMP-engine-fail]") && l.includes("ApplyTransform"));
  const totalFails = failLines.length + applyFailLines.length;
  if (totalFails > 0) {
    throw new Error(
      `Found ${totalFails} Stage 5 transform-related [COMP-engine-fail] line(s):\n  ` +
      [...failLines, ...applyFailLines].join("\n  "),
    );
  }
  expect(totalFails).toBe(0);
});
