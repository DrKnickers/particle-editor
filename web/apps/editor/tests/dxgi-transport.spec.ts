// DXGI transport log-evidence gate.
//
// The headline ship gate: the DXGI swapchain + DComp engine
// visual + per-frame CopyResource + Present1 pipeline is alive and
// stable in steady state. Asserts:
//   1. `[COMP-engine-attach]` log line present (AttachEngineVisual
//      succeeded — see Compositor::AttachEngineVisual).
//   2. `[COMP-engine-frame]` 1 Hz throttled count GROWS between two
//      samples taken ~2 seconds apart (proves CompositeEngineFrame is
//      actively running, not stalled).
//   3. `[COMP-engine-handle-hash]` shows stable resource identity
//      (sharedTex + backBuffer COM-object addresses don't drift mid-
//      run — the spike's dxgi_spike.cpp:355-357 documented "wrong
//      handle silently returns different texture" failure mode would
//      surface as a sharedTex pointer change here).
//   4. No `[COMP-engine-fail]` lines (any failure path in
//      Compositor.cpp emits this prefix).
//
// IMPORTANT CAVEAT — what this spec does NOT test.
//
// Playwright's `page.screenshot()` captures the renderer DOM via CDP.
// It does NOT capture what DComp composites UNDER the WebView2
// visual. Engine pixels live in the DXGI swapchain → DComp engine
// visual, behind the WebView2 visual. A DOM screenshot under
// composition mode shows only the React DOM (empty `<img>`,
// transparent `<canvas>`) — not the DXGI pixels. So this spec
// CANNOT visually assert "engine pixels are correct"; that's
// irreducible to manual smoke or
// host-side AlphaCompositor::CaptureSnapshotPng bridge inspection.
//
// What this spec DOES catch:
//   - Regression in AttachEngineVisual wiring (no [COMP-engine-attach]
//     → entire engine-visual path broken)
//   - Regression in CompositeEngineFrame (count stops growing →
//     per-frame composite stalled)
//   - Regression in RefreshEngineSharedHandle (handle-hash unstable
//     without a preceding [COMP-engine-resize] → silent texture swap)
//   - Any failure path firing (D3D11 device creation, OpenSharedResource,
//     swapchain create, Present1 — all emit [COMP-engine-fail])
// The native suite always exercises the composition transport.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

// Host.log path — written by the host's Log() macro, see HostWindow.cpp.
// Path mirrors what the [host] WebView2 user-data folder line points at.
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

// Count occurrences of a substring in a haystack — no regex
// state-machine surprises with multi-line text.
function countOccurrences(haystack: string, needle: string): number {
  let n = 0;
  let i = haystack.indexOf(needle);
  while (i !== -1) {
    n += 1;
    i = haystack.indexOf(needle, i + needle.length);
  }
  return n;
}

test("[COMP-engine-attach] is present in host.log (AttachEngineVisual succeeded)", () => {
  const log = readHostLog();
  expect(log).toContain("[COMP-engine-attach] engine visual attached");
});

test("[COMP-engine-init] confirms D3D11 device creation with feature level + flags", () => {
  const log = readHostLog();
  // Format from Compositor.cpp:
  //   [COMP-engine-init] D3D11 device created (level=0xB100 flags=0x22)
  expect(log).toMatch(/\[COMP-engine-init\] D3D11 device created \(level=0x[0-9A-Fa-f]+ flags=0x[0-9A-Fa-f]+\)/);
});

test("[COMP-engine-luid] log line records both adapter LUIDs (multi-GPU guard armed)", () => {
  const log = readHostLog();
  // Format:
  //   [COMP-engine-luid] D3D11 adapter LUID=00000000-0001067C (engine LUID=00000000-0001067C)
  expect(log).toMatch(/\[COMP-engine-luid\] D3D11 adapter LUID=[0-9A-Fa-f]+-[0-9A-Fa-f]+ \(engine LUID=[0-9A-Fa-f]+-[0-9A-Fa-f]+\)/);
});

test("[COMP-engine-frame] count grows between samples ~2s apart (CompositeEngineFrame is running)", async () => {
  // Sample 1: extract the current cumulative composite count.
  const log1 = readHostLog();
  const match1 = [...log1.matchAll(/\[COMP-engine-frame\] composite n=(\d+)/g)];
  const count1 = match1.length === 0 ? -1 : parseInt(match1[match1.length - 1][1], 10);

  // Wait ~2.2s — enough for at least 2 throttle ticks past the first
  // (1 Hz throttle means up to 1s between log lines + 1s for the
  // next emission, then a 200ms safety margin).
  await new Promise((r) => setTimeout(r, 2200));

  // Sample 2.
  const log2 = readHostLog();
  const match2 = [...log2.matchAll(/\[COMP-engine-frame\] composite n=(\d+)/g)];
  expect(match2.length).toBeGreaterThan(0);
  const count2 = parseInt(match2[match2.length - 1][1], 10);

  // The count2 - count1 delta should be > 50 frames (typical engine
  // FPS ≥ 30, so 2 seconds is ≥ 60 frames). Gate at > 30 to allow
  // slow CI hosts.
  expect(count2 - count1).toBeGreaterThan(30);
});

