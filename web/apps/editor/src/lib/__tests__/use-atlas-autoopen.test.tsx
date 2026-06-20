import { describe, it, expect, beforeEach } from "vitest";
import { renderHook, act } from "@testing-library/react";
import { useAtlasAutoOpen } from "../use-atlas-autoopen";
import { publishAtlasContext, __resetAtlasContext } from "../atlas-context";
import { setDock, __resetRightDockForTests, useRightDockStoreForTests } from "../right-dock";

const dock = () => useRightDockStoreForTests().getState().dock;
beforeEach(() => { localStorage.clear(); __resetRightDockForTests(); __resetAtlasContext(); setDock("lighting"); });

describe("useAtlasAutoOpen", () => {
  it("auto-opens on first index key selection; restores lighting on channel leave", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting");
  });
  it("a manual dock change while auto-open cancels the restore", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => setDock("spawner"));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("spawner"); // NOT restored to lighting
  });
  it("does not auto-open when not eligible", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: false }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("lighting");
  });
  it("restores on emitter cleared", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => publishAtlasContext({ emitterId: null, focusedTrack: "index", interpolation: null, selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting");
  });
  it("emitter→emitter switch (still on index) is a smooth swap, not a restore", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    // switch to a DIFFERENT non-null emitter, still on index, fresh selection
    act(() => publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step", selection: { frame: 7, keyTimes: [0.5] } }));
    expect(dock()).toBe("atlas"); // stayed — NOT restored to "lighting"
  });
});
