import { describe, expect, it } from "vitest";
import type { EmitterTreeNode, SpawnParamsDto } from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import {
  CHAIN_WARN_THRESHOLD,
  estimateChainLoad,
  estimatePerEmitter,
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
    const child = node("dust", { nParticlesPerSecond: 2, lifetime: 0.2 }, [], "death");
    const root = node("blast", { nParticlesPerSecond: 50_000, lifetime: 1 }, [child]);
    const w = estimateChainLoad(syntheticRoot([root])).get(child.stableId)!;
    const text = formatChainWarning(w);
    expect(text).toContain("×0.4");
    expect(text).not.toContain("×0 ");
  });
});
