import { describe, expect, it } from "vitest";
import type { EmitterTreeNode, SpawnParamsDto } from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import {
  CHAIN_WARN_THRESHOLD,
  estimateChainLoad,
  estimatePerEmitter,
  estimateSystemLoad,
  formatChainWarning,
} from "../chain-load";

const spawn = (s: Partial<SpawnParamsDto>): SpawnParamsDto => ({ ...ZERO_SPAWN, ...s });

let nextStable = 1;
const node = (
  name: string,
  s: Partial<SpawnParamsDto>,
  children: EmitterTreeNode[] = [],
  role: EmitterTreeNode["role"] = "root",
): EmitterTreeNode => ({
  id: nextStable, stableId: nextStable++, name, role,
  linkGroup: 0, visible: true, spawn: spawn(s), children,
});

const syntheticRoot = (children: EmitterTreeNode[]): EmitterTreeNode => ({
  id: -1, stableId: 0, name: "", role: "root",
  linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children,
});

describe("estimatePerEmitter", () => {
  it("continuous: rate × lifetime", () => {
    expect(estimatePerEmitter(spawn({ nParticlesPerSecond: 12, lifetime: 1.5 }))).toBe(18);
  });
  it("burst: particlesPerBurst × concurrent bursts, capped by nBursts", () => {
    // lifetime 3s / delay 1s → floor(3)+1 = 4 concurrent, capped at nBursts=2
    expect(estimatePerEmitter(spawn({
      useBursts: true, nParticlesPerBurst: 10, nBursts: 2, burstDelay: 1, lifetime: 3,
    }))).toBe(20);
  });
  it("burst: nBursts=0 means infinite (no cap)", () => {
    expect(estimatePerEmitter(spawn({
      useBursts: true, nParticlesPerBurst: 10, nBursts: 0, burstDelay: 1, lifetime: 3,
    }))).toBe(40);
  });
  it("burst: burstDelay=0 degenerates to per-burst × nBursts", () => {
    expect(estimatePerEmitter(spawn({
      useBursts: true, nParticlesPerBurst: 7, nBursts: 5, burstDelay: 0, lifetime: 1,
    }))).toBe(35);
  });
  it("burst: infinite bursts at zero delay clamps finite (no Infinity/NaN)", () => {
    const e = estimatePerEmitter(spawn({
      useBursts: true, nParticlesPerBurst: 1, nBursts: 0, burstDelay: 0, lifetime: 1,
    }));
    expect(Number.isFinite(e)).toBe(true);
    expect(e).toBeGreaterThan(CHAIN_WARN_THRESHOLD);
  });
  it("zero-rate emitter estimates 0", () => {
    expect(estimatePerEmitter(ZERO_SPAWN)).toBe(0);
  });
  it("negative rate clamps to 0 (corrupt DTO defense)", () => {
    expect(estimatePerEmitter(spawn({ nParticlesPerSecond: -5, lifetime: 2 }))).toBe(0);
  });
});

