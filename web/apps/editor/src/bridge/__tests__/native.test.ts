// NativeBridge request-lifecycle tests (audit G12) — the pending-request
// map must not leak a forever-pending promise when postMessage throws or
// the page tears down, and an opt-in timeout reclaims a dropped response.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { NativeBridge } from "../native";

type PostMessage = (s: string) => void;

// Install a minimal window.chrome.webview so the NativeBridge ctor succeeds.
// `postMessage` is swapped per-test.
function installWebview(postMessage: PostMessage) {
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      postMessage,
      addEventListener: vi.fn(),
    },
  };
}

function pendingSize(b: NativeBridge): number {
  return (b as unknown as { pending: Map<unknown, unknown> }).pending.size;
}

beforeEach(() => {
  installWebview(() => {});
});

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
  vi.useRealTimers();
});

describe("NativeBridge G12 — request lifecycle does not leak pending entries", () => {
  it("rejects and removes the pending entry when postMessage throws", async () => {
    installWebview(() => {
      throw new Error("postMessage boom");
    });
    const b = new NativeBridge();
    await expect(
      b.request({ kind: "emitters/list", params: {} } as never),
    ).rejects.toThrow(/boom/);
    // The entry registered before the throw must be cleaned up — no leak.
    expect(pendingSize(b)).toBe(0);
    // Still usable: a second request also rejects cleanly (processed, not
    // hung) and leaves no leaked entry.
    await expect(
      b.request({ kind: "emitters/list", params: {} } as never),
    ).rejects.toThrow(/boom/);
    expect(pendingSize(b)).toBe(0);
  });

  it("dispose() rejects every outstanding request and clears the map", async () => {
    const b = new NativeBridge();
    const p1 = b.request({ kind: "emitters/list", params: {} } as never);
    const p2 = b.request({ kind: "engine/state/snapshot", params: {} } as never);
    expect(pendingSize(b)).toBe(2);

    b.dispose();

    await expect(p1).rejects.toThrow(/disposed|disconnect/i);
    await expect(p2).rejects.toThrow(/disposed|disconnect/i);
    expect(pendingSize(b)).toBe(0);
    // A request after dispose rejects immediately (fails closed).
    await expect(b.request({ kind: "emitters/list", params: {} } as never)).rejects.toThrow(
      /disposed|disconnect/i,
    );
  });

  it("opt-in timeout rejects + removes a request whose response never arrives", async () => {
    vi.useFakeTimers();
    const b = new NativeBridge({ requestTimeoutMs: 5_000 });
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    expect(pendingSize(b)).toBe(1);
    vi.advanceTimersByTime(5_001);
    await expect(p).rejects.toThrow(/timed out/i);
    expect(pendingSize(b)).toBe(0);
  });

  it("default (no opts) installs NO timeout — interactive requests can block indefinitely", () => {
    vi.useFakeTimers();
    const b = new NativeBridge();
    void b.request({ kind: "file/open", params: {} } as never);
    // No timer scheduled → advancing time does not reject/clear it.
    vi.advanceTimersByTime(10 * 60_000);
    expect(pendingSize(b)).toBe(1);
  });
});
