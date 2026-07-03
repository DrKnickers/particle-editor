// Vitest tests for lib/file-state.ts — the Zustand atom mirroring the
// host's file state, the useSeedFileState event wiring, and the
// promptSaveChanges gate.
//
// Coverage:
//   1. useSeedFileState seeds from engine/state/snapshot + file/recent/list.
//   2. dirty/changed, recent/changed, engine/state/changed each update
//      the atom; engine/state/changed alone is enough (the fallback for
//      a dropped dirty/changed event).
//   3. Unmount cleans up: no live subscriptions, late snapshot resolution
//      is discarded (`cancelled` guard).
//   4. React StrictMode double-mount does not double-subscribe.
//   5. promptSaveChanges: clean → runs immediately; dirty → stored as
//      pendingAction and resolved via the SaveChangesPrompt's Save /
//      Don't Save / Cancel buttons. Cancel is the data-loss guard: the
//      pending destructive closure must NEVER run afterwards.
//
// Bridge double: a hand-rolled fake with a live handler registry (the
// same pattern as use-engine-snapshot.test.ts) so tests can emit host
// events and count active subscriptions per channel.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, renderHook, screen, fireEvent, waitFor, act } from "@testing-library/react";
import { StrictMode } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import {
  useFileStateStore,
  useSeedFileState,
  promptSaveChanges,
} from "../file-state";
import { SaveChangesPrompt } from "@/screens/SaveChangesPrompt";
import { useFileOpErrorStore } from "@/lib/file-op";

type FakeEvent = { kind: string; payload: unknown };
type Handler = (e: FakeEvent) => void;

function makeFakeBridge(opts?: {
  snapshot?: { currentFilePath: string | null; dirty: boolean };
  recents?: string[];
  /** When set, engine/state/snapshot never resolves on its own — call
   *  the returned `resolveSnapshot` to complete it. */
  deferSnapshot?: boolean;
}) {
  const snapshot = opts?.snapshot ?? { currentFilePath: null, dirty: false };
  const recents = opts?.recents ?? [];
  const handlers = new Map<string, Set<Handler>>();
  let resolveSnapshot: () => void = () => {};

  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        if (opts?.deferSnapshot) {
          return new Promise((resolve) => {
            resolveSnapshot = () => resolve(snapshot);
          });
        }
        return Promise.resolve(snapshot);
      }
      if (req.kind === "file/recent/list") {
        return Promise.resolve({ paths: recents });
      }
      // file/save (SaveChangesPrompt's Save path) and anything else.
      if (req.kind === "file/save") {
        return Promise.resolve({ ok: true, path: "C:/mock/saved.alo" });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, cb: Handler) => {
      let bucket = handlers.get(kind);
      if (!bucket) {
        bucket = new Set();
        handlers.set(kind, bucket);
      }
      bucket.add(cb);
      return () => bucket.delete(cb);
    }),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };

  const emit = (kind: string, payload: unknown) => {
    handlers.get(kind)?.forEach((h) => h({ kind, payload }));
  };
  const activeCount = (kind: string) => handlers.get(kind)?.size ?? 0;

  return { bridge, emit, activeCount, resolveSnapshot: () => resolveSnapshot() };
}

beforeEach(() => {
  useFileStateStore.setState({
    currentFilePath: null,
    dirty: false,
    recentFiles: [],
    pendingAction: null,
  });
  useFileOpErrorStore.setState({ message: null, title: null });
});

// ─── useSeedFileState — seeding ──────────────────────────────────────

describe("useSeedFileState — seeding", () => {
  it("populates currentFilePath + dirty from the snapshot and recentFiles from file/recent/list", async () => {
    const { bridge } = makeFakeBridge({
      snapshot: { currentFilePath: "C:/mods/fire.alo", dirty: true },
      recents: ["C:/mods/fire.alo", "C:/mods/smoke.alo"],
    });

    const { unmount } = renderHook(() => useSeedFileState(bridge));

    await waitFor(() => {
      expect(useFileStateStore.getState().currentFilePath).toBe("C:/mods/fire.alo");
    });
    expect(useFileStateStore.getState().dirty).toBe(true);
    await waitFor(() => {
      expect(useFileStateStore.getState().recentFiles).toEqual([
        "C:/mods/fire.alo",
        "C:/mods/smoke.alo",
      ]);
    });
    unmount();
  });

  it("a snapshot that resolves AFTER unmount is discarded (cancelled guard)", async () => {
    const { bridge, resolveSnapshot } = makeFakeBridge({
      snapshot: { currentFilePath: "C:/late.alo", dirty: true },
      deferSnapshot: true,
    });

    const { unmount } = renderHook(() => useSeedFileState(bridge));
    unmount();

    await act(async () => {
      resolveSnapshot();
      await Promise.resolve();
    });

    // The late resolution must not write into the atom.
    expect(useFileStateStore.getState().currentFilePath).toBeNull();
    expect(useFileStateStore.getState().dirty).toBe(false);
  });
});

// ─── useSeedFileState — event sync ───────────────────────────────────

