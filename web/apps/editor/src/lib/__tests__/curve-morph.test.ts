import { describe, it, expect } from "vitest";
import type { TrackDto } from "@particle-editor/bridge-schema";
import {
  sampleTrackY, buildMorphGrid, sampleTrackPx, resampleOntoGrid,
  classifyTrackChange, matchKeys, easeOutCubic,
  MORPH_MS, KEY_MATCH_EPS,
} from "../curve-morph";
import { movesMatch } from "../use-curve-morph";

const track = (
  keys: Array<{ time: number; value: number }>,
  interpolation: TrackDto["interpolation"] = "linear",
  lockedTo: TrackDto["lockedTo"] = null,
): TrackDto => ({ name: "red", keys, interpolation, lockedTo });

describe("sampleTrackY", () => {
  const keys = [{ time: 0, value: 0 }, { time: 50, value: 1 }, { time: 100, value: 0.5 }];

  it("linear: exact lerp", () => {
    expect(sampleTrackY(keys, "linear", 25)).toBeCloseTo(0.5, 10);
    expect(sampleTrackY(keys, "linear", 75)).toBeCloseTo(0.75, 10);
  });

  it("step: left-key plateau, including exactly at a key", () => {
    expect(sampleTrackY(keys, "step", 49.999)).toBe(0);
    expect(sampleTrackY(keys, "step", 50)).toBe(1);     // at the key, the key's own value
    expect(sampleTrackY(keys, "step", 50.001)).toBe(1);
  });

  it("smooth: hits key values at key times; midpoint matches the smoothstep identity", () => {
    expect(sampleTrackY(keys, "smooth", 0)).toBeCloseTo(0, 10);
    expect(sampleTrackY(keys, "smooth", 50)).toBeCloseTo(1, 10);
    // x-midpoint of segment: u=0.5 → t solves 0.75t+0.75t²-0.5t³=0.5 → t=0.5
    // (check: 0.375+0.1875-0.0625 = 0.5) → y = smoothstep(0,1,0.5) = 0.5
    expect(sampleTrackY(keys, "smooth", 25)).toBeCloseTo(0.5, 6);
  });

  it("smooth: Newton converges on adversarial segment widths", () => {
    const tiny = [{ time: 0, value: 0 }, { time: 1e-3, value: 1 }, { time: 100, value: 0 }];
    for (const x of [0, 2.5e-4, 5e-4, 1e-3, 50, 99.9]) {
      const y = sampleTrackY(tiny, "smooth", x);
      expect(Number.isFinite(y)).toBe(true);
      expect(y).toBeGreaterThanOrEqual(-1e-9);
      expect(y).toBeLessThanOrEqual(1 + 1e-9);
    }
  });

  it("clamps outside the border keys", () => {
    expect(sampleTrackY(keys, "smooth", -10)).toBe(0);
    expect(sampleTrackY(keys, "smooth", 200)).toBe(0.5);
  });

  it("duplicate-time keys produce finite values (vertical jump, no NaN)", () => {
    // Two keys share time=50; the while-loop advances past the first duplicate
    // so b.time > a.time is always satisfied — no divide-by-zero.
    const dupKeys = [
      { time: 0, value: 0 },
      { time: 50, value: 0.2 },
      { time: 50, value: 0.8 },
      { time: 100, value: 1 },
    ];
    for (const interp of ["linear", "smooth"] as const) {
      for (const x of [49.999, 50, 50.001]) {
        const y = sampleTrackY(dupKeys, interp, x);
        expect(Number.isFinite(y)).toBe(true);
      }
    }
  });
});

