// Preview overload guard regression spec (preemptive estimate gate).
//
// The crash this replaces: a huge nParticlesPerSecond (typed, Shift-×10'd,
// or chain-multiplied) drove unbounded heap growth in the live preview and
// hard-crashed the editor (OOM; host.log stopped mid-stream). The engine
// now enforces a PREEMPTIVE estimate gate: the web pushes a per-instance
// alive estimate (engine/set/estimated-load) and the engine REFUSES a
// placement (and clears the whole preview) before any heap growth occurs
// when (placed+1)×estimate exceeds the configured cap — or clears
// retroactively when a parameter edit pushes placed×estimate over.
// Refusals surface as engine/overload/refused.
//
// The engine also keeps a runtime particle/instance budget
// (kMaxLivePreviewParticles / kMaxLiveEmitterInstances, engine.h) as a
// belt-and-suspenders backstop for the rare case where the pushed
// estimate undercounts the real population. The earlier specs that drove
// a live 1e9-rate bomb to exercise that backstop directly are retired:
// the estimate-push hook now reports the bomb, so the preemptive gate
// refuses the spawn before the runtime budget ever engages, making those
// visible-rate scenarios unreachable. The gate specs below prove the
// over-budget cases are prevented up front.

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

// Wait for the next `engine/overload/refused` event (the preemptive
// estimate gate's one-shot refusal). Subscribes in-page, resolves the
// payload on the first event, or null on timeout
// (so a missing event surfaces as a clear assertion rather than a hang).
type Refusal = { estimated: number; cap: number; attemptedCount: number };
async function waitForRefusal(timeoutMs: number): Promise<Refusal | null> {
  return page.evaluate(
    ({ timeoutMs }) =>
      new Promise<Refusal | null>((resolve) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const b = (window as any).bridge;
        const timer = setTimeout(() => {
          off();
          resolve(null);
        }, timeoutMs);
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const off = b.on("engine/overload/refused", (e: any) => {
          clearTimeout(timer);
          off();
          resolve(e.payload as Refusal);
        });
      }),
    { timeoutMs },
  );
}

// Count `engine/overload/refused` events over a fixed observation window
// (used by the churn-stop spec to prove EXACTLY one refusal across many
// spawner intervals). Always runs the full window — never short-circuits
// — so an over-firing gate is caught.
async function countRefusals(windowMs: number): Promise<number> {
  return page.evaluate(
    ({ windowMs }) =>
      new Promise<number>((resolve) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const b = (window as any).bridge;
        let count = 0;
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const off = b.on("engine/overload/refused", () => {
          count += 1;
        });
        setTimeout(() => {
          off();
          resolve(count);
        }, windowMs);
      }),
    { windowMs },
  );
}

// Read the live placed-instance count from the next stats/tick (the
// snapshot has no instance count; stats/tick.payload.instances does).
// Resolves -1 on timeout so a stalled stats stream is distinguishable
// from a genuine 0.
async function readInstanceCount(timeoutMs: number): Promise<number> {
  return page.evaluate(
    ({ timeoutMs }) =>
      new Promise<number>((resolve) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const b = (window as any).bridge;
        const timer = setTimeout(() => {
          off();
          resolve(-1);
        }, timeoutMs);
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const off = b.on("stats/tick", (e: any) => {
          clearTimeout(timer);
          off();
          resolve(e.payload.instances as number);
        });
      }),
    { timeoutMs },
  );
}

// Shared manual-spawner config for the gate specs: burstSize 1 so each
// trigger places exactly one instance, no auto interval (enabled:false,
// manual mode), no lifetime cap (maxLifetimeSec:0) so clears are
// attributable to the gate, not lifetime expiry.
const MANUAL_SPAWNER_1 = {
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
};

// ─────────────────────────────────────────────────────────────────────
// Preemptive estimate-gate specs (overload hard-guard, session 38).
//
// These exercise the NEW gate: the web pushes a per-instance alive
// estimate (engine/set/estimated-load); the engine refuses a placement
// (and clears the whole preview) when (placed+1)×estimate exceeds the
// cap, or clears retroactively when an estimate push pushes
// placed×estimate over. Refusals surface as engine/overload/refused.
//
// nullptr ⟺ refusal invariant: SpawnParticleSystem returns nullptr ONLY
// on a gate refusal, so "no instance placed + a refusal event" is the
// observable proof the gate fired.
//
// Every spec restores: guard 10k + estimate 0 + engine/action/clear in
// finally, so the gate is inert for the bomb specs that share this host.
// All caps ≤ 2k (#134 LOW-cap discipline).
// ─────────────────────────────────────────────────────────────────────

