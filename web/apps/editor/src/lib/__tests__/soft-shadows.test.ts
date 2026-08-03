import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  readSoftShadows,
  writeSoftShadows,
  applySoftShadows,
} from "@/lib/soft-shadows";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("soft-shadows preference", () => {
  beforeEach(() => localStorage.clear());

  it("defaults to ON when unset", () => {
    expect(readSoftShadows()).toBe(true);
  });

  it("round-trips true/false through localStorage", () => {
    writeSoftShadows(false);
    expect(readSoftShadows()).toBe(false);
    writeSoftShadows(true);
    expect(readSoftShadows()).toBe(true);
  });

  it("accepts the legacy 'true'/'1' encodings as ON", () => {
    localStorage.setItem("alo:soft-shadows", "true");
    expect(readSoftShadows()).toBe(true);
    localStorage.setItem("alo:soft-shadows", "1");
    expect(readSoftShadows()).toBe(true);
    localStorage.setItem("alo:soft-shadows", "0");
    expect(readSoftShadows()).toBe(false);
  });

  it("applySoftShadows sends engine/set/soft-shadows with the flag", () => {
    const request = vi.fn().mockResolvedValue({});
    const bridge = { request } as unknown as Bridge;
    applySoftShadows(bridge, false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/soft-shadows",
      params: { enabled: false },
    });
  });

  it("applySoftShadows sends engine/set/soft-shadows with enabled:true", () => {
    const request = vi.fn().mockResolvedValue({});
    const bridge = { request } as unknown as Bridge;
    applySoftShadows(bridge, true);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/soft-shadows",
      params: { enabled: true },
    });
  });

  it("applySoftShadows swallows a rejected request (fire-and-forget)", async () => {
    const request = vi.fn().mockRejectedValue(new Error("host gone"));
    const bridge = { request } as unknown as Bridge;
    expect(() => applySoftShadows(bridge, true)).not.toThrow();
    await Promise.resolve();
  });
});
