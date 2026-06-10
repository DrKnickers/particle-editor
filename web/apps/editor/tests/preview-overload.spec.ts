// Preview overload guard regression spec (session 35 part 2).
//
// The crash this replaces: a huge nParticlesPerSecond (typed, Shift-×10'd,
// or chain-multiplied) drove unbounded heap growth in the live preview and
// hard-crashed the editor (OOM; host.log stopped mid-stream). The engine
// now enforces kMaxLivePreviewParticles / kMaxLiveEmitterInstances
// (engine.h): over budget it SUPPRESSES spawning, latches an overload
// flag surfaced on `stats/tick`, and resumes when the population decays
// (hysteresis at 90% of the cap).
//
// Test flow (one serial test — the phases share live-engine state):
//   1. Bomb: patch the first root emitter to rate=1e9, lifetime=5 and
//      spawn a preview instance via the manual SpawnerDriver (new
//      instances read the authored values at construction — the same
//      path the real crash took).
//   2. Assert a stats/tick arrives with overload=true and particles
//      within budget (the plateau replaces the old OOM death).
//   3. Restore: patch the rate back to 10 and push the change into the
//      LIVE instance via engine/action/on-particle-system-changed(-1)
//      (set-properties alone only writes the data model; live
//      EmitterInstances cache their spawn delay). Assert overload clears
//      once the population decays.
//   4. Cleanup (finally): restore original properties + spawner config,
//      engine/action/clear to kill the lingering preview instance so
//      later runs / specs aren't poisoned. The doc is never saved, so
//      there's no persistence risk.
//
// A second test exercises the OTHER refusal path: a death child under
// overload makes every particle death call SpawnEmitter, which the
// instance budget (kMaxLiveEmitterInstances) refuses with nullptr —
// KillParticle must tolerate that thousands of times per second.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

// Engine::kMaxLivePreviewParticles (src/engine.h) is 100_000, plus
// slack since the stats counter is sampled at 4 Hz between frames. In
// practice a SINGLE emitter plateaus far lower (the per-instance uint16
// index cap, 16,383) — the assertion only needs to prove the population
// is bounded, not which ceiling bit first.
const BUDGET_SLACK = 110_000;

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as unknown as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

// Bridge request helper evaluated in the page. Kept as a string-free
// page.evaluate per the house CDP idiom (see emitter-tree.spec.ts).
async function bridgeRequest<T>(kind: string, params: unknown): Promise<T> {
  return page.evaluate(
    async ({ kind, params }) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge;
      return b.request({ kind, params });
    },
    { kind, params },
  ) as Promise<T>;
}

// Best-effort cleanup wrapper for finally blocks: if the host died or
// wedged, a throwing cleanup call would REPLACE the original assertion
// error with an unhelpful bridge error. Log and continue instead.
async function cleanupStep(label: string, fn: () => Promise<unknown>): Promise<void> {
  try {
    await fn();
  } catch (err) {
    console.log(`[preview-overload] cleanup step '${label}' failed (ignored):`, String(err));
  }
}

// Wait for the next stats/tick whose payload.overload matches `want`.
// Resolves { hit: null } on timeout instead of throwing, carrying every
// observed tick so a failure message shows exactly what the host
// reported during the window.
type Tick = { fps: number; particles: number; instances: number; overload: boolean };
async function waitForOverload(
  want: boolean,
  timeoutMs: number,
): Promise<{ hit: Tick | null; seen: Tick[] }> {
  return page.evaluate(
    ({ want, timeoutMs }) =>
      new Promise<{ hit: Tick | null; seen: Tick[] }>((resolve) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const b = (window as any).bridge;
        const seen: Tick[] = [];
        const timer = setTimeout(() => {
          off();
          resolve({ hit: null, seen });
        }, timeoutMs);
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const off = b.on("stats/tick", (e: any) => {
          seen.push(e.payload);
          if (e.payload.overload === want) {
            clearTimeout(timer);
            off();
            resolve({ hit: e.payload, seen });
          }
        });
      }),
    { want, timeoutMs },
  );
}