describe("buildMorphGrid", () => {
  it("contains uniform samples plus epsilon pairs around every key time of both tracks, sorted, within range", () => {
    const a = track([{ time: 0, value: 0 }, { time: 30, value: 1 }, { time: 100, value: 0 }]);
    const b = track([{ time: 0, value: 0 }, { time: 62, value: 1 }, { time: 100, value: 0 }]);
    const g = buildMorphGrid(a, b, 0, 100, 160);
    expect(g.length).toBeGreaterThanOrEqual(161);
    for (let i = 1; i < g.length; i++) expect(g[i]!).toBeGreaterThanOrEqual(g[i - 1]!);
    expect(g[0]).toBe(0);
    expect(g[g.length - 1]).toBe(100);
    // epsilon pair straddles each interior key time
    const has = (x: number) => Array.from(g).some((v) => Math.abs(v - x) < 1e-9);
    const below30 = Array.from(g).some((v) => v < 30 && 30 - v < 1e-3);
    const above30 = Array.from(g).some((v) => v > 30 && v - 30 < 1e-3);
    expect(below30 && above30).toBe(true);
    const below62 = Array.from(g).some((v) => v < 62 && 62 - v < 1e-3);
    const above62 = Array.from(g).some((v) => v > 62 && v - 62 < 1e-3);
    expect(below62 && above62).toBe(true);
    expect(has(0)).toBe(true);
  });

  it("step jump renders as a true vertical: the epsilon pair samples both plateau values", () => {
    const s = track([{ time: 0, value: 0 }, { time: 50, value: 1 }, { time: 100, value: 1 }], "step");
    const g = buildMorphGrid(s, s, 0, 100, 160);
    const ys = sampleTrackPx(s, g, { vMin: 0, vMax: 1, height: 100 });
    // find the epsilon pair around 50: y jumps from 100 (v=0 → bottom) to 0 (v=1 → top)
    let jumped = false;
    for (let i = 1; i < g.length; i++) {
      if (g[i]! > 49.9 && g[i]! < 50.1 && Math.abs(ys[i]! - ys[i - 1]!) > 99) jumped = true;
    }
    expect(jumped).toBe(true);
  });
});

describe("sampleTrackPx / resampleOntoGrid", () => {
  it("projects with y-inversion (bigger value = smaller pixel y)", () => {
    const t = track([{ time: 0, value: 0 }, { time: 100, value: 1 }]);
    const g = buildMorphGrid(t, t, 0, 100, 4);
    const ys = sampleTrackPx(t, g, { vMin: 0, vMax: 1, height: 300 });
    expect(ys[0]).toBeCloseTo(300, 6);
    expect(ys[ys.length - 1]).toBeCloseTo(0, 6);
  });

  it("resampleOntoGrid: linear interpolation of a displayed polyline onto a new grid", () => {
    const xs = new Float64Array([0, 50, 100]);
    const ys = new Float64Array([0, 100, 0]);
    const out = resampleOntoGrid(xs, ys, new Float64Array([0, 25, 50, 75, 100]));
    expect(Array.from(out)).toEqual([0, 50, 100, 50, 0]);
  });

  it("resampleOntoGrid: empty xsOld → all-zero output of the right length", () => {
    const out = resampleOntoGrid(
      new Float64Array(0),
      new Float64Array(0),
      new Float64Array([0, 25, 50, 75, 100]),
    );
    expect(out.length).toBe(5);
    expect(Array.from(out)).toEqual([0, 0, 0, 0, 0]);
  });

  it("resampleOntoGrid: single-point xsOld → every output equals ysOld[0]", () => {
    const out = resampleOntoGrid(
      new Float64Array([42]),
      new Float64Array([7.5]),
      new Float64Array([0, 25, 50, 75, 100]),
    );
    expect(out.length).toBe(5);
    expect(Array.from(out)).toEqual([7.5, 7.5, 7.5, 7.5, 7.5]);
  });
});

