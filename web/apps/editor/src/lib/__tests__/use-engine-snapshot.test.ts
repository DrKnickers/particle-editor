// Pins the load-bearing contract of useEngineSnapshot (the extracted dropdown
// hook, DRY audit web-screens-0): seed from engine/state/snapshot, track
// engine/state/changed, and — critically — SWALLOW a rejected request (return
// null, never console.warn). The transitive dropdown tests always resolve
// `request`, so this branch was previously unguarded.
import { describe, it, expect, vi, afterEach } from "vitest";
import { renderHook, waitFor, act } from "@testing-library/react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { useEngineSnapshot } from "../use-engine-snapshot";

afterEach(() => vi.restoreAllMocks());

function bg(value: number): EngineStateDto {
  return { background: value } as unknown as EngineStateDto;
}

describe("useEngineSnapshot", () => {
  it("seeds from engine/state/snapshot, then tracks engine/state/changed", async () => {
    let changed: ((e: { payload: EngineStateDto }) => void) | null = null;
    const bridge = {
      request: vi.fn().mockResolvedValue(bg(0x111111)),
      on: vi.fn().mockImplementation((kind: string, cb: (e: { payload: EngineStateDto }) => void) => {
        if (kind === "engine/state/changed") changed = cb;
        return () => {};
      }),
    } as unknown as Bridge;

    const { result } = renderHook(() => useEngineSnapshot(bridge));
    await waitFor(() => expect(result.current).not.toBeNull());
    expect((result.current as { background: number }).background).toBe(0x111111);

    // A broadcast updates the snapshot in place.
    act(() => changed!({ payload: bg(0x222222) }));
    expect((result.current as { background: number }).background).toBe(0x222222);
  });

  it("swallows a rejected snapshot request: returns null, never console.warn (the contract)", async () => {
    const warn = vi.spyOn(console, "warn").mockImplementation(() => {});
    const bridge = {
      request: vi.fn().mockRejectedValue(new Error("boom")),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge;

    const { result } = renderHook(() => useEngineSnapshot(bridge));
    // Let the rejected promise settle.
    await act(async () => {
      await Promise.resolve();
    });
    expect(result.current).toBeNull();
    expect(warn).not.toHaveBeenCalled();
  });

  it("unsubscribes from engine/state/changed on unmount", () => {
    const off = vi.fn();
    const bridge = {
      request: vi.fn().mockResolvedValue(bg(0)),
      on: vi.fn().mockReturnValue(off),
    } as unknown as Bridge;

    const { unmount } = renderHook(() => useEngineSnapshot(bridge));
    unmount();
    expect(off).toHaveBeenCalledTimes(1);
  });
});
