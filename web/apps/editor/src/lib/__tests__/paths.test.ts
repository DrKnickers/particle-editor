import { describe, expect, it } from "vitest";
import { basename, eqPath } from "../paths";

describe("path helpers", () => {
  it("keeps basename display case while accepting either separator", () => {
    expect(basename("C:\\Mods/Fire.ALO")).toBe("Fire.ALO");
  });

  it("normalizes repeated and trailing separators only when requested", () => {
    expect(basename("C:\\Mods\\\\Fire\\", { normalizeSeparators: true })).toBe("Fire");
    expect(basename("C:\\Mods\\\\Fire\\")).toBe("");
  });

  it("compares canonical layer paths case-insensitively without trailing separators", () => {
    expect(eqPath("C:/Mods/Fire/", "c:/mods/fire")).toBe(true);
    expect(eqPath("C:/Mods/Fire", "c:\\mods\\fire")).toBe(false);
  });
});
