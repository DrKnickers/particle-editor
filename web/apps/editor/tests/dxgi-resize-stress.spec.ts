// DXGI resize-stress gate.
//
// Drives `layout/viewport-rect` through 50 size cycles. Each cycle
// invalidates AlphaCompositor's shared HANDLE (releases the old D3D9
// texture + creates a new one with a new handle). Compositor's lazy
// handle-check in CompositeEngineFrame should pick up the new handle
// on the next frame, drop the D3D11 alias, OpenSharedResource on the
// new handle, ResizeBuffers on the swapchain if size changed, and
// resume.
//
// Asserts:
//   1. Host doesn't crash (bridge stays responsive across all 50 cycles).
//   2. `[COMP-engine-resize]` lines accumulate in host.log (proves lazy
//      detection fired — without the lazy handle-check this would be zero).
//   3. No `[COMP-engine-fail]` lines added during the stress run.
//   4. CompositeEngineFrame keeps running through the stress (composite
//      count continues to grow).
//
// Skip behaviour: gates on ALO_HOSTING_MODE != legacy (default), same
// pattern as composition-hosting.spec.ts.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const COMPOSITION_MODE = process.env.ALO_HOSTING_MODE !== "legacy";
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
        "ALO_HOSTING_MODE == 'legacy' (composition mode inactive) — DXGI resize stress not " +
        "applicable to HWND-mode runs.",
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

function countOccurrences(haystack: string, needle: string): number {
  let n = 0;
  let i = haystack.indexOf(needle);
  while (i !== -1) {
    n += 1;
    i = haystack.indexOf(needle, i + needle.length);
  }
  return n;
}

