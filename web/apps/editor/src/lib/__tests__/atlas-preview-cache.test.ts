import { describe, it, expect, beforeEach, vi } from "vitest";
import { getPreviewCached, invalidatePreviewCache, __resetPreviewCache } from "../atlas-preview-cache";

beforeEach(() => __resetPreviewCache());

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
});
