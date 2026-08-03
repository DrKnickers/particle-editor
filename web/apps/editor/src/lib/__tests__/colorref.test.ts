import { describe, it, expect } from "vitest";
import { colorrefToRgb, pillScrimMode, pillBackdropColor } from "@/lib/colorref";

describe("colorrefToRgb", () => {
  it("extracts R from the low byte (Win32 COLORREF order)", () => {
    // RGB(0x14,0x08,0x34) === 0x00340814 (see mock-state.ts)
    expect(colorrefToRgb(0x00340814)).toEqual({ r: 0x14, g: 0x08, b: 0x34 });
  });
});

describe("pillScrimMode", () => {
  it("flips to light over dark-through-neutral backgrounds (engages on neutral)", () => {
    expect(pillScrimMode(0x000000)).toBe("light"); // pure black
    expect(pillScrimMode(0x00340814)).toBe("light"); // dark purple, lum ~0.006
    expect(pillScrimMode(0x00505050)).toBe("light"); // dark grey, lum ~0.080
    expect(pillScrimMode(0x006e6e6e)).toBe("light"); // neutral grey (common default), ~0.156 < 0.2
  });
  it("uses the dark scrim over mid-grey-and-brighter backgrounds", () => {
    expect(pillScrimMode(0x00808080)).toBe("dark"); // mid-grey, ~0.216 >= 0.2
    expect(pillScrimMode(0x00b0b0b0)).toBe("dark"); // light grey
    expect(pillScrimMode(0x00ffffff)).toBe("dark"); // white
  });
});

describe("pillBackdropColor", () => {
  const base = { ground: false, groundColor: 0x00334455, background: 0x00340814 };
  it("uses the background when the ground is hidden", () => {
    expect(pillBackdropColor({ ...base, ground: false })).toBe(0x00340814);
  });
  it("uses the host-computed ground colour when the ground is shown (solid or averaged texture)", () => {
    expect(pillBackdropColor({ ...base, ground: true, groundColor: 0x00334455 })).toBe(0x00334455);
    expect(pillBackdropColor({ ...base, ground: true, groundColor: 0x00ffffff })).toBe(0x00ffffff);
  });
});
