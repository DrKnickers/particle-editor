import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  initModStack,
  getModStack,
  __setModStackForTests,
  __resetModStackForTests,
} from "../mod-stack";
import { __resetPreviewCache } from "../atlas-preview-cache";
import * as cache from "../atlas-preview-cache";

beforeEach(() => {
  __resetModStackForTests();
  __resetPreviewCache();
});

// Minimal fake bridge: request returns a mods/list response,
// on() captures the handler and returns an unsubscribe stub.
//
// stackRef is a mutable box so tests can change what mods/list returns
// between the initial seed and a subsequent engine/state/changed fire —
// needed to exercise the "only invalidate on change" path.
function makeFakeBridge(initialStack: string[] = ["A", "B"]) {
  const stackRef = { current: initialStack };
  const handlers = new Map<string, ((...args: unknown[]) => void)[]>();

  const bridge = {
    stackRef,
    request: vi.fn().mockImplementation(() =>
      Promise.resolve({
        mods: [],
        layers: [],
        stack: stackRef.current,
        activePath: stackRef.current[0] ?? null,
      }),
    ),
    on: vi.fn((kind: string, handler: (...args: unknown[]) => void) => {
      const bucket = handlers.get(kind) ?? [];
      bucket.push(handler);
      handlers.set(kind, bucket);
      return () => {
        const b = handlers.get(kind) ?? [];
        handlers.set(kind, b.filter((h) => h !== handler));
      };
    }),
    /** Test helper — fire a fake engine/state/changed event. */
    fire(kind: string, payload = {}) {
      (handlers.get(kind) ?? []).forEach((h) => h({ kind, payload }));
    },
  };

  return bridge;
}

describe("mod-stack store", () => {
  it("__setModStackForTests + getModStack round-trips", () => {
    __setModStackForTests(["A", "B"]);
    expect(getModStack()).toEqual(["A", "B"]);
  });

  it("__resetModStackForTests clears the stack", () => {
    __setModStackForTests(["X"]);
    __resetModStackForTests();
    expect(getModStack()).toEqual([]);
  });
});

describe("initModStack", () => {
  it("seeds the store from mods/list on init", async () => {
    const bridge = makeFakeBridge(["C", "D"]);
    initModStack(bridge as never);
    // mods/list is async — wait for microtasks
    await vi.waitFor(() => expect(getModStack()).toEqual(["C", "D"]));
  });

  it("registers an engine/state/changed handler", () => {
    const bridge = makeFakeBridge();
    initModStack(bridge as never);
    expect(bridge.on).toHaveBeenCalledWith("engine/state/changed", expect.any(Function));
  });

  it("refreshes the stack when engine/state/changed fires", async () => {
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);
    await vi.waitFor(() => expect(getModStack()).toEqual(["A"]));

    // Simulate a mod switch: mutate stackRef so bridge returns a new stack.
    bridge.stackRef.current = ["B", "A"];
    bridge.fire("engine/state/changed");

    await vi.waitFor(() => expect(getModStack()).toEqual(["B", "A"]));
  });

  it("calls invalidatePreviewCache when engine/state/changed fires with a CHANGED stack", async () => {
    const invalidate = vi.spyOn(cache, "invalidatePreviewCache");
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);

    // The initial seed: prev=[] → next=["A"] → different → invalidate called once.
    await vi.waitFor(() => expect(invalidate).toHaveBeenCalledTimes(1));

    // Fire with a different stack → should invalidate again.
    bridge.stackRef.current = ["B", "A"];
    bridge.fire("engine/state/changed");
    await vi.waitFor(() => expect(invalidate).toHaveBeenCalledTimes(2));
  });

  it("does NOT call invalidatePreviewCache when engine/state/changed fires with an UNCHANGED stack", async () => {
    const invalidate = vi.spyOn(cache, "invalidatePreviewCache");
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);

    // Wait for the initial seed to settle (invalidate called once).
    await vi.waitFor(() => expect(invalidate).toHaveBeenCalledTimes(1));

    // Fire with the same stack — stack unchanged, cache must NOT be invalidated.
    bridge.fire("engine/state/changed");
    // Give microtasks time to flush.
    await new Promise((r) => setTimeout(r, 0));
    expect(invalidate).toHaveBeenCalledTimes(1);
  });

  it("returns an unsubscribe function that can be called", () => {
    const bridge = makeFakeBridge();
    const off = initModStack(bridge as never);
    expect(typeof off).toBe("function");
    expect(() => off()).not.toThrow();
  });
});