test("50 layout/viewport-rect cycles keep host responsive + Compositor recovers each time", async () => {
  test.setTimeout(60_000);  // 50 cycles × ~500ms = ~25s plus margins

  const before = readHostLog();
  const failsBefore = countOccurrences(before, "[COMP-engine-fail]");
  const resizesBefore = countOccurrences(before, "[COMP-engine-resize]");
  // Last composite count BEFORE the stress.
  const beforeMatches = [...before.matchAll(/\[COMP-engine-frame\] composite n=(\d+)/g)];
  const compositeBefore = beforeMatches.length === 0
    ? 0
    : parseInt(beforeMatches[beforeMatches.length - 1][1], 10);

  // Drive resizes through a spread of sizes designed to actually
  // change the popup-client dimensions each cycle (no two consecutive
  // resizes at the same size, since AlphaCompositor::Resize is a no-op
  // when width+height are unchanged). Sizes mix small/medium/large +
  // different aspect ratios to stress ResizeBuffers and CopyResource
  // size-matching.
  const sizes = [
    { x: 0, y: 0, w: 640,  h: 480  },
    { x: 0, y: 0, w: 1280, h: 720  },
    { x: 0, y: 0, w: 1920, h: 1080 },
    { x: 0, y: 0, w: 2560, h: 1440 },
    { x: 0, y: 0, w: 800,  h: 600  },
    { x: 0, y: 0, w: 1024, h: 768  },
    { x: 0, y: 0, w: 1366, h: 768  },
    { x: 0, y: 0, w: 1600, h: 900  },
    { x: 0, y: 0, w: 1440, h: 900  },
    { x: 0, y: 0, w: 1920, h: 1200 },
  ];

  const results: Array<{ snapshot: boolean; size: typeof sizes[0]; cycle: number; error?: string }> = await page.evaluate(async (sizes) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const r: Array<{ snapshot: boolean; size: typeof sizes[0]; cycle: number; error?: string }> = [];
    for (let i = 0; i < 50; i++) {
      const s = sizes[i % sizes.length];
      try {
        await b.request({ kind: "layout/viewport-rect", params: s });
        // Sleep a touch so the host's per-frame loop has a chance to
        // detect the handle change and re-open. Without this, the
        // bridge snapshot below might race the lazy re-open.
        await new Promise((res) => setTimeout(res, 50));
        const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
        r.push({ snapshot: snap != null, size: s, cycle: i });
      } catch (err) {
        r.push({ snapshot: false, size: s, cycle: i, error: (err as Error).message });
      }
    }
    return r;
  }, sizes);

  // Assertion 1: bridge stayed responsive through all 50 cycles.
  expect(results).toHaveLength(50);
  const failedSnapshots = results.filter((r) => !r.snapshot);
  if (failedSnapshots.length > 0) {
    throw new Error(
      `${failedSnapshots.length} of 50 resize cycles had bridge failures:\n` +
      failedSnapshots
        .slice(0, 5)
        .map((r) => `  cycle ${r.cycle} (${JSON.stringify(r.size)}): ${r.error ?? "snapshot returned null"}`)
        .join("\n"),
    );
  }

  // Brief tail to let the last [COMP-engine-frame] flush after the
  // final resize cycle's settle.
  await new Promise((r) => setTimeout(r, 1500));

  const after = readHostLog();
  const failsAfter = countOccurrences(after, "[COMP-engine-fail]");
  const resizesAfter = countOccurrences(after, "[COMP-engine-resize]");
  const afterMatches = [...after.matchAll(/\[COMP-engine-frame\] composite n=(\d+)/g)];
  const compositeAfter = afterMatches.length === 0
    ? 0
    : parseInt(afterMatches[afterMatches.length - 1][1], 10);

  // Assertion 2: lazy detection actually fired. The 50 cycles cycled
  // through ≥10 distinct sizes, so at MINIMUM 10 handle changes were
  // observed. (Same size in consecutive cycles is a no-op in
  // AlphaCompositor::Resize but DIFFERENT sizes recreate the texture.)
  // Conservative gate: at least 5 resize events recorded.
  const newResizes = resizesAfter - resizesBefore;
  expect(
    newResizes,
    `Only ${newResizes} [COMP-engine-resize] events recorded after 50 size cycles ` +
    `(spread across ${sizes.length} distinct sizes). Expected ≥ 5 — lazy detection in ` +
    `CompositeEngineFrame may have regressed.`,
  ).toBeGreaterThanOrEqual(5);

  // Assertion 3: no NEW [COMP-engine-fail] lines were added during
  // the stress run.
  const newFails = failsAfter - failsBefore;
  if (newFails > 0) {
    const failLines = after
      .split("\n")
      .filter((l) => l.includes("[COMP-engine-fail]"))
      .slice(-newFails);  // most-recent newFails entries
    throw new Error(
      `${newFails} new [COMP-engine-fail] lines added during stress:\n  ${failLines.join("\n  ")}`,
    );
  }

  // Assertion 4: composite count grew through the stress (per-frame
  // loop kept running even while AlphaCompositor::Resize was firing
  // 10+ times). Tight gate of 10 frames is conservative — typical
  // engine should produce 50+ in this window.
  expect(
    compositeAfter - compositeBefore,
    `Composite count only grew by ${compositeAfter - compositeBefore} during the stress ` +
    `(before=${compositeBefore}, after=${compositeAfter}). Expected ≥ 10 — ` +
    `CompositeEngineFrame may have stalled.`,
  ).toBeGreaterThanOrEqual(10);
});

test("layout/viewport-rect to known size produces an [COMP-engine-resize] line with new dimensions", async () => {
  // Targeted regression check for the lazy detection path itself.
  // Pick a size unlikely to match a prior resize so the [COMP-engine-resize]
  // line includes our specific WxH at the end.
  const targetW = 1700;
  const targetH = 950;

  await page.evaluate(async ({ w, h }) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "layout/viewport-rect", params: { x: 0, y: 0, w, h } });
    // Let the host's per-frame loop tick at least once so the lazy
    // detection runs.
    await new Promise((res) => setTimeout(res, 200));
  }, { w: targetW, h: targetH });

  const log = readHostLog();
  const matches = [...log.matchAll(/\[COMP-engine-resize\] handle [^\s]+ -> [^\s]+, size (\d+)x(\d+) -> (\d+)x(\d+)/g)];
  // We don't necessarily expect ONLY our targeted resize to match (the
  // 50-cycle test above runs first + might have produced lines with
  // the same size if the user reruns). Filter to lines whose NEW size
  // matches our target.
  const ourResize = matches.find((m) => Number(m[3]) === targetW && Number(m[4]) === targetH);
  expect(
    ourResize,
    `Expected at least one [COMP-engine-resize] line with new size ${targetW}x${targetH} ` +
    `after layout/viewport-rect resize. Matches found: ${matches.length}`,
  ).toBeDefined();
});
