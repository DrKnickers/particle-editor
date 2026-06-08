import { describe, it, expect, vi } from "vitest";
import { render } from "@testing-library/react";
import { useAppAccelerators } from "../use-app-accelerators";
import { useEmitterSelectionStore } from "../emitter-selection";

// A minimal fake bridge: captures the `accelerator/pressed` +
// `engine/state/changed` handlers and spies on `request`.
function makeFakeBridge() {
  const handlers: Record<string, (e: { payload: unknown }) => void> = {};
  const request = vi.fn((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") {
      return Promise.resolve({ ground: false, paused: false, heatDebug: false });
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

  it("Ctrl+Y → undo/perform redo ()", () => {
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

  it("Alt+Up → emitters/move up for the primary selection ()", () => {
    const b = makeFakeBridge();
    useEmitterSelectionStore.getState().setIds([7], 7);
    render(<Harness bridge={b} />);
    b.request.mockClear();
    b.fire("Alt+Up");
    expect(b.request).toHaveBeenCalledWith({ kind: "emitters/move", params: { id: 7, direction: "up" } });
    useEmitterSelectionStore.getState().clear();
  });
});
