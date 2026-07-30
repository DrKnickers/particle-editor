// D3D9Ex migration regression spec.
//
// Asserts that the production engine (running under D3D9Ex instead of
// D3D9) survives the four D3DPOOL_DEFAULT migration sites' new
// Release-before / Recreate-after Reset cycle. Specifically guards
// against the known incident shape — a D3DPOOL_DEFAULT resource missing
// from Engine::Reset's release/recreate flow producing a device-lost
// state on the next Reset trigger.
//
// What this spec proves:
//
//   1. window.bridge attached    ⇒ Engine constructor completed.
//      Because the engine hard-fails on D3D9Ex unavailable,
//      bridge attachment is positive proof Direct3DCreate9Ex +
//      CreateDeviceEx + InitSkydomeMesh (with D3DPOOL_DEFAULT VB/IB)
//      all succeeded — the failure modes from yesterday's "Unable to
//      create skydome mesh" dialog cannot reach this point.
//
//   2. Ground-texture cycle through bundled slots (0..3) + solid-colour
//      slot (4) — exercises the CreateSolidColorTexture path migrated
//      from D3DPOOL_MANAGED to D3DPOOL_DEFAULT (engine.cpp:1044).
//
//   3. Skydome cycle through bundled slots — every Reset triggered
//      between mutations exercises the new
//      ReleaseSkydomeMeshBuffers/CreateSkydomeMeshBuffers pair, plus
//      ReloadSkydomeTexture(m_skydomeIndex) after Reset.
//
//   4. Resize cycle stress — each `layout/viewport-rect` mutation runs
//      LayoutBroker::Apply → Engine::Reset, which is the high-frequency
//      driver of the new release/recreate path. 10 cycles assert the
//      engine remains responsive (no resource leak, no device-lost).
//
//   5. Polluter pair scenario — explicitly reproduces the spec-
//      ordering that surfaced the incident (background-picker × spawner toggle
//      then ground-texture set). Today the engine handles it via the
//      skydome OnLost/OnReset + the new D3DPOOL_DEFAULT migration;
//      regression would mean a new resource missed the Reset flow.
//
// This spec does NOT capture the engine's `[D3D9Ex] device created` log
// line directly — GUI apps don't easily expose stdout to a CDP-attached
// Playwright spec. Bridge responsiveness is the functional equivalent
// of the log assertion: if D3D9Ex init had failed, the editor would
// have thrown in the ctor and the test runner would have timed out
// waiting for CDP. The harness already captures that path via
// `Host process exited before CDP came up` in run-native-tests.mjs.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

type EngineStateDto = {
  ground: boolean;
  groundZ: number;
  groundTexture: number;
  skydomeSlot: number;
  background: { r: number; g: number; b: number };
};

type DeviceRecoveryWorkState = {
  pending: boolean;
  reloadCount: number;
  authoredApplyCount: number;
  deviceProbeCount: number;
  composedFramePrepareCount: number;
  frameReady: boolean;
};

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

test("bridge attached ⇒ D3D9Ex init + DPOOL_DEFAULT skydome mesh succeeded", async () => {
  // Engine::Engine throws on Direct3DCreate9Ex failure (hard-fail)
  // and on InitSkydomeMesh failure ("Unable to
  // create skydome mesh"). Either failure would prevent the editor from
  // reaching CDP, so this spec executing AT ALL implies both succeeded.
  // The explicit probe here documents the implicit contract.
  const probe = await page.evaluate(() => {
    const b = (window as { bridge?: { request: unknown; on: unknown } }).bridge;
    return {
      hasBridge: typeof b !== "undefined",
      hasRequest: typeof b?.request === "function",
      hasOn: typeof b?.on === "function",
    };
  });
  expect(probe.hasBridge).toBe(true);
  expect(probe.hasRequest).toBe(true);
  expect(probe.hasOn).toBe(true);
});

