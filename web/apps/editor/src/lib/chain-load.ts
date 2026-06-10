import type { EmitterTreeNode, SpawnParamsDto } from "@particle-editor/bridge-schema";

// soft chain warning. Advisory only — nothing in the editor blocks
// on this. Spec: docs/superpowers/specs/2026-06-10-chain-warning-design.md.

// Vanilla effects run tens-to-hundreds alive; the v1 chain-test bomb was
// millions. 10k flags genuinely explosive chains without nagging
// legitimate dense effects.
export const CHAIN_WARN_THRESHOLD = 10_000;

// Degenerate infinite-bursts-at-zero-delay is unbounded; clamp so the
// tooltip never shows Infinity/NaN. Far above the threshold, so the
// warning still fires.
const DEGENERATE_CAP = 1_000_000_000;

// Steady-state alive-particle estimate for ONE emitter (Little's law).
// Continuous: rate × lifetime. Burst: particles-per-burst × the number of
// bursts whose particles coexist (lifetime / burstDelay, capped by
// nBursts; nBursts === 0 means infinite).
export function estimatePerEmitter(s: SpawnParamsDto): number {
  // Clamped to ≥0 at the single exit: a corrupt DTO with two negative rates
  // along a chain would otherwise multiply into a positive spurious warning.
  let result: number;
  if (!s.useBursts) {
    result = s.nParticlesPerSecond * s.lifetime;
  } else if (s.burstDelay <= 0) {
    const infinite = s.nBursts === 0;
    result = infinite
      ? s.nParticlesPerBurst > 0 ? DEGENERATE_CAP : 0
      : s.nParticlesPerBurst * s.nBursts;
  } else {
    const infinite = s.nBursts === 0;
    const concurrent = Math.floor(s.lifetime / s.burstDelay) + 1;
    const bursts = infinite ? concurrent : Math.min(concurrent, s.nBursts);
    result = s.nParticlesPerBurst * Math.max(1, bursts);
  }
  return Math.max(0, result);
}

export type ChainWarning = {
  // Worst cumulative estimate among offending paths through this row.
  estimate: number;
  // Root→offender breakdown for the tooltip, one entry per generation.
  path: Array<{ name: string; perEmitter: number; cumulative: number }>;
};

// Walks the tree (synthetic root excluded) and returns stableId →
// ChainWarning for every row on a root→node path whose cumulative estimate
// crosses CHAIN_WARN_THRESHOLD. A(child) = A(parent) × E(child): every
// alive parent particle hosts one child-emitter instance. Life and death
// children deliberately share the rule — documented approximation, see
// spec §1.
export function estimateChainLoad(root: EmitterTreeNode): Map<number, ChainWarning> {
  const out = new Map<number, ChainWarning>();
  type TrailEntry = { stableId: number; name: string; perEmitter: number; cumulative: number };
  const visit = (node: EmitterTreeNode, parentCumulative: number, trail: TrailEntry[]): void => {
    const perEmitter = estimatePerEmitter(node.spawn);
    const cumulative = parentCumulative * perEmitter;
    const path = [...trail, { stableId: node.stableId, name: node.name, perEmitter, cumulative }];
    if (cumulative > CHAIN_WARN_THRESHOLD) {
      // One shared copy is safe: the display array is never mutated downstream.
      const display = path.map(({ name, perEmitter: e, cumulative: a }) => ({
        name, perEmitter: e, cumulative: a,
      }));
      for (const entry of path) {
        const prev = out.get(entry.stableId);
        if (prev === undefined || cumulative > prev.estimate) {
          out.set(entry.stableId, { estimate: cumulative, path: display });
        }
      }
    }
    node.children.forEach((c) => visit(c, cumulative, path));
  };
  root.children.forEach((c) => visit(c, 1, []));
  return out;
}

// Multi-line tooltip body (the native `title` attribute renders \n as
// line breaks).
export function formatChainWarning(w: ChainWarning): string {
  const fmt = (n: number) => Math.round(n).toLocaleString("en-US");
  // Sub-10 multipliers keep a decimal so e.g. ×0.4 doesn't render as ×0.
  const fmtSmall = (n: number) =>
    n >= 10 ? Math.round(n).toLocaleString("en-US") : n.toLocaleString("en-US", { maximumFractionDigits: 1 });
  const lines = w.path.map((p, i) =>
    i === 0
      ? `${p.name}: ~${fmtSmall(p.perEmitter)} alive`
      : `→ ${p.name}: ×${fmtSmall(p.perEmitter)} → ~${fmt(p.cumulative)}`,
  );
  return [
    `Soft warning: ~${fmt(w.estimate)} particles estimated alive through this chain`,
    ...lines,
  ].join("\n");
}
