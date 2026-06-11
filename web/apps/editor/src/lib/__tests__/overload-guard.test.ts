import { describe, it, expect, vi } from "vitest";
import {
  OVERLOAD_GUARD_DEFAULT,
  clampMaxParticles,
  readOverloadGuard,
  writeOverloadGuard,
  applyOverloadGuard,
} from "../overload-guard";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("overload-guard", () => {
  it("defaults when localStorage is empty", () => {
    expect(readOverloadGuard()).toEqual(OVERLOAD_GUARD_DEFAULT);
    expect(OVERLOAD_GUARD_DEFAULT).toEqual({ enabled: true, maxParticles: 10_000 });
  });

  it("round-trips a written config", () => {
    writeOverloadGuard({ enabled: false, maxParticles: 80_000 });
    expect(readOverloadGuard()).toEqual({ enabled: false, maxParticles: 80_000 });
  });

  it("clamps maxParticles to [1_000, 1_000_000]; NaN falls back to the default", () => {
    expect(clampMaxParticles(0)).toBe(1_000);
    expect(clampMaxParticles(999)).toBe(1_000);
    expect(clampMaxParticles(2_000_000)).toBe(1_000_000);
    expect(clampMaxParticles(25_000.7)).toBe(25_001);
    expect(clampMaxParticles(Number.NaN)).toBe(OVERLOAD_GUARD_DEFAULT.maxParticles);
  });

  it("survives corrupt localStorage (bad JSON, wrong types) with the default", () => {
    localStorage.setItem("alo:overload-guard", "{not json");
    expect(readOverloadGuard()).toEqual(OVERLOAD_GUARD_DEFAULT);
    localStorage.setItem("alo:overload-guard", JSON.stringify({ enabled: "yes", maxParticles: "many" }));
    expect(readOverloadGuard()).toEqual(OVERLOAD_GUARD_DEFAULT);
  });

  it("clamps out-of-range stored values on read", () => {
    localStorage.setItem("alo:overload-guard", JSON.stringify({ enabled: true, maxParticles: 5 }));
    expect(readOverloadGuard()).toEqual({ enabled: true, maxParticles: 1_000 });
  });

  it("applyOverloadGuard sends the clamped config over the bridge, fire-and-forget", () => {
    const request = vi.fn().mockResolvedValue({});
    applyOverloadGuard({ request } as unknown as Bridge, { enabled: true, maxParticles: 50 });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 1_000 },
    });
  });
});