describe("classifyTrackChange", () => {
  const base = track([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
  it("none for identical", () => {
    expect(classifyTrackChange(base, track(base.keys.map((k) => ({ ...k }))))).toBe("none");
  });
  it("moved for value-only deltas", () => {
    const next = track([{ time: 0, value: 0 }, { time: 50, value: 0.9 }, { time: 100, value: 1 }]);
    expect(classifyTrackChange(base, next)).toBe("moved");
  });
  it("structural for add / delete / time-move / interp flip", () => {
    expect(classifyTrackChange(base, track([...base.keys, { time: 75, value: 0.2 }]
      .sort((a, b) => a.time - b.time)))).toBe("structural");
    expect(classifyTrackChange(base, track([base.keys[0]!, base.keys[2]!]))).toBe("structural");
    expect(classifyTrackChange(base, track([{ time: 0, value: 0 }, { time: 60, value: 0.5 }, { time: 100, value: 1 }]))).toBe("structural");
    expect(classifyTrackChange(base, track(base.keys, "step"))).toBe("structural");
  });
  it("lockedTo alone is NOT a shape change", () => {
    expect(classifyTrackChange(base, track(base.keys, "linear", "red"))).toBe("none");
  });
});

describe("matchKeys", () => {
  it("partitions moved/added/removed by time within EPS", () => {
    const prev = track([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    const next = track([{ time: 0, value: 0.2 }, { time: 75, value: 0.9 }, { time: 100, value: 1 }]);
    const m = matchKeys(prev, next);
    expect(m.moved.map((p) => p.to.time)).toEqual([0, 100]);
    expect(m.added.map((k) => k.time)).toEqual([75]);
    expect(m.removed.map((k) => k.time)).toEqual([50]);
  });
});

describe("easing/constants", () => {
  it("easeOutCubic endpoints + monotonic", () => {
    expect(easeOutCubic(0)).toBe(0);
    expect(easeOutCubic(1)).toBe(1);
    expect(easeOutCubic(0.5)).toBeGreaterThan(0.5);
  });
  it("exports the tunables", () => {
    expect(MORPH_MS).toBeGreaterThan(0);
    expect(KEY_MATCH_EPS).toBeGreaterThan(0);
  });
});

describe("movesMatch", () => {
  const mk = (keys: Array<{ time: number; value: number }>): TrackDto => ({
    name: "red",
    keys,
    interpolation: "linear",
    lockedTo: null,
  });

  it("single move, in-order → true", () => {
    const prev = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    const next = mk([{ time: 0, value: 0 }, { time: 70, value: 0.7 }, { time: 100, value: 1 }]);
    const moves = [{ oldTime: 50, newTime: 70, newValue: 0.7 }];
    expect(movesMatch(prev, next, moves)).toBe(true);
  });

  it("group move that REORDERS keys → true (the reorder-tolerant case)", () => {
    // prev: [0, 30, 60, 100]; selected key at t=30 is dragged to t=80,
    // crossing the unselected key at t=60. The optimistic overlay re-sorts
    // next to [0, 60, 80, 100]. Index-paired comparison would fail here.
    const prev = mk([
      { time: 0, value: 0 },
      { time: 30, value: 0.3 },
      { time: 60, value: 0.6 },
      { time: 100, value: 1 },
    ]);
    const next = mk([
      { time: 0, value: 0 },
      { time: 60, value: 0.6 },
      { time: 80, value: 0.8 },
      { time: 100, value: 1 },
    ]);
    const moves = [{ oldTime: 30, newTime: 80, newValue: 0.8 }];
    expect(movesMatch(prev, next, moves)).toBe(true);
  });

  it("unexplained extra structural change (an added key) → false", () => {
    const prev = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    // next has 4 keys — count mismatch
    const next = mk([
      { time: 0, value: 0 },
      { time: 50, value: 0.5 },
      { time: 75, value: 0.75 },
      { time: 100, value: 1 },
    ]);
    const moves = [{ oldTime: 50, newTime: 50, newValue: 0.5 }];
    expect(movesMatch(prev, next, moves)).toBe(false);
  });

  it("move whose oldTime matches no prev key → false", () => {
    const prev = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    const next = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    // oldTime: 999 doesn't exist in prev
    const moves = [{ oldTime: 999, newTime: 50, newValue: 0.5 }];
    expect(movesMatch(prev, next, moves)).toBe(false);
  });

  it("unchanged track with no moves → true", () => {
    const prev = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }]);
    const next = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }]);
    expect(movesMatch(prev, next, [])).toBe(true);
  });

  it("next key value diverges from recorded move's newValue → false", () => {
    const prev = mk([{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]);
    const next = mk([{ time: 0, value: 0 }, { time: 70, value: 0.9 }, { time: 100, value: 1 }]);
    // Move says newValue=0.7 but next key landed at 0.9
    const moves = [{ oldTime: 50, newTime: 70, newValue: 0.7 }];
    expect(movesMatch(prev, next, moves)).toBe(false);
  });
});