test("cumulative spawn gate refuses the over-cap placement and clears the preview", async () => {
  test.setTimeout(120_000);
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });
  await bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 1_000 });
  // estimate 400: 1×400=400 ok, 2×400=800 ok, 3×400=1200 > 1000 → refuse.
  await bridgeRequest("engine/set/estimated-load", { perInstance: 400 });

  const snapshot = await bridgeRequest<{ spawner: unknown }>("engine/state/snapshot", {});
  const origSpawner = snapshot.spawner;

  try {
    await bridgeRequest("spawner/start", MANUAL_SPAWNER_1);
    // Two placements: 1×400 then 2×400 — both within the 1k cap.
    await bridgeRequest("spawner/trigger", {});
    await bridgeRequest("spawner/trigger", {});

    // The third placement is refused: 3×400 = 1200 > 1000. Arm the waiter
    // BEFORE the trigger so the one-shot event isn't missed.
    const refusalP = waitForRefusal(8_000);
    await bridgeRequest("spawner/trigger", {});
    const refusal = await refusalP;
    expect(refusal, "expected an engine/overload/refused event on the 3rd placement").not.toBeNull();
    expect(refusal!.estimated).toBeGreaterThan(1_000);
    expect(refusal!.cap).toBe(1_000);

    // Clear-on-refusal: the whole preview is gone (count 0).
    const instances = await readInstanceCount(5_000);
    expect(instances, "expected the preview cleared to 0 instances after refusal").toBe(0);

    // The editor is alive: a follow-up bridge request resolves.
    const list = await bridgeRequest<{ root: unknown }>("emitters/list", {});
    expect(list).toBeTruthy();
  } finally {
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    await cleanupStep("estimate-reset", () =>
      bridgeRequest("engine/set/estimated-load", { perInstance: 0 }),
    );
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
    await cleanupStep("guard-restore", () =>
      bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 10_000 }),
    );
  }
});

test("edit-time estimate push over the cap clears the already-placed preview", async () => {
  test.setTimeout(120_000);
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });
  await bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 1_000 });
  await bridgeRequest("engine/set/estimated-load", { perInstance: 400 });

  const snapshot = await bridgeRequest<{ spawner: unknown }>("engine/state/snapshot", {});
  const origSpawner = snapshot.spawner;

  try {
    await bridgeRequest("spawner/start", MANUAL_SPAWNER_1);
    // Two placements at estimate 400 → 2×400 = 800, within the 1k cap,
    // no refusal.
    await bridgeRequest("spawner/trigger", {});
    await bridgeRequest("spawner/trigger", {});

    // A parameter edit raises the estimate to 600: 2×600 = 1200 > 1000.
    // The edit-time check inside SetEstimatedLoad clears the preview and
    // records the refusal. Arm the waiter before the push.
    const refusalP = waitForRefusal(8_000);
    await bridgeRequest("engine/set/estimated-load", { perInstance: 600 });
    const refusal = await refusalP;
    expect(refusal, "expected an engine/overload/refused event from the edit-time check").not.toBeNull();
    expect(refusal!.estimated).toBeGreaterThan(1_000);
    expect(refusal!.cap).toBe(1_000);

    const instances = await readInstanceCount(5_000);
    expect(instances, "expected the preview cleared to 0 after the edit-time check").toBe(0);
  } finally {
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    await cleanupStep("estimate-reset", () =>
      bridgeRequest("engine/set/estimated-load", { perInstance: 0 }),
    );
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
    await cleanupStep("guard-restore", () =>
      bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 10_000 }),
    );
  }
});

test("a single instance over the cap is refused on every retry (no lock-out)", async () => {
  test.setTimeout(120_000);
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });
  await bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 1_000 });
  // 1×1500 = 1500 > 1000 → even a single placement is refused.
  await bridgeRequest("engine/set/estimated-load", { perInstance: 1_500 });

  const snapshot = await bridgeRequest<{ spawner: unknown }>("engine/state/snapshot", {});
  const origSpawner = snapshot.spawner;

  try {
    await bridgeRequest("spawner/start", MANUAL_SPAWNER_1);

    // First trigger → refused. (After a clear-on-refusal the count is 0,
    // so the next trigger re-checks 0+1 from scratch — proving there is
    // no persistent locked state.)
    const r1P = waitForRefusal(8_000);
    await bridgeRequest("spawner/trigger", {});
    const r1 = await r1P;
    expect(r1, "expected the first over-cap placement to be refused").not.toBeNull();
    expect(r1!.estimated).toBeGreaterThan(1_000);

    // Second trigger → refused AGAIN (a distinct event). No lock-out.
    const r2P = waitForRefusal(8_000);
    await bridgeRequest("spawner/trigger", {});
    const r2 = await r2P;
    expect(r2, "expected the retry to be refused again (no lock-out state)").not.toBeNull();
    expect(r2!.estimated).toBeGreaterThan(1_000);
  } finally {
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    await cleanupStep("estimate-reset", () =>
      bridgeRequest("engine/set/estimated-load", { perInstance: 0 }),
    );
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
    await cleanupStep("guard-restore", () =>
      bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 10_000 }),
    );
  }
});

