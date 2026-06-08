// Vitest specs for the emitter-selection Zustand atom (Screen 4 B2).
// Covers the four action shapes: setSingle, toggle add/remove, range,
// and clear. The atom is the React-side ground truth for multi-select;
// EmitterTree click handlers route through it.

import { describe, it, expect, beforeEach } from "vitest";
import { useEmitterSelectionStore } from "../emitter-selection";

beforeEach(() => {
  useEmitterSelectionStore.getState().clear();
});

describe("emitter-selection atom", () => {
  it("setSingle replaces the selection and sets primary", () => {
    const s = useEmitterSelectionStore.getState();
    s.setSingle(3);
    expect(useEmitterSelectionStore.getState().ids).toEqual([3]);
    expect(useEmitterSelectionStore.getState().primary).toBe(3);
    s.setSingle(7);
    expect(useEmitterSelectionStore.getState().ids).toEqual([7]);
    expect(useEmitterSelectionStore.getState().primary).toBe(7);
  });

  it("toggle adds an unselected id and makes it primary", () => {
    const s = useEmitterSelectionStore.getState();
    s.setSingle(3);
    s.toggle(5);
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([3, 5]);
    expect(st.primary).toBe(5);
  });

  it("toggle removes a selected id and demotes primary on removal", () => {
    const s = useEmitterSelectionStore.getState();
    s.setSingle(3);
    s.toggle(5); // primary now 5
    s.toggle(5); // remove primary
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([3]);
    expect(st.primary).toBe(3);
  });

  it("toggle removing a non-primary keeps primary", () => {
    const s = useEmitterSelectionStore.getState();
    s.setSingle(3);
    s.toggle(5); // primary now 5
    s.toggle(3); // remove non-primary
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([5]);
    expect(st.primary).toBe(5);
  });

  it("range selects the inclusive slice between primary and toId in tree order", () => {
    const order = [0, 1, 2, 3, 4, 5];
    const s = useEmitterSelectionStore.getState();
    s.setSingle(1);
    s.range(4, order);
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([1, 2, 3, 4]);
    expect(st.primary).toBe(4);
  });

  it("range works both directions and updates primary to toId", () => {
    const order = [0, 1, 2, 3, 4, 5];
    const s = useEmitterSelectionStore.getState();
    s.setSingle(4);
    s.range(1, order);
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([1, 2, 3, 4]);
    expect(st.primary).toBe(1);
  });

  it("consecutive shift ranges pivot from the ORIGINAL anchor, not the moving primary", () => {
    // Repro: click `default` (0), shift-click `default_2` (1), shift-click
    // `default_1` (2). Each shift must extend from the first-clicked anchor
    // (0), so the result is [0,1,2] — not [1,2] (the moving-anchor bug).
    const order = [0, 1, 2, 3, 4, 5];
    const s = useEmitterSelectionStore.getState();
    s.setSingle(0); // anchor = 0
    s.range(1, order); // [0,1]
    s.range(2, order); // extends from anchor 0, not primary 1
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([0, 1, 2]);
    expect(st.primary).toBe(2);
  });

  it("a plain click re-anchors subsequent shift ranges", () => {
    const order = [0, 1, 2, 3, 4, 5];
    const s = useEmitterSelectionStore.getState();
    s.setSingle(0);
    s.range(2, order); // [0,1,2], anchor still 0
    s.setSingle(4); // new anchor = 4
    s.range(5, order); // extends from 4, not 0
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([4, 5]);
    expect(st.primary).toBe(5);
  });

  it("clear empties the selection", () => {
    const s = useEmitterSelectionStore.getState();
    s.setSingle(3);
    s.clear();
    const st = useEmitterSelectionStore.getState();
    expect(st.ids).toEqual([]);
    expect(st.primary).toBeNull();
  });
});
