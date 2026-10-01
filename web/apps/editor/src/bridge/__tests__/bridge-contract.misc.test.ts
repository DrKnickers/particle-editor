// Contract tests for MockBridge — the spawner, texture previews, and on()/event fan-out.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { MockBridge } from "../mock";
import type {
  Event,
} from "@particle-editor/bridge-schema";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — spawner/*, textures/*, subscriptions", () => {
  // ─── spawner + emitters/preview-from-file
  it("spawner/start clamps every native-bound field before the mock echoes it", async () => {
    const b = new MockBridge();
    let lastSnap: { spawner?: unknown } | null = null;
    const off = b.on("engine/state/changed", (e) => {
      lastSnap = e.payload;
    });

    await b.request({
      kind: "spawner/start",
      params: {
        mode: "unexpected" as unknown as "auto",
        enabled: true,
        burstSize: 99,
        spacingSec: -1,
        intervalSec: 61,
        position: [10_001, -10_001, 10_001],
        velocity: [-10_001, 10_001, -10_001],
        maxLifetimeSec: -1,
        jitterPosition: [10_001, -10_001, 10_001],
        acceleration: [-10_001, 10_001, -10_001],
        squiggleAmplitude: [10_001, -10_001, 10_001],
        squiggleFrequency: 21,
      },
    });

    const expected = {
      mode: "auto",
      enabled: true,
      burstSize: 10,
      spacingSec: 0,
      intervalSec: 60,
      position: [10_000, -10_000, 10_000],
      velocity: [-10_000, 10_000, -10_000],
      maxLifetimeSec: 0,
      jitterPosition: [10_000, -10_000, 10_000],
      acceleration: [-10_000, 10_000, -10_000],
      squiggleAmplitude: [10_000, -10_000, 10_000],
      squiggleFrequency: 20,
    };
    expect(lastSnap).not.toBeNull();
    expect(lastSnap!.spawner).toEqual(expected);

    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.spawner).toEqual(expected);
    off();
  });

  it("spawner/trigger is a no-op in Auto mode and bumps the count in Manual mode", async () => {
    const b = new MockBridge();
    let count = 0;
    let countEvents = 0;
    const off = b.on("spawner/active-count", (e) => {
      count = e.payload.count;
      countEvents += 1;
    });

    const r = await b.request({ kind: "spawner/trigger", params: {} });
    expect(r).toEqual({});
    expect(countEvents).toBe(0);
    expect(count).toBe(0);

    const initial = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({
      kind: "spawner/start",
      params: { ...initial.spawner, mode: "manual" },
    });
    await b.request({ kind: "spawner/trigger", params: {} });
    expect(countEvents).toBe(1);
    expect(count).toBe(1);
    off();
  });

  // ─── host-state plumbing: MockBridge mirrors the native spawner
  // state-change contract so browser-mode tests cannot bless a divergence.
  it("spawner/stop disables the seeded config, broadcasts it, and resets the active count", async () => {
    const b = new MockBridge();
    let lastCount: number | null = null;
    const off = b.on("spawner/active-count", (e) => { lastCount = e.payload.count; });
    let lastState: { spawner?: unknown } | null = null;
    const offState = b.on("engine/state/changed", (e) => { lastState = e.payload; });
    const initial = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({
      kind: "spawner/start",
      params: {
        ...initial.spawner,
        mode: "manual",
        enabled: true,
        burstSize: 7,
        position: [1, 2, 3],
      },
    });
    const beforeStop = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({ kind: "spawner/trigger", params: {} });
    expect(lastCount).toBeGreaterThan(0);

    const r = await b.request({ kind: "spawner/stop", params: {} });
    expect(r).toEqual({});
    expect(lastCount).toBe(0);
    expect(lastState!.spawner).toMatchObject({ enabled: false });
    const afterStop = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect({ ...afterStop.spawner, enabled: true }).toEqual(beforeStop.spawner);
    off();
    offState();
  });

  it("on() returns a working unsubscribe", async () => {
    const b = new MockBridge();
    const seen: Event[] = [];
    const off = b.on("engine/state/changed", (e) => { seen.push(e); });
    await b.request({ kind: "engine/set/ground-z", params: { z: 1 } });
    off();
    await b.request({ kind: "engine/set/ground-z", params: { z: 2 } });
    expect(seen).toHaveLength(1);
  });
});

describe("MockBridge event fan-out", () => {
  it("a throwing subscriber is logged and neither fails the request nor starves later subscribers", async () => {
    const b = new MockBridge();
    const err = vi.spyOn(console, "error").mockImplementation(() => {});
    let seen = 0;
    b.on("engine/state/changed", () => { throw new Error("subscriber bug"); });
    b.on("engine/state/changed", () => { seen++; });
    await expect(b.request({ kind: "engine/set/ground-z", params: { z: 3 } })).resolves.toEqual({});
    expect(seen).toBe(1);
    expect(err).toHaveBeenCalled();
    err.mockRestore();
  });
});

describe("textures/get-preview (mock parity)", () => {
  it("ok for a normal filename, with data URI + source dims", async () => {
    const b = new MockBridge();
    const res = await b.request({ kind: "textures/get-preview", params: { filename: "fire.dds" } });
    // Unconditional: a regression to a non-ok status fails loudly instead of
    // silently skipping the inner asserts below.
    expect(res.status).toBe("ok");
    if (res.status === "ok") {
      expect(res.dataUri.startsWith("data:image/png;base64,")).toBe(true);
      expect(res.srcW).toBeGreaterThan(0);
      expect(res.srcH).toBeGreaterThan(0);
    }
  });
  it("missing/broken sentinels", async () => {
    const b = new MockBridge();
    expect((await b.request({ kind: "textures/get-preview", params: { filename: "__missing__.dds" } })).status).toBe("missing");
    expect((await b.request({ kind: "textures/get-preview", params: { filename: "__broken__.dds" } })).status).toBe("broken");
  });
});