describe("estimateChainLoad", () => {
  it("vanilla-scale tree produces no warnings", () => {
    const tree = syntheticRoot([
      node("smoke", { nParticlesPerSecond: 10, lifetime: 2 }, [
        node("embers", { nParticlesPerSecond: 5, lifetime: 1 }, [], "lifetime"),
      ]),
    ]);
    expect(estimateChainLoad(tree).size).toBe(0);
  });
  it("depth-3 product crossing the threshold marks the whole path", () => {
    // 18 × 30 × 40 = 21,600 > 10,000
    const leaf = node("smoke", { nParticlesPerSecond: 40, lifetime: 1 }, [], "death");
    const mid = node("highlight", { nParticlesPerSecond: 30, lifetime: 1 }, [leaf], "lifetime");
    const root = node("sparkle", { nParticlesPerSecond: 12, lifetime: 1.5 }, [mid]);
    const warnings = estimateChainLoad(syntheticRoot([root]));
    expect(warnings.size).toBe(3);
    for (const n of [root, mid, leaf]) {
      expect(warnings.get(n.stableId)?.estimate).toBeCloseTo(21_600);
    }
    expect(warnings.get(root.stableId)?.path.map((p) => p.name))
      .toEqual(["sparkle", "highlight", "smoke"]);
  });
  it("a sibling on a sane branch stays unmarked", () => {
    const bomb = node("bomb", { nParticlesPerSecond: 200, lifetime: 100 }, [], "lifetime"); // 20,000
    const calm = node("calm", { nParticlesPerSecond: 1, lifetime: 1 }, [], "death");
    const root = node("base", { nParticlesPerSecond: 10, lifetime: 1 }, [bomb, calm]);
    const warnings = estimateChainLoad(syntheticRoot([root]));
    expect(warnings.has(bomb.stableId)).toBe(true);
    expect(warnings.has(root.stableId)).toBe(true);
    expect(warnings.has(calm.stableId)).toBe(false);
  });
  it("an ancestor shared by two offending paths reports the WORST estimate", () => {
    const worse = node("worse", { nParticlesPerSecond: 5000, lifetime: 1 }, [], "lifetime"); // 50,000
    const bad = node("bad", { nParticlesPerSecond: 2000, lifetime: 1 }, [], "death");        // 20,000
    const root = node("base", { nParticlesPerSecond: 10, lifetime: 1 }, [worse, bad]);
    const warnings = estimateChainLoad(syntheticRoot([root]));
    expect(warnings.get(root.stableId)?.estimate).toBeCloseTo(50_000);
  });
  it("a zero-rate link breaks the chain (downstream estimates 0, no warning)", () => {
    const leaf = node("leaf", { nParticlesPerSecond: 1e9, lifetime: 10 }, [], "death");
    const dead = node("dead", {}, [leaf], "lifetime"); // E = 0
    const root = node("base", { nParticlesPerSecond: 100, lifetime: 10 }, [dead]);
    expect(estimateChainLoad(syntheticRoot([root])).size).toBe(0);
  });
  it("single emitter over threshold warns alone (chain of one)", () => {
    const solo = node("solo", { nParticlesPerSecond: 20_000, lifetime: 1 });
    const warnings = estimateChainLoad(syntheticRoot([solo]));
    expect(warnings.size).toBe(1);
    expect(warnings.get(solo.stableId)?.estimate).toBe(20_000);
  });
  it("never emits Infinity or NaN even on degenerate inputs", () => {
    const degenerate = node("degen", { useBursts: true, nParticlesPerBurst: 1, nBursts: 0, burstDelay: 0 });
    const warnings = estimateChainLoad(syntheticRoot([degenerate]));
    const w = warnings.get(degenerate.stableId);
    expect(w).toBeDefined();
    expect(Number.isFinite(w!.estimate)).toBe(true);
  });
  it("honours a passed threshold below the default (cap-tracking glyph)", () => {
    // 2,000/s × 1 s = 2,000 — silent at the default 10k, flagged at cap 1,000.
    const root = node("stream", { nParticlesPerSecond: 2_000, lifetime: 1 });
    expect(estimateChainLoad(syntheticRoot([root])).size).toBe(0);
    const warnings = estimateChainLoad(syntheticRoot([root]), 1_000);
    expect(warnings.get(root.stableId)?.estimate).toBe(2_000);
  });
  it("honours a passed threshold above the default", () => {
    // 20,000 estimate: flagged at the default 10k, silent at cap 50k —
    // the deliberate semantic change (glyph ⟺ gate, not "heavy").
    const root = node("big", { nParticlesPerSecond: 20_000, lifetime: 1 });
    expect(estimateChainLoad(syntheticRoot([root])).size).toBe(1);
    expect(estimateChainLoad(syntheticRoot([root]), 50_000).size).toBe(0);
  });
});

describe("estimateSystemLoad", () => {
  it("empty tree → 0", () => {
    expect(estimateSystemLoad(syntheticRoot([]))).toBe(0);
  });

  it("single root = its own per-emitter estimate", () => {
    // nParticlesPerSecond=12, lifetime=1 → E = 12
    const s = spawn({ nParticlesPerSecond: 12, lifetime: 1 });
    const root = syntheticRoot([node("a", s)]);
    expect(estimateSystemLoad(root)).toBeCloseTo(estimatePerEmitter(s), 6);
  });

  it("chains multiply and SUM across nodes (A(parent)+A(parent)×E(child))", () => {
    // parent E=10, child E=5 → A(parent)=10, A(child)=10×5=50 → total=60
    const child = node("child", spawn({ nParticlesPerSecond: 5, lifetime: 1 }), [], "lifetime");
    const parent = node("parent", spawn({ nParticlesPerSecond: 10, lifetime: 1 }), [child]);
    expect(estimateSystemLoad(syntheticRoot([parent]))).toBeCloseTo(60, 6);
  });

  it("multiple roots sum", () => {
    // rootA E=10, rootB E=20 → total = 30
    const rootA = node("rootA", spawn({ nParticlesPerSecond: 10, lifetime: 1 }));
    const rootB = node("rootB", spawn({ nParticlesPerSecond: 20, lifetime: 1 }));
    expect(estimateSystemLoad(syntheticRoot([rootA, rootB]))).toBeCloseTo(30, 6);
  });

  it("agreement: system load >= max single-chain cumulative for an over-threshold tree", () => {
    // Reuse the depth-3 bomb fixture: 18 × 30 × 40 = 21,600
    // estimateChainLoad reports each node's cumulative; system load sums ALL nodes
    // so it must be >= the highest cumulative in the chain-load map.
    const leaf = node("smoke", { nParticlesPerSecond: 40, lifetime: 1 }, [], "death");
    const mid = node("highlight", { nParticlesPerSecond: 30, lifetime: 1 }, [leaf], "lifetime");
    const root = node("sparkle", { nParticlesPerSecond: 12, lifetime: 1.5 }, [mid]);
    const tree = syntheticRoot([root]);
    const chainMap = estimateChainLoad(tree);
    const maxChainCumulative = Math.max(...[...chainMap.values()].map((w) => w.estimate));
    expect(estimateSystemLoad(tree)).toBeGreaterThanOrEqual(maxChainCumulative);
  });
});

