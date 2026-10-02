// Pins the Paste gate's host-follow: the emitter clipboard lives in the
// host and outlives the page, so after a reload (WebView2 crash recovery,
// ErrorBoundary Reload) useSeedEmitterClipboard must turn the page-local
// flag back on from the snapshot's emitterClipboardHasContent — without
// any copy on the new page — and must never turn it off.
import { describe, it, expect, beforeEach, vi } from "vitest";
import { renderHook, waitFor, act } from "@testing-library/react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { MockBridge } from "@/bridge/mock";
import { resetMockState } from "@/test/mock-state";
import {
  markEmittersCopied,
  useEmitterClipboardStore,
  useSeedEmitterClipboard,
} from "../emitter-clipboard";

function state(emitterClipboardHasContent: boolean): EngineStateDto {
  return { emitterClipboardHasContent } as unknown as EngineStateDto;
}

/** Bridge double: resolves the snapshot with `initial` and hands back the
 *  engine/state/changed listener so tests can broadcast. */
function fakeBridge(initial: boolean) {
  let changed: ((e: { payload: EngineStateDto }) => void) | null = null;
  const bridge = {
    request: vi.fn().mockResolvedValue(state(initial)),
    on: vi.fn().mockImplementation((kind: string, cb: (e: { payload: EngineStateDto }) => void) => {
      if (kind === "engine/state/changed") changed = cb;
      return () => {};
    }),
  } as unknown as Bridge;
  return { bridge, broadcast: (v: boolean) => changed!({ payload: state(v) }) };
}

beforeEach(() => {
  resetMockState();
  useEmitterClipboardStore.setState({ hasContent: false });
});

describe("useSeedEmitterClipboard", () => {
  it("turns Paste on after a reload when the host still holds copied emitters", async () => {
    // Host side: copied before the reload. Page side: a fresh store.
    const bridge = new MockBridge();
    await bridge.request({ kind: "emitters/copy", params: { ids: [0] } });
    expect(useEmitterClipboardStore.getState().hasContent).toBe(false);

    renderHook(() => useSeedEmitterClipboard(bridge));
    await waitFor(() => expect(useEmitterClipboardStore.getState().hasContent).toBe(true));
  });

  it("leaves Paste off while the host clipboard is empty", async () => {
    const { bridge } = fakeBridge(false);
    renderHook(() => useSeedEmitterClipboard(bridge));
    await act(async () => {
      await Promise.resolve();
    });
    expect(bridge.request).toHaveBeenCalled();
    expect(useEmitterClipboardStore.getState().hasContent).toBe(false);
  });

  it("follows engine/state/changed when the host reports content", async () => {
    const { bridge, broadcast } = fakeBridge(false);
    renderHook(() => useSeedEmitterClipboard(bridge));
    act(() => broadcast(true));
    expect(useEmitterClipboardStore.getState().hasContent).toBe(true);
  });

  it("never clears a flag a copy already set (a stale false snapshot is ignored)", async () => {
    const { bridge, broadcast } = fakeBridge(false);
    markEmittersCopied();
    renderHook(() => useSeedEmitterClipboard(bridge));
    await act(async () => {
      await Promise.resolve();
    });
    act(() => broadcast(false));
    expect(useEmitterClipboardStore.getState().hasContent).toBe(true);
  });
});