test("huge spawn rate plateaus at the budget, latches overload, and recovers", async () => {
  // Phases: bomb (≤10 s) + decay (≤20 s) + cleanup — needs more than the
  // 30 s config default. The generous ceiling also covers inflated
  // bridge round-trip latency while the host is simulating tens of
  // thousands of Debug-build particles (observed: ~70 s total in a
  // full-harness run whose document carried an extra emitter).
  test.setTimeout(150_000);

  // Defensive: make sure the 4 Hz stats stream is flowing (an earlier
  // crashed a11y spec could have left stats frozen).
  await bridgeRequest("stats/set-frozen", { frozen: false });
  // Defensive: unpause the preview clock. The a11y composition specs
  // pause it in beforeEach (engine/set/paused: true) and their afterAll
  // cleanup unfreezes stats + file/new but does NOT unpause — with the
  // clock frozen no spawn round ever fires, so overload could never
  // latch (bit this spec on its first full-harness run).
  await bridgeRequest("engine/set/paused", { paused: false });

  // Locate the first root emitter and snapshot the fields we'll touch.
  const tree = await bridgeRequest<{ root: { children: { id: number }[] } }>(
    "emitters/list",
    {},
  );
  const targetId = tree.root.children[0]?.id;
  expect(targetId).not.toBeUndefined();

  const before = await bridgeRequest<{
    properties: { lifetime: number; useBursts: boolean; nParticlesPerSecond: number };
  }>("emitters/get-properties", { id: targetId });
  const orig = before.properties;

  const snapshot = await bridgeRequest<{ spawner: unknown }>(
    "engine/state/snapshot",
    {},
  );
  const origSpawner = snapshot.spawner;

  try {
    // ── Phase 1: arm the bomb ────────────────────────────────────────
    // rate=1e9 alone exceeds the 100k budget thousands of times over
    // every frame — no chain needed. lifetime=5 keeps the population
    // alive long enough for a deterministic plateau read at 4 Hz.
    await bridgeRequest("emitters/set-properties", {
      id: targetId,
      patch: { nParticlesPerSecond: 1_000_000_000, lifetime: 5, useBursts: false },
    });

    // Spawn one preview instance via the manual spawner — instances
    // constructed AFTER the patch read the huge rate (the real crash
    // path). maxLifetimeSec=0: no cap, so recovery below is attributable
    // to the rate restore, not lifetime expiry.
    await bridgeRequest("spawner/start", {
      mode: "manual",
      enabled: false,
      burstSize: 1,
      spacingSec: 0,
      intervalSec: 10,
      position: [0, 0, 0],
      velocity: [0, 0, 0],
      maxLifetimeSec: 0,
      jitterPosition: [0, 0, 0],
      jitterVelocity: [0, 0, 0],
    });
    await bridgeRequest("spawner/trigger", {});

    // ── Phase 2: the engine survives and latches ─────────────────────
    const overloaded = await waitForOverload(true, 10_000);
    // Diagnostics on failure: show what the host reported (paused state,
    // patched props, and every tick observed in the window).
    if (overloaded.hit === null) {
      const snap = await bridgeRequest<{ paused: boolean }>("engine/state/snapshot", {});
      const props = await bridgeRequest<{ properties: unknown }>(
        "emitters/get-properties",
        { id: targetId },
      );
      console.log("[preview-overload] paused:", JSON.stringify(snap.paused));
      console.log("[preview-overload] target props:", JSON.stringify(props.properties));
      console.log("[preview-overload] ticks seen:", JSON.stringify(overloaded.seen));
    }
    expect(overloaded.hit, "expected a stats/tick with overload=true").not.toBeNull();
    expect(overloaded.hit!.overload).toBe(true);
    expect(overloaded.hit!.particles).toBeLessThanOrEqual(BUDGET_SLACK);

    // The editor is still responsive: a bridge round-trip completes.
    // (Pre-guard, the process was dead by now.)
    const alive = await bridgeRequest<{ selectedEmitterId: number | null }>(
      "engine/state/snapshot",
      {},
    );
    expect(alive).toBeTruthy();

    // ── Phase 3: lower the rate → overload clears ────────────────────
    await bridgeRequest("emitters/set-properties", {
      id: targetId,
      patch: { nParticlesPerSecond: 10, lifetime: 1 },
    });
    // Push the new values into the LIVE EmitterInstance (it caches its
    // spawn delay at construction / on this notification).
    await bridgeRequest("engine/action/on-particle-system-changed", { track: -1 });

    // Population decays as the 5 s-lifetime particles die; spawning
    // resumes below 90% of the cap and the latch drops.
    const recovered = await waitForOverload(false, 20_000);
    if (recovered.hit === null) {
      console.log("[preview-overload] recovery ticks seen:", JSON.stringify(recovered.seen));
    }
    expect(recovered.hit, "expected overload to clear after the rate drop").not.toBeNull();
    expect(recovered.hit!.overload).toBe(false);
  } finally {
    // ── Phase 4: cleanup even on failure (best-effort — see cleanupStep) ──
    await cleanupStep("restore-properties", () =>
      bridgeRequest("emitters/set-properties", {
        id: targetId,
        patch: {
          nParticlesPerSecond: orig.nParticlesPerSecond,
          lifetime: orig.lifetime,
          useBursts: orig.useBursts,
        },
      }),
    );
    await cleanupStep("push-live", () =>
      bridgeRequest("engine/action/on-particle-system-changed", { track: -1 }),
    );
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    // Kill the lingering preview instance (and reset the budget latch).
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
  }
});

