import { describe, it, expect, vi, afterEach } from "vitest";
import { render } from "@testing-library/react";
import { useAppAccelerators } from "../use-app-accelerators";
import { useEmitterSelectionStore } from "../emitter-selection";
import { useFileStateStore } from "../file-state";
import { RESET_CAMERA } from "../reset-camera";
import { useTextureEpoch, __resetPreviewCache } from "../atlas-preview-cache";
import { __resetRightDockForTests, useRightDockStoreForTests } from "../right-dock";

// A minimal fake bridge: captures the `accelerator/pressed` +
// `engine/state/changed` handlers and spies on `request`.
function makeFakeBridge() {
  const handlers: Record<string, (e: { payload: unknown }) => void> = {};
  const request = vi.fn((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") {
      return Promise.resolve({ ground: false, paused: false, heatDebug: false });
    }
    if (req.kind === "file/open") {
      // Cancelled native picker — keeps runFileOp's error modal + emitters/list
      // follow-up out of these dispatch-focused tests.
      return Promise.resolve({ ok: false, error: "user-cancelled" });
    }
    return Promise.resolve({});
  });
  return {
    request,
    on: (kind: string, cb: (e: { payload: unknown }) => void) => {
      handlers[kind] = cb;
      return () => {
        delete handlers[kind];
      };
    },
    fire: (combo: string) => handlers["accelerator/pressed"]?.({ payload: { combo } }),
    emitState: (s: unknown) => handlers["engine/state/changed"]?.({ payload: s }),
  };
}

function Harness({ bridge }: { bridge: ReturnType<typeof makeFakeBridge> }) {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  useAppAccelerators(bridge as any);
  return null;
}

const flush = () => Promise.resolve().then(() => Promise.resolve());

describe("useAppAccelerators", () => {
  it("registers the legacy combos (incl. Ctrl+Y / Alt+Up) but not bare Delete/F2", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    const reg = b.request.mock.calls
      .map(([r]) => r as { kind: string; params: { combos: string[] } })
      .find((r) => r.kind === "register-accelerators");
    expect(reg).toBeTruthy();
    const combos = reg!.params.combos;
    for (const c of ["Ctrl+S", "Ctrl+N", "Ctrl+O", "Ctrl+Y", "Alt+Up", "Alt+Down", "F7", "F8", "Ctrl+Space"]) {
      expect(combos).toContain(c);
    }
    expect(combos).not.toContain("Delete");
    expect(combos).not.toContain("F2");
  });

  it("Ctrl+S → file/save", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Ctrl+S");
    expect(b.request).toHaveBeenCalledWith({ kind: "file/save", params: {} });
  });

  it("Ctrl+S whose file/save REJECTS is caught — no unhandled rejection (#489)", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const unhandled = vi.fn();
    process.on("unhandledRejection", unhandled);

    const b = makeFakeBridge();
    // Transport-level failure (bridge not ready / dead pipe): runFileOp
    // surfaces it in the error modal and then RE-THROWS. Before the fix the
    // fire-and-forget `void runFileOp(...)` let that reject escape unhandled.
    b.request.mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/save") return Promise.reject(new Error("transport dead"));
      if (req.kind === "engine/state/snapshot")
        return Promise.resolve({ ground: false, paused: false, heatDebug: false });
      return Promise.resolve({});
    });
    render(<Harness bridge={b} />);
    b.fire("Ctrl+S");
    await flush();
    await flush();

    process.off("unhandledRejection", unhandled);
    expect(unhandled).not.toHaveBeenCalled();
    expect(warn).toHaveBeenCalledWith("[accel] Ctrl+S save failed:", expect.any(Error));
    warn.mockRestore();
  });

  it("Ctrl+Y → undo/perform redo", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Ctrl+Y");
    expect(b.request).toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "redo" } });
  });

  it("F9 / F10 → step-frames 1 / 10", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("F9");
    b.fire("F10");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/step-frames", params: { frames: 1 } });
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/step-frames", params: { frames: 10 } });
  });

  it("F8 toggles paused from live engine state", async () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    await flush(); // snapshot resolves paused:false
    b.request.mockClear();
    b.fire("F8");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/paused", params: { paused: true } });
    b.emitState({ ground: false, paused: true, heatDebug: false });
    b.request.mockClear();
    b.fire("F8");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/paused", params: { paused: false } });
  });

  it("Alt+Up → emitters/move-many up for the selection", () => {
    const b = makeFakeBridge();
    useEmitterSelectionStore.getState().setIds([7], 7);
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Alt+Up");
    expect(b.request).toHaveBeenCalledWith({ kind: "emitters/move-many", params: { ids: [7], direction: "up" } });
    useEmitterSelectionStore.getState().clear();
  });
});

