import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { resolveTheme, readStoredMode, applyMode } from "../theme";

describe("theme 3-way", () => {
  beforeEach(() => {
    localStorage.clear();
    document.documentElement.removeAttribute("data-theme");
  });
  it("resolves explicit modes verbatim", () => {
    expect(resolveTheme("dark", true)).toBe("dark");
    expect(resolveTheme("light", true)).toBe("light");
  });
  it("resolves system to the OS preference", () => {
    expect(resolveTheme("system", true)).toBe("dark");
    expect(resolveTheme("system", false)).toBe("light");
  });
  it("defaults to system when nothing is stored", () => {
    expect(readStoredMode()).toBe("system");
  });
  it("reads a stored explicit mode", () => {
    localStorage.setItem("alo:theme", "light");
    expect(readStoredMode()).toBe("light");
  });
  it("applyMode sets data-theme to the resolved value and persists the mode", () => {
    applyMode("dark", true);
    expect(document.documentElement.dataset.theme).toBe("dark");
    expect(localStorage.getItem("alo:theme")).toBe("dark");
  });
});

describe("theme storage guards", () => {
  afterEach(() => vi.restoreAllMocks());

  it("readStoredMode falls back to system when localStorage throws", () => {
    vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => {
      throw new DOMException("SecurityError");
    });
    expect(readStoredMode()).toBe("system");
  });

  it("applyMode still applies the theme when localStorage throws", () => {
    vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => {
      throw new DOMException("QuotaExceededError");
    });
    expect(() => applyMode("light", true)).not.toThrow();
    expect(document.documentElement.dataset.theme).toBe("light");
  });
});
