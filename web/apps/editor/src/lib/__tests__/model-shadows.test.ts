import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  readModelShadows,
  writeModelShadows,
  applyModelShadows,
} from "@/lib/model-shadows";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("model-shadows preference", () => {
  beforeEach(() => localStorage.clear());

  it("defaults to ON when unset", () => {
    expect(readModelShadows()).toBe(true);
  });

  it("round-trips true/false through localStorage", () => {
    writeModelShadows(false);
    expect(readModelShadows()).toBe(false);
    writeModelShadows(true);
    expect(readModelShadows()).toBe(true);
  });

  it("accepts the legacy 'true'/'1' encodings as ON", () => {
    localStorage.setItem("alo:model-shadows", "true");
    expect(readModelShadows()).toBe(true);
    localStorage.setItem("alo:model-shadows", "1");
    expect(readModelShadows()).toBe(true);
    localStorage.setItem("alo:model-shadows", "0");
    expect(readModelShadows()).toBe(false);
  });

  it("applyModelShadows sends engine/set/model-shadows with the flag", () => {
    const request = vi.fn().mockResolvedValue({});
    const bridge = { request } as unknown as Bridge;
    applyModelShadows(bridge, false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/model-shadows",
      params: { enabled: false },
    });
  });

  it("applyModelShadows sends engine/set/model-shadows with enabled:true", () => {
    const request = vi.fn().mockResolvedValue({});
    const bridge = { request } as unknown as Bridge;
    applyModelShadows(bridge, true);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/model-shadows",
      params: { enabled: true },
    });
  });

  it("applyModelShadows swallows a rejected request (fire-and-forget)", async () => {
    const request = vi.fn().mockRejectedValue(new Error("host gone"));
    const bridge = { request } as unknown as Bridge;
    expect(() => applyModelShadows(bridge, true)).not.toThrow();
    await Promise.resolve();
  });
});