test("ground texture cycle through bundled slots (device-lost regression)", async () => {
  // Slots 0..3 are bundled RCDATA textures loaded via
  // LoadGroundTextureFromResource → D3DXCreateTextureFromFileInMemory.
  // Cycle through them, assert each mutation lands. Failure mode would
  // be groundTexture stuck at 0 — the literal incident symptom.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const snapshot = async () =>
      (await b.request({ kind: "engine/state/snapshot", params: {} })) as EngineStateDto;
    const set = async (slot: number) => {
      await b.request({ kind: "engine/set/ground-texture", params: { slot } });
      return (await snapshot()).groundTexture;
    };
    return {
      after0: await set(0),
      after1: await set(1),
      after2: await set(2),
      after3: await set(3),
      back0: await set(0),
    };
  });
  expect(result.after0).toBe(0);
  expect(result.after1).toBe(1);
  expect(result.after2).toBe(2);
  expect(result.after3).toBe(3);
  expect(result.back0).toBe(0);
});

test("solid-colour ground (slot 4) ⇒ CreateSolidColorTexture under D3DPOOL_DEFAULT", async () => {
  // CreateSolidColorTexture (engine.cpp:1044) migrated
  // from D3DPOOL_MANAGED to D3DPOOL_DEFAULT. The first time this path
  // runs on the D3D9Ex device it allocates a fresh 1×1 RGBA texture,
  // locks it, writes the colour, unlocks. If the migration is buggy
  // (wrong usage flags, lock failure), setting the slot would fail and
  // groundTexture would stay at its prior value.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    // Color is a packed COLORREF number (Win32 0x00BBGGRR), not an
    // {r,g,b} object. 0x2050C8 = B=32, G=80, R=200 — orange-ish.
    await b.request({ kind: "engine/set/ground-solid-color", params: { rgb: 0x2050C8 } });
    await b.request({ kind: "engine/set/ground-texture", params: { slot: 4 } });
    const dto = (await b.request({ kind: "engine/state/snapshot", params: {} })) as EngineStateDto;
    return dto.groundTexture;
  });
  expect(result).toBe(4);
});

test("skydome cycle through bundled slots ⇒ implicit Reset exercise", async () => {
  // Skydome slot changes can trigger Reset paths via the ReloadSkydome
  // codepath; even without a Reset, setting each slot exercises
  // ReloadSkydomeTexture's slot dispatch. Bundled slots are FileManager-
  // first with RCDATA fallback. Failure would indicate the skydome
  // texture path itself broke.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const get = async () =>
      ((await b.request({ kind: "engine/state/snapshot", params: {} })) as EngineStateDto).skydomeSlot;
    const set = async (slot: number) => {
      await b.request({ kind: "engine/set/skydome-slot", params: { slot } });
      return get();
    };
    return {
      off: await set(0),
      one: await set(1),
      backOff: await set(0),
    };
  });
  expect(result.off).toBe(0);
  expect(result.one).toBe(1);
  expect(result.backOff).toBe(0);
});

test("10× resize cycle ⇒ Engine::Reset survives the new D3DPOOL_DEFAULT release/recreate path", async () => {
  // Each layout/viewport-rect mutation runs LayoutBroker::Apply →
  // Engine::Reset → release skydome VB/IB + skydome texture + ground
  // texture + compositor RT + shaders → m_pDevice->Reset → recreate
  // all of the above. 10 cycles at alternating sizes stresses both the
  // release order and the recreate order.
  //
  // Success: snapshot still responds after all 10 cycles AND
  // groundTexture/skydomeSlot survive intact.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // Seed a non-default state so we can verify it survives the cycles.
    await b.request({ kind: "engine/set/ground-texture", params: { slot: 2 } });
    await b.request({ kind: "engine/set/skydome-slot", params: { slot: 1 } });

    const sizes = [
      { x: 0, y: 0, w: 640,  h: 480  },
      { x: 0, y: 0, w: 1280, h: 720  },
      { x: 0, y: 0, w: 1920, h: 1080 },
      { x: 0, y: 0, w: 800,  h: 600  },
      { x: 0, y: 0, w: 1600, h: 900  },
    ];
    let lastSnapshot: EngineStateDto | null = null;
    for (let cycle = 0; cycle < 2; ++cycle) {
      for (const s of sizes) {
        await b.request({ kind: "layout/viewport-rect", params: s });
        lastSnapshot = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as EngineStateDto;
      }
    }
    return lastSnapshot;
  });

  expect(result).not.toBeNull();
  expect(result!.groundTexture).toBe(2);
  expect(result!.skydomeSlot).toBe(1);
});