describe("useSeedFileState — host events", () => {
  it("dirty/changed updates the dirty flag", async () => {
    const { bridge, emit } = makeFakeBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge));

    act(() => emit("dirty/changed", { dirty: true }));
    expect(useFileStateStore.getState().dirty).toBe(true);

    act(() => emit("dirty/changed", { dirty: false }));
    expect(useFileStateStore.getState().dirty).toBe(false);
    unmount();
  });

  it("recent/changed replaces the recent-files list", () => {
    const { bridge, emit } = makeFakeBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge));

    act(() => emit("recent/changed", { paths: ["C:/a.alo", "C:/b.alo"] }));
    expect(useFileStateStore.getState().recentFiles).toEqual([
      "C:/a.alo",
      "C:/b.alo",
    ]);
    unmount();
  });

  it("engine/state/changed alone updates currentFilePath + dirty (fallback for a dropped dirty/changed)", () => {
    const { bridge, emit } = makeFakeBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge));

    // No dedicated dirty/changed event fires — only the full snapshot
    // broadcast. The atom must still pick up both fields.
    act(() =>
      emit("engine/state/changed", {
        currentFilePath: "C:/mods/ion.alo",
        dirty: true,
      }),
    );
    expect(useFileStateStore.getState().currentFilePath).toBe("C:/mods/ion.alo");
    expect(useFileStateStore.getState().dirty).toBe(true);
    unmount();
  });

  it("unmount unsubscribes: post-unmount events don't touch the atom", () => {
    const { bridge, emit, activeCount } = makeFakeBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge));

    expect(activeCount("dirty/changed")).toBe(1);
    expect(activeCount("recent/changed")).toBe(1);
    expect(activeCount("engine/state/changed")).toBe(1);

    unmount();

    expect(activeCount("dirty/changed")).toBe(0);
    expect(activeCount("recent/changed")).toBe(0);
    expect(activeCount("engine/state/changed")).toBe(0);

    emit("dirty/changed", { dirty: true });
    expect(useFileStateStore.getState().dirty).toBe(false);
  });

  it("StrictMode double-mount ends with exactly ONE live subscription per channel", () => {
    const { bridge, emit, activeCount } = makeFakeBridge();
    const { unmount } = renderHook(() => useSeedFileState(bridge), {
      wrapper: StrictMode,
    });

    // Strict mode runs mount → cleanup → mount; a leaked first-mount
    // subscription would leave 2 here.
    expect(activeCount("dirty/changed")).toBe(1);
    expect(activeCount("recent/changed")).toBe(1);
    expect(activeCount("engine/state/changed")).toBe(1);

    // Events still land once (single handler on a shared store — no dupes).
    act(() => emit("recent/changed", { paths: ["C:/one.alo"] }));
    expect(useFileStateStore.getState().recentFiles).toEqual(["C:/one.alo"]);

    unmount();
    expect(activeCount("dirty/changed")).toBe(0);
    expect(activeCount("recent/changed")).toBe(0);
    expect(activeCount("engine/state/changed")).toBe(0);
  });
});

// ─── promptSaveChanges ───────────────────────────────────────────────

describe("promptSaveChanges", () => {
  it("clean document: runs the action immediately, never opens the prompt", () => {
    const action = vi.fn();
    promptSaveChanges(action);

    expect(action).toHaveBeenCalledTimes(1);
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });

  it("dirty document: stores the action as pendingAction without running it", () => {
    useFileStateStore.getState().setDirty(true);
    const action = vi.fn();
    promptSaveChanges(action);

    expect(action).not.toHaveBeenCalled();
    expect(useFileStateStore.getState().pendingAction).toBe(action);
  });

  it("dirty → Save: file/save succeeds, then the pending action runs and the slot clears", async () => {
    useFileStateStore.getState().setDirty(true);
    const { bridge } = makeFakeBridge();
    const action = vi.fn();
    promptSaveChanges(action);

    render(<SaveChangesPrompt bridge={bridge} />);
    fireEvent.click(screen.getByRole("button", { name: "Save" }));

    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "file/save", params: {} });
    });
    await waitFor(() => expect(action).toHaveBeenCalledTimes(1));
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });

  it("dirty → Don't Save: runs the pending action without dispatching file/save", async () => {
    useFileStateStore.getState().setDirty(true);
    const { bridge } = makeFakeBridge();
    const action = vi.fn();
    promptSaveChanges(action);

    render(<SaveChangesPrompt bridge={bridge} />);
    fireEvent.click(screen.getByRole("button", { name: "Don't Save" }));

    await waitFor(() => expect(action).toHaveBeenCalledTimes(1));
    expect(bridge.request).not.toHaveBeenCalledWith(
      expect.objectContaining({ kind: "file/save" }),
    );
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });

  it("dirty → Cancel: discards the pending closure — the action NEVER runs (data-loss guard)", async () => {
    useFileStateStore.getState().setDirty(true);
    const { bridge } = makeFakeBridge();
    const action = vi.fn();
    promptSaveChanges(action);

    render(<SaveChangesPrompt bridge={bridge} />);
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));

    expect(useFileStateStore.getState().pendingAction).toBeNull();
    expect(bridge.request).not.toHaveBeenCalledWith(
      expect.objectContaining({ kind: "file/save" }),
    );

    // Flush any queued microtasks: a deferred invocation of the discarded
    // closure would be a destructive-op-after-cancel bug.
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(action).not.toHaveBeenCalled();
  });
});