describe("death children (spawnOnDeath) scale by death rate, not alive count (#562)", () => {
  it("death child = A(parent) × E × L(child)/L(parent), well below the uniform product", () => {
    // parent E = 100×10 = 1000 (A=1000); death child E = 10×2 = 20.
    // factor = L(child)/L(parent) = 2/10 = 0.2 → A(child) = 1000×20×0.2 = 4,000.
    const child = node("debris", { nParticlesPerSecond: 10, lifetime: 2 }, [], "death");
    const parent = node("smoke", { nParticlesPerSecond: 100, lifetime: 10 }, [child]);
    // total = 1000 (parent) + 4000 (child) = 5000; uniform rule was 1000 + 20000 = 21000.
    expect(estimateSystemLoad(syntheticRoot([parent]))).toBeCloseTo(5_000, 6);
    expect(estimateSystemLoad(syntheticRoot([parent]))).toBeLessThan(21_000);
  });

  it("a LIFETIME child keeps the uniform product even with differing lifetimes (control)", () => {
    // Same numbers as above but a lifetime child: no death factor → A(child)=20000.
    const child = node("embers", { nParticlesPerSecond: 10, lifetime: 2 }, [], "lifetime");
    const parent = node("smoke", { nParticlesPerSecond: 100, lifetime: 10 }, [child]);
    expect(estimateSystemLoad(syntheticRoot([parent]))).toBeCloseTo(21_000, 6);
  });

  it("a deep death chain under long-lived parents no longer explodes exponentially", () => {
    // smoke(10s) → death debris(2s) → death sparks(0.5s). Long parents die
    // rarely, so each death link scales DOWN, not up.
    const sparks = node("sparks", { nParticlesPerSecond: 40, lifetime: 0.5 }, [], "death"); // E=20
    const debris = node("debris", { nParticlesPerSecond: 50, lifetime: 2 }, [sparks], "death"); // E=100
    const smoke = node("smoke", { nParticlesPerSecond: 20, lifetime: 10 }, [debris]); // E=200
    // A: smoke=200; debris=200×100×(2/10)=4,000; sparks=4000×20×(0.5/2)=20,000. total=24,200.
    // Uniform rule would have been 200 + 20,000 + 400,000 = 420,200 (~17× larger).
    const total = estimateSystemLoad(syntheticRoot([smoke]));
    expect(total).toBeCloseTo(24_200, 6);
    expect(total).toBeLessThan(420_200);
  });

  it("an instantaneous (0-lifetime) parent clamps the death rate — finite, no Infinity", () => {
    // Burst parent with lifetime 0 has E>0 but L(parent)=0; the MIN_PARENT_LIFETIME
    // floor keeps the death factor finite.
    const child = node("child", { nParticlesPerSecond: 1, lifetime: 1 }, [], "death");
    const parent = node("flash", {
      useBursts: true, nParticlesPerBurst: 5, nBursts: 1, burstDelay: 1, lifetime: 0,
    }, [child]);
    const total = estimateSystemLoad(syntheticRoot([parent]));
    expect(Number.isFinite(total)).toBe(true);
  });
});

describe("formatChainWarning", () => {
  it("renders header + one line per generation with running product", () => {
    const leaf = node("smoke", { nParticlesPerSecond: 40, lifetime: 1 }, [], "death");
    const root = node("sparkle", { nParticlesPerSecond: 500, lifetime: 1 }, [leaf]);
    const w = estimateChainLoad(syntheticRoot([root])).get(root.stableId)!;
    const text = formatChainWarning(w);
    expect(text).toContain("20,000");
    expect(text.split("\n")).toHaveLength(3); // header + 2 generations
    expect(text).toContain("sparkle");
    expect(text).toContain("→ smoke");
  });
  it("sub-1 multipliers render with a decimal, not ×0", () => {
    // 50,000 × 0.4 = 20,000 — offending, with a sub-1 child multiplier.
    // (lifetime child so the multiplier is E=0.4 directly; a death child would
    // scale further by L(child)/L(parent) — covered separately below.)
    const child = node("dust", { nParticlesPerSecond: 2, lifetime: 0.2 }, [], "lifetime");
    const root = node("blast", { nParticlesPerSecond: 50_000, lifetime: 1 }, [child]);
    const w = estimateChainLoad(syntheticRoot([root])).get(child.stableId)!;
    const text = formatChainWarning(w);
    expect(text).toContain("×0.4");
    expect(text).not.toContain("×0 ");
  });
});
