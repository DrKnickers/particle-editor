import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  readSkydomeSeamFix,
  writeSkydomeSeamFix,
  applySkydomeSeamFix,
} from "@/lib/skydome-seam-fix";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("skydome-seam-fix preference", () => {
  beforeEach(() => localStorage.clear());

  it("defaults to ON when unset", () => {
    expect(readSkydomeSeamFix()).toBe(true);
  });

  it("round-trips true/false through localStorage", () => {
    writeSkydomeSeamFix(false);
    expect(readSkydomeSeamFix()).toBe(false);
    writeSkydomeSeamFix(true);
    expect(readSkydomeSeamFix()).toBe(true);
  });

  it("accepts the legacy 'true'/'1' encodings as ON", () => {
    localStorage.setItem("alo:skydome-seam-fix", "true");
    expect(readSkydomeSeamFix()).toBe(true);
    localStorage.setItem("alo:skydome-seam-fix", "1");
    expect(readSkydomeSeamFix()).toBe(true);
    localStorage.setItem("alo:skydome-seam-fix", "0");
    expect(readSkydomeSeamFix()).toBe(false);
  });

  it("applySkydomeSeamFix sends engine/set/skydome-seam-fix with the flag", () => {
    const request = vi.fn().mockResolvedValue({});
    const bridge = { request } as unknown as Bridge;
    applySkydomeSeamFix(bridge, false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/skydome-seam-fix",
      params: { enabled: false },
    });
  });

  it("applySkydomeSeamFix swallows a rejected request (fire-and-forget)", async () => {
    const request = vi.fn().mockRejectedValue(new Error("host gone"));
    const bridge = { request } as unknown as Bridge;
    expect(() => applySkydomeSeamFix(bridge, true)).not.toThrow();
    await Promise.resolve();
  });
});
