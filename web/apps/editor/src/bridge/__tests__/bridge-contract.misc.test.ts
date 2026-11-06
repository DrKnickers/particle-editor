// Contract tests for MockBridge — the spawner, texture previews, and on()/event fan-out.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { MockBridge } from "../mock";
import { makeMockBridgeWithListener } from "@/test/mock-bridge";
import {
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type EnginePreviewPendingDto,
  type EngineStateDto,
  type Event,
} from "@particle-editor/bridge-schema";
import { resetMockState } from "@/test/mock-state";
import { EXTERNAL_LINKS } from "@/lib/external-links";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — app/open-external", () => {
  afterEach(() => vi.restoreAllMocks());

  it.each(["guide", "repository"] as const)("opens the fixed %s URL without mutating the document", async (target) => {
    const open = vi.spyOn(window, "open").mockReturnValue(null);
    const b = new MockBridge();
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    const dirty = vi.fn();
    const off = b.on("dirty/changed", dirty);
    try {
      await expect(b.request({ kind: "app/open-external", params: { target } })).resolves.toEqual({ opened: true });
      expect(open).toHaveBeenCalledTimes(1);
      expect(open).toHaveBeenCalledWith(EXTERNAL_LINKS[target], "_blank", "noopener");
      expect(await b.request({ kind: "engine/state/snapshot", params: {} })).toEqual(before);
      expect(dirty).not.toHaveBeenCalled();
    } finally {
      off();
    }
  });

  it.each(["", "Guide", "Repository", "https://example.com", "file:///C:/test", "__proto__"])(
    "rejects unknown target %s without opening a window",
    async (target) => {
      const open = vi.spyOn(window, "open").mockReturnValue(null);
      const b = new MockBridge();
      await expect(
        b.request({
          kind: "app/open-external",
          params: { target: target as "guide" },
        }),
      ).rejects.toThrow("app/open-external: unknown target");
      expect(open).not.toHaveBeenCalled();
    },
  );
});

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
        matchProjectile: true,
        targetPosition: [10_001, -10_001, 10_001],
        bombingRun: true,
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
      matchProjectile: true,
      targetPosition: [10_000, -10_000, 10_000],
      bombingRun: true,
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
    let lastCount: number | null = null;
    const { b, off } = makeMockBridgeWithListener("spawner/active-count", (e) => {
      lastCount = e.payload.count;
    });
    let lastState: { spawner?: unknown } | null = null;
    const offState = b.on("engine/state/changed", (e) => {
      lastState = e.payload;
    });
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
    const seen: Event[] = [];
    const { b, off } = makeMockBridgeWithListener("engine/state/changed", (e) => {
      seen.push(e);
    });
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
    b.on("engine/state/changed", () => {
      throw new Error("subscriber bug");
    });
    b.on("engine/state/changed", () => {
      seen++;
    });
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
});

describe("MockBridge contract — engine host settings", (): void => {
  it("keeps a configured host absent on status and starts it only on live start", async (): Promise<void> => {
    vi.useFakeTimers();
    try {
      const bridge: MockBridge = new MockBridge();
      await bridge.request({ kind: "engineHost/settings/set", params: { exePath: "C:\\Preview\\editor.exe" } });
      const checked: ReturnType<typeof vi.fn> = vi.fn();
      bridge.on(ENGINE_PREVIEW_STATUS_CHECKED_EVENT, checked);
      const pending: EnginePreviewPendingDto = await bridge.request({ kind: "enginePreview/status", params: {} });
      await vi.runAllTimersAsync();
      expect(checked).toHaveBeenCalledWith(
        expect.objectContaining({ payload: { state: "absent", requestId: pending.requestId } }),
      );
      expect(bridge.enginePreview.state).toBe("absent");
      expect(await bridge.request({ kind: "engineLive/start", params: {} })).toEqual({ state: "starting" });
      await vi.runAllTimersAsync();
      expect(bridge.enginePreview.live).toBe("live");
    } finally {
      vi.useRealTimers();
    }
  });
  it("round-trips and clears the executable without dirtying the document", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    const path = "C:\\Preview\\EAW Terrain Editor.exe";
    const before: EngineStateDto = await bridge.request({ kind: "engine/state/snapshot", params: {} });
    expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: null });
    expect(await bridge.request({ kind: "engineHost/settings/set", params: { exePath: path } })).toEqual({
      exePath: path,
    });
    expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: path });
    expect(await bridge.request({ kind: "engineHost/settings/set", params: { exePath: null } })).toEqual({
      exePath: null,
    });
    expect(await bridge.request({ kind: "engine/state/snapshot", params: {} })).toEqual(before);
    expect(await bridge.request({ kind: "file/pick-open", params: { filter: "exe" } })).toEqual({
      ok: false,
      error: "browser-mode",
    });
  });
  it.each(["", "relative.exe", "C:\\Preview\\editor.txt", "C:editor.exe"])(
    "refuses invalid executable %s",
    async (exePath: string): Promise<void> => {
      const bridge: MockBridge = new MockBridge();
      await expect(bridge.request({ kind: "engineHost/settings/set", params: { exePath } })).rejects.toThrow(
        "absolute .exe",
      );
      expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: null });
    },
  );
});
