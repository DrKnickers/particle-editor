import { beforeEach, describe, expect, it } from "vitest";
import {
  __resetRightDockForTests,
  setDock,
  toggleDock,
  useRightDockStoreForTests,
} from "@/lib/right-dock";

// The store is module-level; clear localStorage and re-seed the in-memory
// state from it before each test so cases don't leak into one another.
function reseed(seed?: Record<string, string>) {
  localStorage.clear();
  if (seed) for (const [k, v] of Object.entries(seed)) localStorage.setItem(k, v);
  __resetRightDockForTests();
}

describe("right-dock store", () => {
  beforeEach(() => reseed());

  it("defaults to the Spawner when no keys are set", () => {
    expect(useRightDockStoreForTests().getState().dock).toBe("spawner");
  });

  it("toggle is exclusive: opening the open target closes it", () => {
    setDock("spawner");
    toggleDock("spawner");
    expect(useRightDockStoreForTests().getState().dock).toBeNull();
    toggleDock("spawner");
    expect(useRightDockStoreForTests().getState().dock).toBe("spawner");
  });

  it("toggle swaps content when a different target is opened", () => {
    setDock("spawner");
    toggleDock("lighting");
    expect(useRightDockStoreForTests().getState().dock).toBe("lighting");
    toggleDock("spawner");
    expect(useRightDockStoreForTests().getState().dock).toBe("spawner");
  });

  it("persists the selection to localStorage('alo:right-dock')", () => {
    setDock("lighting");
    expect(localStorage.getItem("alo:right-dock")).toBe("lighting");
    setDock(null);
    expect(localStorage.getItem("alo:right-dock")).toBe("none");
  });

  it("reads back a persisted 'none' as null", () => {
    reseed({ "alo:right-dock": "none" });
    expect(useRightDockStoreForTests().getState().dock).toBeNull();
  });

  it("migrates legacy alo:spawner-visible=true → spawner", () => {
    reseed({ "alo:spawner-visible": "true" });
    expect(useRightDockStoreForTests().getState().dock).toBe("spawner");
  });

  it("migrates legacy alo:spawner-visible=false → null (closed)", () => {
    reseed({ "alo:spawner-visible": "false" });
    expect(useRightDockStoreForTests().getState().dock).toBeNull();
  });

  it("prefers the new key over the legacy key when both exist", () => {
    reseed({ "alo:right-dock": "lighting", "alo:spawner-visible": "false" });
    expect(useRightDockStoreForTests().getState().dock).toBe("lighting");
  });
});

describe("right-dock atlas value", () => {
  beforeEach(() => reseed());

  it("setDock('atlas') is accepted and persisted", () => {
    setDock("atlas");
    expect(useRightDockStoreForTests().getState().dock).toBe("atlas");
    expect(localStorage.getItem("alo:right-dock")).toBe("atlas");
  });

  it("a persisted 'atlas' boots to null (no selection context at startup)", () => {
    // reseed with atlas: readInitial() should reject 'atlas' and return null
    reseed({ "alo:right-dock": "atlas" });
    expect(useRightDockStoreForTests().getState().dock).toBeNull();
  });

  it("persisted lighting still restores after atlas is added", () => {
    reseed({ "alo:right-dock": "lighting" });
    expect(useRightDockStoreForTests().getState().dock).toBe("lighting");
  });

  it("toggleDock('atlas') opens the atlas panel", () => {
    toggleDock("atlas");
    expect(useRightDockStoreForTests().getState().dock).toBe("atlas");
  });

  it("toggleDock('atlas') when already open closes it", () => {
    setDock("atlas");
    toggleDock("atlas");
    expect(useRightDockStoreForTests().getState().dock).toBeNull();
  });
});
