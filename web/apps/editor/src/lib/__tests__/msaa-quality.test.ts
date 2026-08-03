import { describe, it, expect, vi } from "vitest";
import {
  MSAA_QUALITY_CHANGED_EVENT,
  readMsaaLevel,
  writeMsaaLevel,
  applyMsaaLevel,
  queryMsaaLevels,
  type MsaaLevel,
} from "../msaa-quality";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("msaa-quality", () => {
  it("defaults to 4 when localStorage is empty", () => {
    expect(readMsaaLevel()).toBe(4);
  });

  it("round-trips a written level", () => {
    writeMsaaLevel(2);
    expect(readMsaaLevel()).toBe(2);

    writeMsaaLevel(0);
    expect(readMsaaLevel()).toBe(0);

    writeMsaaLevel(8);
    expect(readMsaaLevel()).toBe(8);
  });

  it("returns default (4) for an invalid stored value", () => {
    localStorage.setItem("alo:msaa-quality", "7");
    expect(readMsaaLevel()).toBe(4);

    localStorage.setItem("alo:msaa-quality", "not-a-number");
    expect(readMsaaLevel()).toBe(4);

    localStorage.setItem("alo:msaa-quality", "");
    expect(readMsaaLevel()).toBe(4);
  });

  it("survives corrupt / missing localStorage with the default", () => {
    localStorage.removeItem("alo:msaa-quality");
    expect(readMsaaLevel()).toBe(4);
  });

  it("writeMsaaLevel dispatches the change event", () => {
    const seen = vi.fn();
    window.addEventListener(MSAA_QUALITY_CHANGED_EVENT, seen);
    writeMsaaLevel(2 as MsaaLevel);
    expect(seen).toHaveBeenCalledTimes(1);
    window.removeEventListener(MSAA_QUALITY_CHANGED_EVENT, seen);
  });

  it("applyMsaaLevel sends the level over the bridge, fire-and-forget", () => {
    const request = vi.fn().mockResolvedValue({});
    applyMsaaLevel({ request } as unknown as Bridge, 4);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: { level: 4 },
    });
  });

  it("applyMsaaLevel is fire-and-forget: a rejection is swallowed", async () => {
    const request = vi.fn().mockRejectedValue(new Error("bridge down"));
    // Should not throw
    await expect(
      Promise.resolve().then(() => applyMsaaLevel({ request } as unknown as Bridge, 0)),
    ).resolves.toBeUndefined();
  });

  it("queryMsaaLevels returns the bridge response on success", async () => {
    const request = vi.fn().mockResolvedValue({ levels: [0, 2, 4], current: 4 });
    const result = await queryMsaaLevels({ request } as unknown as Bridge);
    expect(result).toEqual({ levels: [0, 2, 4], current: 4 });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/query/msaa-levels",
      params: {},
    });
  });

  it("queryMsaaLevels returns the unknown fallback { levels: [], current: -1 } on error", async () => {
    const request = vi.fn().mockRejectedValue(new Error("unavailable"));
    const result = await queryMsaaLevels({ request } as unknown as Bridge);
    expect(result).toEqual({ levels: [], current: -1 });
  });

  it("queryMsaaLevels returns the unknown fallback { levels: [], current: -1 } on a malformed response", async () => {
    const request = vi.fn().mockResolvedValue({});
    const result = await queryMsaaLevels({ request } as unknown as Bridge);
    expect(result).toEqual({ levels: [], current: -1 });
  });
});
