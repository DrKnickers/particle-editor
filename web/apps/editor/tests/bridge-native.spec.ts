// Contract tests: drive the *real* native bridge inside
// ParticleEditor.exe --test-host via CDP. These specs exist to
// catch schema drift between the TypeScript MockBridge (covered by
// Vitest) and the C++ BridgeDispatcher — a failure mode
// the plan called out as a risk.
//
// Channel: in --test-host mode, App.tsx swaps `window.bridge` for a
// TestHostBridge that routes requests through WebView2's host-object
// IPC channel (`chrome.webview.hostObjects.hostBridge`) instead of
// `chrome.webview.postMessage`. WebView2 silently drops postMessage
// calls from page → host while a CDP debugger is attached;
// the host-object channel is on a separate
// marshalling path and is unaffected. Events (host → page) still flow
// over postMessage and are wired up by TestHostBridge.on().
import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));

  // The WebView2 navigation is async vs. the host launch; wait until
  // `window.bridge` is attached by App.tsx before any spec runs.
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("CDP connect + window.bridge is attached (smoke)", async () => {
  // Positive smoke: the host's --test-host plumbing came up, WebView2's
  // CDP endpoint is reachable, the React app navigated, App.tsx ran and
  // attached the bridge to window. Catches every regression except the
  // postMessage-from-CDP one that .fixme'd specs cover.
  const probe = await page.evaluate(() => {
    const b = (window as { bridge?: { constructor: { name: string } } }).bridge;
    return {
      hasBridge: typeof b !== "undefined",
      hasRequest: typeof (b as { request?: unknown })?.request === "function",
      hasOn: typeof (b as { on?: unknown })?.on === "function",
      hasWebview: typeof (window as { chrome?: { webview?: unknown } }).chrome?.webview === "object",
    };
  });
  expect(probe.hasBridge).toBe(true);
  expect(probe.hasRequest).toBe(true);
  expect(probe.hasOn).toBe(true);
  expect(probe.hasWebview).toBe(true);
});

test("engine/state/snapshot returns a valid EngineStateDto shape", async () => {
  const dto = (await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "engine/state/snapshot", params: {} });
  })) as Record<string, unknown>;

  expect(dto).toHaveProperty("ground");
  expect(dto).toHaveProperty("groundZ");
  expect(dto).toHaveProperty("groundTexture");
  expect(dto).toHaveProperty("skydomeSlot");
  expect(dto).toHaveProperty("background");
  expect(dto).toHaveProperty("bloom");
  expect(dto).toHaveProperty("bloomAvailable");
  expect(dto).toHaveProperty("lights");
  expect(dto).toHaveProperty("camera");
  expect(typeof dto.groundZ).toBe("number");
  expect(typeof dto.bloomAvailable).toBe("boolean");
});

test("engine/set/ground-z mutates state and fires engine/state/changed", async () => {
  const result = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
      on(kind: string, h: (e: { payload: unknown }) => void): () => void;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");

    return new Promise<{
      before: number;
      after: number;
      event: { groundZ: number } | null;
    }>((resolve, reject) => {
      let event: { groundZ: number } | null = null;
      const off = b.on("engine/state/changed", (e) => {
        event = e.payload as { groundZ: number };
      });
      b.request({ kind: "engine/state/snapshot", params: {} })
        .then(async (before) => {
          const newZ = 17.5;
          await b.request({ kind: "engine/set/ground-z", params: { z: newZ } });
          await new Promise((r) => setTimeout(r, 50));
          const after = (await b.request({
            kind: "engine/state/snapshot",
            params: {},
          })) as { groundZ: number };
          off();
          resolve({
            before: (before as { groundZ: number }).groundZ,
            after: after.groundZ,
            event,
          });
        })
        .catch(reject);
    });
  });

  expect(result.after).toBeCloseTo(17.5, 5);
  expect(result.event).not.toBeNull();
  expect(result.event!.groundZ).toBeCloseTo(17.5, 5);
});

test("setter burst: coalesced state events still deliver the final value (B1)", async () => {
  // Perf-plan B1 contract: EmitEngineStateChanged live-coalesces bursts
  // (leading edge immediate, follow-ups pending, trailing flush from the
  // idle branch / next dispatch). Invariants a regression would break:
  //   1. every response in a rapid setter burst still resolves;
  //   2. the trailing engine/state/changed is NEVER lost — an event
  //      carrying the FINAL value arrives even with no follow-up request
  //      (the idle-branch flush), so the web can't be left stale;
  //   3. at least one event fired (leading edge intact).
  const result = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
      on(kind: string, h: (e: { payload: unknown }) => void): () => void;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");

    const seen: number[] = [];
    const off = b.on("engine/state/changed", (e) => {
      seen.push((e.payload as { groundZ: number }).groundZ);
    });
    const N = 20;
    const finalZ = 42.5;
    for (let i = 1; i <= N; i++) {
      const z = i === N ? finalZ : i * 0.5;
      await b.request({ kind: "engine/set/ground-z", params: { z } });
    }
    // No further requests: the trailing emit must arrive via the host's
    // idle-branch flush (≤ one display frame) — grace-wait well past it.
    await new Promise((r) => setTimeout(r, 250));
    off();
    return { events: seen.length, sawFinal: seen.includes(finalZ) };
  });

  // Restore the baseline groundZ BEFORE asserting, so a failed assertion
  // can't leak mutated state into later specs on the shared page.
  await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: object): Promise<unknown> } }).bridge!;
    await b.request({ kind: "engine/set/ground-z", params: { z: 0 } });
  });

  expect(result.events).toBeGreaterThanOrEqual(1); // leading edge intact
  expect(result.events).toBeLessThanOrEqual(21);   // sanity: ≤ N + trailing
  expect(result.sawFinal).toBe(true);              // trailing emit never lost
});

test("engine/set/background round-trips a COLORREF", async () => {
  const result = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    // Capture and RESTORE. Native specs share one host process and one page, so
    // a test that mutates engine state and walks away leaves every later test
    // running against it — this one left the background gray for the rest of
    // the run, and nothing downstream asserted the default, so a regression in
    // background restoration would have gone unnoticed (2026-07 audit, an-audit-finding).
    const before = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { background: number };

    const rgb = 0x00808080;
    await b.request({ kind: "engine/set/background", params: { rgb } });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { background: number };

    await b.request({ kind: "engine/set/background", params: { rgb: before.background } });
    const after = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { background: number };

    return { roundTripped: snap.background, restored: after.background, original: before.background };
  });
  expect(result.roundTripped).toBe(0x00808080);
  // The restore has to actually work, or this test just moved the leak.
  expect(result.restored).toBe(result.original);
});

test("engine/query/ground-slot-empty returns boolean", async () => {
  const r = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    return b.request({ kind: "engine/query/ground-slot-empty", params: { slot: 0 } });
  });
  expect(typeof r).toBe("boolean");
});
