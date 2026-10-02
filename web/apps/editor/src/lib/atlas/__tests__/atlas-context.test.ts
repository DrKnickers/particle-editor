import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  publishAtlasContext,
  getAtlasContext,
  subscribeAtlasContext,
  __resetAtlasContext,
} from "../atlas-context";

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

  describe("subscribeAtlasContext", () => {
    it("fires the listener with (next, prev) on each publish; unsubscribe stops it", () => {
      const listener = vi.fn();
      const unsub = subscribeAtlasContext(listener);

      // Distinct objects each publish — publishAtlasContext replaces the root,
      // so every call notifies (a same-Object.is state would be skipped).
      publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
      expect(listener).toHaveBeenCalledTimes(1);
      const [next, prev] = listener.mock.calls[0]!;
      expect(next.emitterId).toBe(1);
      expect(prev.emitterId).toBeNull(); // was the EMPTY default

      publishAtlasContext({ emitterId: 2, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } });
      expect(listener).toHaveBeenCalledTimes(2);
      expect(listener.mock.calls[1]![1].emitterId).toBe(1); // prev is the first publish

      unsub();
      publishAtlasContext({ emitterId: 3, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } });
      expect(listener).toHaveBeenCalledTimes(2); // no further calls after unsubscribe
    });
  });
});
