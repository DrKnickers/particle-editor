// Pins the shared boolean-pref backing (DRY audit web-bridge-state-4): the
// "1"/"0" encoding, the legacy "true" decode, the absent->default path, and the
// SILENT quota/private-mode catch (previously uncovered — no module test stubbed
// a throwing localStorage).
import { describe, it, expect, afterEach, vi } from "vitest";
import { readBooleanPref, writeBooleanPref } from "../boolean-pref";

const KEY = "alo:test-pref";

afterEach(() => {
  vi.restoreAllMocks();
  localStorage.clear();
});

describe("boolean-pref", () => {
  it("read: absent -> default (either polarity)", () => {
    expect(readBooleanPref(KEY, true)).toBe(true);
    expect(readBooleanPref(KEY, false)).toBe(false);
  });

  it('read: "1"/"true" -> true, "0"/anything-else -> false', () => {
    localStorage.setItem(KEY, "1");
    expect(readBooleanPref(KEY, false)).toBe(true);
    localStorage.setItem(KEY, "true"); // legacy encoding still honoured
    expect(readBooleanPref(KEY, false)).toBe(true);
    localStorage.setItem(KEY, "0");
    expect(readBooleanPref(KEY, true)).toBe(false);
  });

  it("write: stores the canonical \"1\"/\"0\"", () => {
    writeBooleanPref(KEY, true);
    expect(localStorage.getItem(KEY)).toBe("1");
    writeBooleanPref(KEY, false);
    expect(localStorage.getItem(KEY)).toBe("0");
  });

  it("write: a throwing localStorage (quota/private-mode) is swallowed, not propagated", () => {
    vi.spyOn(Storage.prototype, "setItem").mockImplementation(() => {
      throw new DOMException("QuotaExceededError");
    });
    expect(() => writeBooleanPref(KEY, true)).not.toThrow();
  });

  it("read: a throwing localStorage falls back to the default", () => {
    vi.spyOn(Storage.prototype, "getItem").mockImplementation(() => {
      throw new DOMException("SecurityError");
    });
    expect(readBooleanPref(KEY, true)).toBe(true);
  });
});