describe("useAppAccelerators — uncovered dispatch + guard paths", () => {
  afterEach(() => {
    useEmitterSelectionStore.getState().clear();
    useFileStateStore.setState({ dirty: false, pendingAction: null });
    __resetPreviewCache();
    localStorage.clear();
    __resetRightDockForTests();
    vi.restoreAllMocks();
  });

  it("an unknown combo is ignored without dispatching or throwing", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    expect(() => b.fire("Ctrl+Q")).not.toThrow();
    expect(b.request).not.toHaveBeenCalled();
  });

  it("unmount unsubscribes — a combo fired afterwards dispatches nothing", () => {
    const b = makeFakeBridge();
    const { unmount } = render(<Harness bridge={b} />);
    unmount();
    b.request.mockClear();
    b.fire("Ctrl+S");
    b.fire("F9");
    expect(b.request).not.toHaveBeenCalled();
  });

  it("Ctrl+Del / Ctrl+Z / Ctrl+Shift+Z / Ctrl+Space / F6 dispatch their bridge commands", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Ctrl+Del");
    b.fire("Ctrl+Z");
    b.fire("Ctrl+Shift+Z");
    b.fire("Ctrl+Space");
    b.fire("F6");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/clear", params: {} });
    expect(b.request).toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "undo" } });
    expect(b.request).toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "redo" } });
    expect(b.request).toHaveBeenCalledWith({ kind: "spawner/trigger", params: {} });
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/reload-shaders", params: {} });
  });

  it("Alt+Down → emitters/move-many down; empty selection dispatches nothing", () => {
    const b = makeFakeBridge();
    useEmitterSelectionStore.getState().setIds([3, 4], 3);
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Alt+Down");
    expect(b.request).toHaveBeenCalledWith({
      kind: "emitters/move-many",
      params: { ids: [3, 4], direction: "down" },
    });
    // Guard: with no selection, moveEmitters early-returns.
    useEmitterSelectionStore.getState().clear();
    b.request.mockClear();
    b.fire("Alt+Down");
    expect(b.request).not.toHaveBeenCalled();
  });

  it("Ctrl+G / Ctrl+H toggle ground / heat-debug from live engine state (null state → enable)", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    // BEFORE the snapshot resolves, stateRef is null → both default to enabling.
    b.request.mockClear();
    b.fire("Ctrl+G");
    b.fire("Ctrl+H");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/ground", params: { enabled: true } });
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/heat-debug", params: { enabled: true } });
    // Live state flips the toggles.
    b.emitState({ ground: true, paused: false, heatDebug: true });
    b.request.mockClear();
    b.fire("Ctrl+G");
    b.fire("Ctrl+H");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/ground", params: { enabled: false } });
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/heat-debug", params: { enabled: false } });
  });

  it("Ctrl+L is guarded: no reference object → NO dispatch; loaded object → lock toggles", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    // No reference object loaded (stateRef null, then explicit empty name).
    b.request.mockClear();
    b.fire("Ctrl+L");
    b.emitState({ referenceObjectName: "", referenceObjectLocked: false });
    b.fire("Ctrl+L");
    expect(b.request).not.toHaveBeenCalled();
    // Loaded + unlocked → lock.
    b.emitState({ referenceObjectName: "AT_AT_Walker", referenceObjectLocked: false });
    b.fire("Ctrl+L");
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/reference-object-lock",
      params: { locked: true },
    });
    // Loaded + locked → unlock.
    b.emitState({ referenceObjectName: "AT_AT_Walker", referenceObjectLocked: true });
    b.request.mockClear();
    b.fire("Ctrl+L");
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/reference-object-lock",
      params: { locked: false },
    });
  });

  it("F7 toggles the spawner right-dock (no bridge dispatch)", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    const dock = useRightDockStoreForTests();
    expect(dock.getState().dock).toBe("spawner"); // default
    b.request.mockClear();
    b.fire("F7");
    expect(dock.getState().dock).toBeNull();
    b.fire("F7");
    expect(dock.getState().dock).toBe("spawner");
    expect(b.request).not.toHaveBeenCalled();
  });

  it("Ctrl+Home → engine/set/camera with the shared RESET_CAMERA vectors", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Ctrl+Home");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/camera", params: RESET_CAMERA });
  });

  it("F5 → reload-textures, then bumps the texture epoch once the reload resolves", async () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    const before = useTextureEpoch.getState().epoch;
    b.request.mockClear();
    b.fire("F5");
    expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/reload-textures", params: {} });
    expect(useTextureEpoch.getState().epoch).toBe(before); // not yet — waits for the host ack
    await flush();
    expect(useTextureEpoch.getState().epoch).toBe(before + 1);
  });

  it("Ctrl+N runs file/new immediately when clean, but parks it behind the save prompt when dirty", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    // Clean → immediate.
    useFileStateStore.setState({ dirty: false, pendingAction: null });
    b.request.mockClear();
    b.fire("Ctrl+N");
    expect(b.request).toHaveBeenCalledWith({ kind: "file/new", params: {} });
    // Dirty → gated: no dispatch, the action is parked as the prompt's closure.
    useFileStateStore.setState({ dirty: true, pendingAction: null });
    b.request.mockClear();
    b.fire("Ctrl+N");
    expect(b.request).not.toHaveBeenCalled();
    const pending = useFileStateStore.getState().pendingAction;
    expect(pending).toBeTypeOf("function");
    // Running the parked closure (what Save / Don't Save does) issues file/new.
    void pending!();
    expect(b.request).toHaveBeenCalledWith({ kind: "file/new", params: {} });
  });

  it("Ctrl+O routes through the save-changes gate and runFileOp", () => {
    const b = makeFakeBridge();
    render(<Harness bridge={b} />);
    useFileStateStore.setState({ dirty: false, pendingAction: null });
    b.request.mockClear();
    b.fire("Ctrl+O");
    expect(b.request).toHaveBeenCalledWith({ kind: "file/open", params: {} });
    // Dirty → parked, not dispatched.
    useFileStateStore.setState({ dirty: true, pendingAction: null });
    b.request.mockClear();
    b.fire("Ctrl+O");
    expect(b.request).not.toHaveBeenCalled();
    expect(useFileStateStore.getState().pendingAction).toBeTypeOf("function");
  });

  it("a failed register-accelerators is warned about, not thrown, and events still dispatch", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const handlers: Record<string, (e: { payload: unknown }) => void> = {};
    const request = vi.fn((req: { kind: string }) => {
      if (req.kind === "register-accelerators") return Promise.reject(new Error("host offline"));
      if (req.kind === "engine/state/snapshot") return Promise.reject(new Error("host offline"));
      return Promise.resolve({});
    });
    const bridge = {
      request,
      on: (kind: string, cb: (e: { payload: unknown }) => void) => {
        handlers[kind] = cb;
        return () => {
          delete handlers[kind];
        };
      },
    };
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    render(<Harness bridge={bridge as any} />);
    await flush();
    expect(warn).toHaveBeenCalledWith(
      "[accel] register-accelerators failed:",
      expect.objectContaining({ message: "host offline" }),
    );
    // The snapshot rejection is swallowed silently, and the handler still works.
    request.mockClear();
    handlers["accelerator/pressed"]?.({ payload: { combo: "Ctrl+Del" } });
    expect(request).toHaveBeenCalledWith({ kind: "engine/action/clear", params: {} });
  });
});