test("[COMP-engine-handle-hash] resource identity is stable across the smoke window", () => {
  // Extract all (sharedTex, backBuffer) tuples logged during the run.
  // The 1 Hz throttle means there are several samples accumulated by
  // the time the previous tests have run.
  const log = readHostLog();
  const matches = [...log.matchAll(/\[COMP-engine-handle-hash\] handle=([0-9A-Fa-fx]+) sharedTex=([0-9A-Fa-fx]+) backBuffer=([0-9A-Fa-fx]+) texSize=(\d+x\d+)/g)];
  expect(matches.length).toBeGreaterThan(1);  // need at least 2 samples to compare

  // Group consecutive samples by handle. Within a span of the same
  // handle, sharedTex and backBuffer should be identical (re-open
  // would have logged [COMP-engine-resize] and changed the handle).
  const handleSpans = new Map<string, Set<string>>();
  for (const m of matches) {
    const handle = m[1];
    const sharedTex = m[2];
    const backBuffer = m[3];
    const key = `${sharedTex}|${backBuffer}`;
    if (!handleSpans.has(handle)) handleSpans.set(handle, new Set());
    handleSpans.get(handle)!.add(key);
  }

  // Each handle should have exactly ONE (sharedTex, backBuffer) tuple
  // associated with it. Multiple tuples for the same handle would
  // mean COM-object identity drifted without a handle change — the
  // spike's wrong-handle failure mode at dxgi_spike.cpp:355-357.
  for (const [handle, tupleSet] of handleSpans) {
    expect(
      tupleSet.size,
      `Handle ${handle} had ${tupleSet.size} distinct (sharedTex, backBuffer) tuples — expected exactly 1`,
    ).toBe(1);
  }
});

test("no [COMP-engine-fail] lines anywhere in the host log", () => {
  const log = readHostLog();
  const fails = countOccurrences(log, "[COMP-engine-fail]");
  if (fails > 0) {
    // Surface the actual failure messages in the assert failure for
    // easier diagnosis. Slice to a reasonable length to avoid noise.
    const failLines = log
      .split("\n")
      .filter((l) => l.includes("[COMP-engine-fail]"));
    throw new Error(
      `Found ${fails} [COMP-engine-fail] line(s) in host.log:\n  ${failLines.join("\n  ")}`,
    );
  }
  expect(fails).toBe(0);
});

test("[COMP-engine-open] confirms shared resource opened with correct format", () => {
  // Format from Compositor.cpp:
  //   [COMP-engine-open] OpenSharedResource handle=00000000400022C2 texSize=1264x761 fmt=87 bind=0x28 share=0x2
  const log = readHostLog();
  const match = log.match(/\[COMP-engine-open\] OpenSharedResource handle=([0-9A-Fa-fx]+) texSize=(\d+)x(\d+) fmt=(\d+) bind=0x([0-9A-Fa-f]+) share=0x([0-9A-Fa-f]+)/);
  expect(match).not.toBeNull();
  if (match) {
    const [, , w, h, fmt, bind, share] = match;
    // DXGI_FORMAT_B8G8R8A8_UNORM = 87 (matches engine's D3DFMT_A8R8G8B8)
    expect(Number(fmt)).toBe(87);
    // D3D11_BIND_RENDER_TARGET (0x20) | D3D11_BIND_SHADER_RESOURCE (0x8) = 0x28
    expect(parseInt(bind, 16)).toBe(0x28);
    // D3D11_RESOURCE_MISC_SHARED = 0x2
    expect(parseInt(share, 16)).toBe(0x2);
    // Texture size must be positive (popup-client size at attach time)
    expect(Number(w)).toBeGreaterThan(0);
    expect(Number(h)).toBeGreaterThan(0);
  }
});

test("[COMP-engine-swap] confirms composition swapchain creation succeeded", () => {
  const log = readHostLog();
  expect(log).toMatch(/\[COMP-engine-swap\] composition swapchain created \d+x\d+ FLIP_SEQ BGRA8/);
});
