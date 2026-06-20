import { describe, it, expect, beforeEach } from "vitest";
import { publishAtlasContext, getAtlasContext, __resetAtlasContext } from "../atlas-context";

beforeEach(() => __resetAtlasContext());

describe("atlas-context store", () => {
  it("defaults to empty context", () => {
    expect(getAtlasContext()).toEqual({
      emitterId: null, focusedTrack: null, interpolation: null,
      selection: { frame: null, keyTimes: [] },
    });
  });
  it("publishes context atomically", () => {
    publishAtlasContext({
      emitterId: 7, focusedTrack: "index", interpolation: "step",
      selection: { frame: 5, keyTimes: [0.1, 0.5] },
    });
    const c = getAtlasContext();
    expect(c.emitterId).toBe(7);
    expect(c.focusedTrack).toBe("index");
    expect(c.selection).toEqual({ frame: 5, keyTimes: [0.1, 0.5] });
  });
  it("__resetAtlasContext clears back to empty", () => {
    publishAtlasContext({ emitterId: 7, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.1] } });
    __resetAtlasContext();
    expect(getAtlasContext().emitterId).toBeNull();
  });
});