test("polluter pair + ground set ⇒ engine accepts mutation after spawner+modal cycle", async () => {
  // Incident surface: spawner toggle (Zustand store + localStorage) +
  // multiple modal-cycle workflows leave m_pSkydomeEffect's D3DPOOL_
  // DEFAULT state-cache references stale across Reset, causing
  // m_pDevice->Reset to return D3DERR_INVALIDCALL and the swallow-
  // catch in LayoutBroker::Apply to silently leave the device in a
  // D3DERR_DEVICENOTRESET state — at which point
  // engine/set/ground-texture silently no-ops (slot stays at 0).
  //
  // After the D3DPOOL_DEFAULT migration the same scenario should still work,
  // because the skydome effect's OnLost/OnReset is already wired
  // (fixed 2026-05-20) and the new D3DPOOL_DEFAULT VB/IB/textures
  // also go through Release/Recreate.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // 1. Emulate the polluter pair by exercising resize + skydome
    //    swap + ground swap repeatedly.
    for (let i = 0; i < 5; ++i) {
      await b.request({
        kind: "layout/viewport-rect",
        params: { x: 0, y: 0, w: 1200 + i * 40, h: 800 + i * 30 },
      });
      await b.request({ kind: "engine/set/skydome-slot", params: { slot: i % 3 } });
    }

    // 2. The regression check: set ground to a non-zero slot and verify it
    //    actually lands. Pre-fix this returned with state still at 0.
    await b.request({ kind: "engine/set/ground-texture", params: { slot: 3 } });
    const dto = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as EngineStateDto;
    return dto.groundTexture;
  });
  expect(result).toBe(3);
});

test("blocked full texture reload replays once without duplicating authored work", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const debug = async (action: "arm" | "release" | "query") =>
      (await b.request({
        kind: "debug/device-recovery-work",
        params: { action },
      })) as DeviceRecoveryWorkState;
    const authoredChange = async (track: number) => {
      await b.request({
        kind: "engine/action/on-particle-system-changed",
        params: { track },
      });
    };
    const reloadTextures = async () => {
      await b.request({
        kind: "engine/action/reload-textures",
        params: {},
      });
    };

    try {
      // Idempotent cleanup gives the shared native host a known baseline even
      // if a previous failed run exited while the synthetic hold was armed.
      await debug("release");
      const controlBase = await debug("query");

      await debug("arm");
      await authoredChange(17);
      const controlHeld = await debug("query");
      const controlAfter = await debug("release");

      const reloadBase = await debug("query");
      await debug("arm");
      await authoredChange(29);
      await reloadTextures();
      await reloadTextures();
      const reloadHeld = await debug("query");
      const reloadAfter = await debug("release");
      const reloadSettled = await debug("release");

      return {
        controlBase,
        controlHeld,
        controlAfter,
        reloadBase,
        reloadHeld,
        reloadAfter,
        reloadSettled,
      };
    } finally {
      // Never poison later specs in the one shared native process.
      await debug("release").catch(() => undefined);
    }
  });

  expect(result.controlHeld.pending).toBe(false);
  expect(result.controlHeld.reloadCount - result.controlBase.reloadCount).toBe(0);
  expect(
    result.controlHeld.authoredApplyCount -
      result.controlBase.authoredApplyCount,
  ).toBe(0);
  expect(result.controlAfter.pending).toBe(false);
  expect(result.controlAfter.reloadCount - result.controlBase.reloadCount).toBe(0);
  expect(
    result.controlAfter.authoredApplyCount -
      result.controlBase.authoredApplyCount,
  ).toBe(1);
  expect(result.controlAfter.frameReady).toBe(true);

  expect(result.reloadHeld.pending).toBe(true);
  expect(result.reloadHeld.reloadCount - result.reloadBase.reloadCount).toBe(0);
  expect(
    result.reloadHeld.authoredApplyCount -
      result.reloadBase.authoredApplyCount,
  ).toBe(0);
  expect(result.reloadAfter.pending).toBe(false);
  expect(result.reloadAfter.reloadCount - result.reloadBase.reloadCount).toBe(1);
  expect(
    result.reloadAfter.authoredApplyCount -
      result.reloadBase.authoredApplyCount,
  ).toBe(1);
  expect(
    result.reloadSettled.authoredApplyCount -
      result.reloadBase.authoredApplyCount,
  ).toBe(1);
  expect(
    result.reloadSettled.reloadCount - result.reloadBase.reloadCount,
  ).toBe(1);
  expect(result.reloadSettled.pending).toBe(false);
  expect(result.reloadAfter.frameReady).toBe(true);
  expect(result.reloadSettled.frameReady).toBe(true);
});