test("an auto spawner refused over-cap fires one banner and self-disables", async () => {
  test.setTimeout(120_000);
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });
  await bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 1_000 });
  await bridgeRequest("engine/set/estimated-load", { perInstance: 1_500 });

  const snapshot = await bridgeRequest<{ spawner: unknown }>("engine/state/snapshot", {});
  const origSpawner = snapshot.spawner;

  try {
    // Auto/interval spawner with a SHORT interval: without the churn stop
    // it would refuse-and-clear every interval, producing one banner per
    // cycle. The driver must self-disable after the FIRST refusal.
    await bridgeRequest("spawner/start", {
      mode: "auto",
      enabled: true,
      burstSize: 1,
      spacingSec: 0,
      intervalSec: 0.25,
      position: [0, 0, 0],
      velocity: [0, 0, 0],
      maxLifetimeSec: 0,
      jitterPosition: [0, 0, 0],
      jitterVelocity: [0, 0, 0],
    });

    // Observe across many intervals (3s ≫ 0.25s interval): exactly one
    // refusal proves the driver disabled itself after the first.
    const count = await countRefusals(3_000);
    expect(count, "expected EXACTLY one refusal across the multi-interval window").toBe(1);

    // The driver self-disabled: the snapshot's spawner reports enabled:false.
    const after = await bridgeRequest<{ spawner: { enabled: boolean } }>(
      "engine/state/snapshot",
      {},
    );
    expect(after.spawner.enabled, "expected the spawner to self-disable on refusal").toBe(false);
  } finally {
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    await cleanupStep("estimate-reset", () =>
      bridgeRequest("engine/set/estimated-load", { perInstance: 0 }),
    );
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
    await cleanupStep("guard-restore", () =>
      bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 10_000 }),
    );
  }
});

test("a disabled guard bypasses the estimate gate (instance placed, no refusal)", async () => {
  test.setTimeout(120_000);
  await bridgeRequest("stats/set-frozen", { frozen: false });
  await bridgeRequest("engine/set/paused", { paused: false });
  // Guard DISABLED: the hard gate is OFF regardless of the estimate
  // (#123 "uncapped is an explicit power-user choice").
  await bridgeRequest("engine/set/overload-guard", { enabled: false, maxParticles: 1_000 });
  // A huge estimate that WOULD be refused if the guard were enabled.
  await bridgeRequest("engine/set/estimated-load", { perInstance: 5_000 });

  const snapshot = await bridgeRequest<{ spawner: unknown }>("engine/state/snapshot", {});
  const origSpawner = snapshot.spawner;

  try {
    await bridgeRequest("spawner/start", MANUAL_SPAWNER_1);

    // Watch for a refusal while triggering — none should fire.
    const refusalP = waitForRefusal(4_000);
    await bridgeRequest("spawner/trigger", {});
    const refusal = await refusalP;
    expect(refusal, "expected NO refusal while the guard is disabled").toBeNull();

    // The instance IS placed (gate bypassed).
    const instances = await readInstanceCount(5_000);
    expect(instances, "expected an instance to be placed with the guard disabled").toBeGreaterThanOrEqual(1);
  } finally {
    await cleanupStep("spawner-stop", () => bridgeRequest("spawner/stop", {}));
    if (origSpawner) {
      await cleanupStep("spawner-restore", () => bridgeRequest("spawner/start", origSpawner));
    }
    await cleanupStep("estimate-reset", () =>
      bridgeRequest("engine/set/estimated-load", { perInstance: 0 }),
    );
    await cleanupStep("engine-clear", () => bridgeRequest("engine/action/clear", {}));
    await cleanupStep("guard-restore", () =>
      bridgeRequest("engine/set/overload-guard", { enabled: true, maxParticles: 10_000 }),
    );
  }
});
