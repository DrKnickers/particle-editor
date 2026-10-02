import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { getPreviewCached, invalidatePreviewCache, __resetPreviewCache } from "../atlas-preview-cache";

beforeEach(() => __resetPreviewCache());
afterEach(() => vi.useRealTimers());

type PreviewReadyEvent = {
  kind: "textures/preview-ready";
  payload: { filename: string; flattenAlpha: boolean; status: "ok" | "broken" };
};

function makeReadyBridge() {
  const listeners = new Set<(event: PreviewReadyEvent) => void>();
  const bridge = {
    on: vi.fn((kind: "textures/preview-ready", handler: (event: PreviewReadyEvent) => void) => {
      expect(kind).toBe("textures/preview-ready");
      listeners.add(handler);
      return () => {
        listeners.delete(handler);
      };
    }),
  };
  return {
    bridge,
    emit(payload: PreviewReadyEvent["payload"]) {
      for (const listener of [...listeners]) {
        listener({ kind: "textures/preview-ready", payload });
      }
    },
    listenerCount() {
      return listeners.size;
    },
  };
}

describe("atlas preview cache (stack + filename key)", () => {
  it("fetches once per (stack, filename)", async () => {
    const f = vi.fn().mockResolvedValue({ status: "ok", dataUri: "x", srcW: 4, srcH: 4 });
    await getPreviewCached(["A", "B"], "fire.dds", f);
    await getPreviewCached(["A", "B"], "fire.dds", f);
    expect(f).toHaveBeenCalledTimes(1);
  });

  it("refetches when the stack changes", async () => {
    const f = vi.fn().mockResolvedValue({ status: "ok", dataUri: "x", srcW: 4, srcH: 4 });
    await getPreviewCached(["A", "B"], "fire.dds", f);
    await getPreviewCached(["B", "A"], "fire.dds", f);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("does not cache a non-ok result", async () => {
    const f = vi
      .fn()
      .mockResolvedValueOnce({ status: "missing" })
      .mockResolvedValueOnce({ status: "ok", dataUri: "x", srcW: 4, srcH: 4 });
    await getPreviewCached(["A"], "x.dds", f);
    await getPreviewCached(["A"], "x.dds", f);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("invalidate clears everything", async () => {
    const f = vi.fn().mockResolvedValue({ status: "ok", dataUri: "x", srcW: 4, srcH: 4 });
    await getPreviewCached(["A"], "x.dds", f);
    invalidatePreviewCache();
    await getPreviewCached(["A"], "x.dds", f);
    expect(f).toHaveBeenCalledTimes(2);
  });

  it("waits for preview-ready after a pending response and refetches once", async () => {
    const ready = makeReadyBridge();
    const f = vi
      .fn()
      .mockResolvedValueOnce({ status: "pending" })
      .mockResolvedValueOnce({ status: "ok", dataUri: "refetched", srcW: 8, srcH: 8 });

    const result = getPreviewCached(["A"], "flat::fire.dds", f, {
      bridge: ready.bridge as never,
      filename: "fire.dds",
      flattenAlpha: true,
      timeoutMs: 1_000,
    });
    await Promise.resolve();

    expect(f).toHaveBeenCalledTimes(1);
    expect(ready.bridge.on).toHaveBeenCalledTimes(1);

    ready.emit({ filename: "fire.dds", flattenAlpha: true, status: "ok" });

    await expect(result).resolves.toEqual({ status: "ok", dataUri: "refetched", srcW: 8, srcH: 8 });
    expect(f).toHaveBeenCalledTimes(2);
    expect(ready.listenerCount()).toBe(0);
  });

  it("shares one pending listener and refetch across concurrent waiters for the same key", async () => {
    const ready = makeReadyBridge();
    const f = vi
      .fn()
      .mockResolvedValueOnce({ status: "pending" })
      .mockResolvedValueOnce({ status: "ok", dataUri: "shared", srcW: 16, srcH: 16 });
    const pendingOptions = {
      bridge: ready.bridge as never,
      filename: "fire.dds",
      flattenAlpha: true,
      timeoutMs: 1_000,
    };

    const first = getPreviewCached(["A"], "flat::fire.dds", f, pendingOptions);
    await Promise.resolve();
    const second = getPreviewCached(["A"], "flat::fire.dds", f, pendingOptions);

    expect(f).toHaveBeenCalledTimes(1);
    expect(ready.bridge.on).toHaveBeenCalledTimes(1);
    expect(ready.listenerCount()).toBe(1);

    ready.emit({ filename: "fire.dds", flattenAlpha: true, status: "ok" });

    await expect(Promise.all([first, second])).resolves.toEqual([
      { status: "ok", dataUri: "shared", srcW: 16, srcH: 16 },
      { status: "ok", dataUri: "shared", srcW: 16, srcH: 16 },
    ]);
    expect(f).toHaveBeenCalledTimes(2);
    expect(ready.listenerCount()).toBe(0);
  });

  it("re-arms instead of resolving pending when a timeout refetch is still encoding", async () => {
    // A wait must resolve only on a TERMINAL status. If the timeout-path
    // refetch still returns pending (slow encode), keep listening — a later
    // preview-ready must still resolve it, not leave the panel loading.
    vi.useFakeTimers();
    const ready = makeReadyBridge();
    const f = vi
      .fn()
      .mockResolvedValueOnce({ status: "pending" }) // initial
      .mockResolvedValueOnce({ status: "pending" }) // timeout refetch: still encoding
      .mockResolvedValueOnce({ status: "ok", dataUri: "late", srcW: 32, srcH: 32 }); // after re-armed event

    const result = getPreviewCached(["A"], "flat::fire.dds", f, {
      bridge: ready.bridge as never,
      filename: "fire.dds",
      flattenAlpha: true,
      timeoutMs: 50,
    });
    await Promise.resolve();
    expect(f).toHaveBeenCalledTimes(1);

    // Timeout fires → refetch still pending → re-arm (listener alive again).
    await vi.advanceTimersByTimeAsync(50);
    expect(f).toHaveBeenCalledTimes(2);
    expect(ready.listenerCount()).toBe(1);

    // The real preview-ready now arrives → terminal refetch resolves it.
    ready.emit({ filename: "fire.dds", flattenAlpha: true, status: "ok" });
    await expect(result).resolves.toEqual({ status: "ok", dataUri: "late", srcW: 32, srcH: 32 });
    expect(f).toHaveBeenCalledTimes(3);
    expect(ready.listenerCount()).toBe(0);
  });

  it("falls back to one refetch when preview-ready is not delivered before timeout", async () => {
    vi.useFakeTimers();
    const ready = makeReadyBridge();
    const f = vi
      .fn()
      .mockResolvedValueOnce({ status: "pending" })
      .mockResolvedValueOnce({ status: "ok", dataUri: "timeout", srcW: 32, srcH: 32 });

    const result = getPreviewCached(["A"], "flat::fire.dds", f, {
      bridge: ready.bridge as never,
      filename: "fire.dds",
      flattenAlpha: true,
      timeoutMs: 50,
    });
    await Promise.resolve();

    expect(f).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(50);

    await expect(result).resolves.toEqual({ status: "ok", dataUri: "timeout", srcW: 32, srcH: 32 });
    expect(f).toHaveBeenCalledTimes(2);
    expect(ready.listenerCount()).toBe(0);
  });
});