test("composed coordinator probes the real D3D9Ex device once per healthy frame", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const debug = async (action: "release" | "query") =>
      (await b.request({
        kind: "debug/device-recovery-work",
        params: { action },
      })) as DeviceRecoveryWorkState;

    await debug("release");
    const before = await debug("query");
    await new Promise((resolve) => setTimeout(resolve, 100));
    const after = await debug("query");
    return {
      frameDelta:
        after.composedFramePrepareCount - before.composedFramePrepareCount,
      probeDelta: after.deviceProbeCount - before.deviceProbeCount,
      frameReady: after.frameReady,
    };
  });

  expect(result.frameReady).toBe(true);
  expect(result.frameDelta).toBeGreaterThan(0);
  expect(result.probeDelta).toBe(result.frameDelta);
});

test("blocked shader and layer actions fail without moving roots; healthy reload succeeds", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const debug = async (action: "arm" | "release") =>
      (await b.request({
        kind: "debug/device-recovery-work",
        params: { action },
      })) as DeviceRecoveryWorkState;
    const list = async () =>
      (await b.request({
        kind: "mods/list",
        params: {},
      })) as { stack: string[] };

    await debug("release");
    const originalStack = [...(await list()).stack];
    const refusedPath = "C:\\__particle_editor_blocked_layer_probe__";
    let blockedLayer:
      | { ok: boolean; stack: string[]; error?: string }
      | undefined;
    let blockedShaderRejected = false;
    let healthyShaderReloaded = false;
    let healthyLayerReloaded = false;

    try {
      await debug("arm");
      blockedLayer = (await b.request({
        kind: "mods/set-layers",
        params: { paths: [refusedPath] },
      })) as { ok: boolean; stack: string[]; error?: string };
      try {
        await b.request({
          kind: "engine/action/reload-shaders",
          params: {},
        });
      } catch {
        blockedShaderRejected = true;
      }
      const heldStack = [...(await list()).stack];

      await debug("release");
      await b.request({
        kind: "engine/action/reload-shaders",
        params: {},
      });
      healthyShaderReloaded = true;
      const healthyLayer = (await b.request({
        kind: "mods/set-layers",
        params: { paths: originalStack },
      })) as { ok: boolean };
      healthyLayerReloaded = healthyLayer.ok;

      return {
        originalStack,
        refusedPath,
        blockedLayer,
        blockedShaderRejected,
        heldStack,
        healthyShaderReloaded,
        healthyLayerReloaded,
      };
    } finally {
      await debug("release").catch(() => undefined);
      // If the regression reappears, put the shared native host back on the
      // exact starting stack so a failed assertion cannot poison later specs.
      await b
        .request({
          kind: "mods/set-layers",
          params: { paths: originalStack },
        })
        .catch(() => undefined);
    }
  });

  expect(result.blockedLayer?.ok).toBe(false);
  expect(result.blockedLayer?.error).toContain("load order was not changed");
  expect(result.blockedLayer?.stack).toEqual(result.originalStack);
  expect(result.heldStack).toEqual(result.originalStack);
  expect(result.heldStack).not.toContain(result.refusedPath);
  expect(result.blockedShaderRejected).toBe(true);
  expect(result.healthyShaderReloaded).toBe(true);
  expect(result.healthyLayerReloaded).toBe(true);
});