test("death-child spawns are refused under overload and the editor survives", async () => {
  // Latch (≤15 s) + a short soak + cleanup, in a Debug build.
  test.setTimeout(90_000);

  // Defensive (same as test 1): stats flowing, clock running.
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });

  const tree = await bridgeRequest<{ root: { children: { id: number }[] } }>(
    "emitters/list",
    {},
  );
  const targetId = tree.root.children[0]?.id;
  expect(targetId).not.toBeUndefined();

  const before = await bridgeRequest<{
    properties: { lifetime: number; useBursts: boolean; nParticlesPerSecond: number };
  }>("emitters/get-properties", { id: targetId });
  const orig = before.properties;

  const snapshot = await bridgeRequest<{ spawner: unknown }>(
    "engine/state/snapshot",
    {},
  );
  const origSpawner = snapshot.spawner;

  let childId: number | null = null;
  try {
    // ── Phase 1: wire a death child onto the root emitter ────────────
    // Every parent-particle death now calls SpawnEmitter for the child;
    // past kMaxLiveEmitterInstances (5,000) that returns nullptr — the
    // refusal path inside EmitterInstance::KillParticle.
    const added = await bridgeRequest<{ newId: number }>("emitters/add-death-child", {
      parentId: targetId,
    });
    childId = added.newId;
    // newId: -1 means the death slot was already filled — the boot doc
    // shouldn't have one; fail loudly rather than testing nothing.
    expect(childId, "add-death-child refused (slot already filled?)").toBeGreaterThanOrEqual(0);

    // Child: moderate rate so the (≤5k) successfully spawned death
    // children don't blow the particle budget on their own.
    await bridgeRequest("emitters/set-properties", {
      id: childId,
      patch: { nParticlesPerSecond: 100, lifetime: 0.5, useBursts: false },
    });
    // Parent: high rate + SHORT lifetime so deaths fire constantly
    // (~50k deaths/s at steady state — each one a SpawnEmitter attempt,
    // refused once the instance cap is pinned).
    await bridgeRequest("emitters/set-properties", {
      id: targetId,
      patch: { nParticlesPerSecond: 50_000, lifetime: 0.2, useBursts: false },
    });

    // Spawn one preview instance reading the patched values (same
    // manual-spawner path as test 1).
    await bridgeRequest("spawner/start", {
      mode: "manual",
      enabled: false,
      burstSize: 1,
      spacingSec: 0,
      intervalSec: 10,
      position: [0, 0, 0],
      velocity: [0, 0, 0],
      maxLifetimeSec: 0,
      jitterPosition: [0, 0, 0],
      jitterVelocity: [0, 0, 0],
    });
    await bridgeRequest("spawner/trigger", {});

    // ── Phase 2: instance-budget refusals latch overload ─────────────
    const overloaded = await waitForOverload(true, 15_000);
    if (overloaded.hit === null) {
      const snap = await bridgeRequest<{ paused: boolean }>("engine/state/snapshot", {});
      console.log("[preview-overload] death-child paused:", JSON.stringify(snap.paused));
      console.log("[preview-overload] death-child ticks seen:", JSON.stringify(overloaded.seen));
    }
    expect(overloaded.hit, "expected overload=true via death-child instance refusals").not.toBeNull();
    expect(overloaded.hit!.overload).toBe(true);
    expect(overloaded.hit!.particles).toBeLessThanOrEqual(BUDGET_SLACK);

    // ── Phase 3: soak the refusal path, then prove responsiveness ────
    // ~5 s pinned at the instance cap ≈ tens of thousands of refused
    // KillParticle→SpawnEmitter calls. The editor must keep answering.
    await page.waitForTimeout(5_000);
    const alive = await bridgeRequest<{ selectedEmitterId: number | null }>(
      "engine/state/snapshot",
      {},
    );
    expect(alive).toBeTruthy();
  } finally {
    // ── Cleanup even on failure (best-effort — see cleanupStep) ──────
    await cleanupStep("restore-properties", () =>
      bridgeRequest("emitters/set-properties", {
        id: targetId,
        patch: {
          nParticlesPerSecond: orig.nParticlesPerSecond,
          lifetime: orig.lifetime,
          useBursts: orig.useBursts,
        },
      }),
    );
    if (childId !== null) {
      // Removes the death child from the doc AND kills its live
      // instances (RemoveEmitter path — including their particle
      // accounting).
      await cleanupStep("delete-death-child", () =>
        bridgeRequest("emitters/delete", { id: childId }),
      );
    }
    await cleanupStep("push-live", () =>
      bridgeRequest("engine/action/on-particle-system-changed", { track: -1 }),
    );
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    // Kill the lingering preview instance (and reset the budget latch).
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
  }
});
