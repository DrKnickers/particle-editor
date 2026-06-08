// Tests for useBackingColorSync (session 3). Verifies the hook
// pushes the resolved --bg to the host on mount and on every data-theme
// change, and skips the push when --bg is unresolved.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { renderHook } from "@testing-library/react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useBackingColorSync } from "../backing-color-sync";

function makeBridge() {
  return { request: vi.fn().mockResolvedValue({}) } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
  };
}

// MutationObserver callbacks fire on a microtask; flush with a macrotask.
const flush = () => new Promise((r) => setTimeout(r, 0));

describe("useBackingColorSync", () => {
  beforeEach(() => {
    document.documentElement.removeAttribute("data-theme");
  });
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("pushes the resolved --bg on mount", () => {
    vi.spyOn(window, "getComputedStyle").mockReturnValue({
      getPropertyValue: (p: string) => (p === "--bg" ? " #111111 " : ""),
    } as unknown as CSSStyleDeclaration);

    const bridge = makeBridge();
    renderHook(() => useBackingColorSync(bridge));

    expect(bridge.request).toHaveBeenCalledTimes(1);
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "host/backing-color",
      params: { color: "#111111" },
    });
  });

  it("re-pushes the new --bg when data-theme changes", async () => {
    let cur = "#111111";
    vi.spyOn(window, "getComputedStyle").mockImplementation(
      () =>
        ({
          getPropertyValue: (p: string) => (p === "--bg" ? cur : ""),
        }) as unknown as CSSStyleDeclaration,
    );

    const bridge = makeBridge();
    renderHook(() => useBackingColorSync(bridge));
    expect(bridge.request).toHaveBeenCalledTimes(1);

    cur = "#ececec";
    document.documentElement.dataset.theme = "light";
    await flush();

    expect(bridge.request).toHaveBeenCalledTimes(2);
    expect(bridge.request).toHaveBeenLastCalledWith({
      kind: "host/backing-color",
      params: { color: "#ececec" },
    });
  });

  it("skips the push when --bg is unresolved (empty)", () => {
    vi.spyOn(window, "getComputedStyle").mockReturnValue({
      getPropertyValue: () => "",
    } as unknown as CSSStyleDeclaration);

    const bridge = makeBridge();
    renderHook(() => useBackingColorSync(bridge));

    expect(bridge.request).not.toHaveBeenCalled();
  });
});
