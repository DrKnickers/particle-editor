import { afterEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, render, screen, waitFor } from "@testing-library/react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { useEngineField } from "../use-engine-snapshot";

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

function engineState(overrides: Partial<EngineStateDto>): EngineStateDto {
  return {
    paused: false,
    canUndo: false,
    background: 0,
    ...overrides,
  } as EngineStateDto;
}

function makeBridge(initial: EngineStateDto) {
  let changed: ((e: { kind: "engine/state/changed"; payload: EngineStateDto }) => void) | null = null;
  const off = vi.fn();
  const bridge = {
    request: vi.fn().mockResolvedValue(initial),
    on: vi.fn((kind: string, cb: (e: { kind: "engine/state/changed"; payload: EngineStateDto }) => void) => {
      if (kind === "engine/state/changed") changed = cb;
      return off;
    }),
  } as unknown as Bridge;

  return {
    bridge,
    emit(payload: EngineStateDto) {
      act(() => {
        changed?.({ kind: "engine/state/changed", payload });
      });
    },
  };
}

describe("useEngineField", () => {
  it("does not re-render when an unrelated engine field changes", async () => {
    const { bridge, emit } = makeBridge(engineState({ paused: false, canUndo: false }));
    let renders = 0;

    function Probe() {
      renders += 1;
      const paused = useEngineField(bridge, (s) => s.paused);
      return <div data-testid="paused">{String(paused)}</div>;
    }

    render(<Probe />);
    await waitFor(() => expect(screen.getByTestId("paused")).toHaveTextContent("false"));

    renders = 0;
    emit(engineState({ paused: false, canUndo: true }));

    expect(screen.getByTestId("paused")).toHaveTextContent("false");
    expect(renders).toBe(0);
  });

  it("re-renders when the selected engine field changes", async () => {
    const { bridge, emit } = makeBridge(engineState({ paused: false }));
    let renders = 0;

    function Probe() {
      renders += 1;
      const paused = useEngineField(bridge, (s) => s.paused);
      return <div data-testid="paused">{String(paused)}</div>;
    }

    render(<Probe />);
    await waitFor(() => expect(screen.getByTestId("paused")).toHaveTextContent("false"));

    renders = 0;
    emit(engineState({ paused: true }));

    await waitFor(() => expect(screen.getByTestId("paused")).toHaveTextContent("true"));
    expect(renders).toBe(1);
  });
});
