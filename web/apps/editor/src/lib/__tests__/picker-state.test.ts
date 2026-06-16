import { beforeEach, describe, expect, it, vi } from "vitest";
import {
  __resetPickerStateCacheForTests,
  loadPickerState,
  resolveFaction,
  savePickerState,
} from "../picker-state";

const KEY = "alo:refpicker:v1";

describe("picker-state persistence", () => {
  beforeEach(() => {
    localStorage.clear();
    __resetPickerStateCacheForTests();
  });

  it("returns defaults when nothing is stored", () => {
    expect(loadPickerState()).toEqual({ faction: null, collapsed: [], scrollTop: 0 });
  });

  it("round-trips faction, collapsed and scrollTop in memory (read-after-write)", () => {
    savePickerState({
      faction: "Empire",
      collapsed: ["sec:Heroes", "buc:Ground/Infantry"],
      scrollTop: 120,
    });
    expect(loadPickerState()).toEqual({
      faction: "Empire",
      collapsed: ["sec:Heroes", "buc:Ground/Infantry"],
      scrollTop: 120,
    });
  });

  it("persists across a cache reset (the debounced localStorage write lands)", () => {
    vi.useFakeTimers();
    savePickerState({ faction: "Rebel", scrollTop: 50 });
    vi.runAllTimers(); // flush the debounce -> localStorage.setItem
    vi.useRealTimers();
    __resetPickerStateCacheForTests(); // simulate a fresh popover mount
    const s = loadPickerState();
    expect(s.faction).toBe("Rebel");
    expect(s.scrollTop).toBe(50);
  });

  it("falls back to defaults on malformed JSON", () => {
    localStorage.setItem(KEY, "{not json");
    __resetPickerStateCacheForTests();
    expect(loadPickerState()).toEqual({ faction: null, collapsed: [], scrollTop: 0 });
  });

  it("sanitizes bad shapes (non-string faction/collapsed entries, negative scroll)", () => {
    localStorage.setItem(
      KEY,
      JSON.stringify({ faction: 42, collapsed: ["ok", 7, null], scrollTop: -5 }),
    );
    __resetPickerStateCacheForTests();
    const s = loadPickerState();
    expect(s.faction).toBeNull(); // non-string faction -> null
    expect(s.collapsed).toEqual(["ok"]); // non-strings dropped
    expect(s.scrollTop).toBe(0); // negative rejected
  });
});

describe("resolveFaction (never filter to empty after a mod switch)", () => {
  it("keeps a saved faction that exists in the current set", () => {
    expect(resolveFaction("Empire", ["Rebel", "Empire"])).toBe("Empire");
  });
  it("falls back to All (null) when the saved faction is absent", () => {
    expect(resolveFaction("Empire", ["Pirates", "Indigenous"])).toBeNull();
  });
  it("falls back to All (null) when the new content has NO factions at all", () => {
    // The dead-end case: a mod whose units carry no affiliation -> empty chip row,
    // so a stale faction must resolve to All or the picker shows an unclearable empty list.
    expect(resolveFaction("Empire", [])).toBeNull();
  });
  it("returns null for a null saved value", () => {
    expect(resolveFaction(null, ["Empire"])).toBeNull();
  });
});
