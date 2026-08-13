import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  initModStack,
  useModStack,
  refreshModStack,
  moveItemToGap,
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
  it("moves an item to the requested insertion gap without mutating the input", () => {
    const order = ["A", "B", "C"];
    expect(moveItemToGap(order, 0, 3)).toEqual(["B", "C", "A"]);
    expect(order).toEqual(["A", "B", "C"]);
  });

  it("__setModStackForTests updates the hook store", () => {
    __setModStackForTests(["A", "B"]);
    expect(useModStack.getState().stack).toEqual(["A", "B"]);
  });

  it("__resetModStackForTests clears the stack", () => {
    __setModStackForTests(["X"]);
    __resetModStackForTests();
    expect(useModStack.getState().stack).toEqual([]);
  });
});

describe("initModStack", () => {
  it("seeds the store from mods/list on init", async () => {
    const bridge = makeFakeBridge(["C", "D"]);
    initModStack(bridge as never);
    // mods/list is async — wait for microtasks
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["C", "D"]));
  });

  it("registers an engine/state/changed handler", () => {
    const bridge = makeFakeBridge();
    initModStack(bridge as never);
    expect(bridge.on).toHaveBeenCalledWith("engine/state/changed", expect.any(Function));
  });

  it("refreshes the stack when engine/state/changed fires", async () => {
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["A"]));

    // Simulate a mod switch: mutate stackRef so bridge returns a new stack.
    bridge.stackRef.current = ["B", "A"];
    bridge.fire("engine/state/changed");

    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["B", "A"]));
  });

  it("does not request mods/list when activeModPath is unchanged", async () => {
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["A"]));

    bridge.fire("engine/state/changed", { activeModPath: "A" });
    await vi.waitFor(() => expect(bridge.request).toHaveBeenCalledTimes(2));

    bridge.request.mockClear();
    bridge.fire("engine/state/changed", { activeModPath: "A" });
    await new Promise((r) => setTimeout(r, 0));

    expect(bridge.request).not.toHaveBeenCalled();
  });

  it("requests mods/list once when activeModPath changes", async () => {
    const bridge = makeFakeBridge(["A"]);
    initModStack(bridge as never);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["A"]));

    bridge.fire("engine/state/changed", { activeModPath: "A" });
    await vi.waitFor(() => expect(bridge.request).toHaveBeenCalledTimes(2));

    bridge.request.mockClear();
    bridge.stackRef.current = ["B", "A"];
    bridge.fire("engine/state/changed", { activeModPath: "B" });

    expect(bridge.request).toHaveBeenCalledTimes(1);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["B", "A"]));
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

  // Same-front stack edits (reorder/remove/append of a secondary layer)
  // don't change activeModPath, so the broadcast gate won't fire —
  // mods/set-layers call sites use refreshModStack() to force the fetch.
  it("refreshModStack forces a mods/list fetch even when activeModPath is unchanged", async () => {
    const bridge = makeFakeBridge(["A", "B"]);
    initModStack(bridge as never);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["A", "B"]));

    bridge.request.mockClear();
    bridge.stackRef.current = ["A", "C"]; // front unchanged, tail edited
    refreshModStack();

    expect(bridge.request).toHaveBeenCalledTimes(1);
    await vi.waitFor(() => expect(useModStack.getState().stack).toEqual(["A", "C"]));
  });

  it("refreshModStack is a no-op after dispose", () => {
    const bridge = makeFakeBridge(["A"]);
    const off = initModStack(bridge as never);
    off();
    bridge.request.mockClear();
    expect(() => refreshModStack()).not.toThrow();
    expect(bridge.request).not.toHaveBeenCalled();
  });
});
