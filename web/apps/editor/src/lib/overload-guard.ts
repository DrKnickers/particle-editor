// overload-guard.ts — [guard-config] web side of the configurable preview
// overload guard. The WEB owns persistence (localStorage, the lib/theme.ts
// pattern); the engine is told via engine/set/overload-guard on every
// change AND once at app mount (App.tsx), so the saved setting applies at
// startup. enabled:false is fully uncapped — a power-user mode that CAN
// OOM the editor on extreme chain effects (the Preferences UI says so).
// The engine clamps defensively too; this clamp exists so the UI and
// localStorage never even hold a nonsense value.

import type { Bridge } from "@particle-editor/bridge-schema";

export type OverloadGuardConfig = { enabled: boolean; maxParticles: number };

// Frozen so the by-reference returns from readOverloadGuard() (the empty /
// corrupt / wrong-type paths) can't be mutated into a corrupted singleton.
export const OVERLOAD_GUARD_DEFAULT: OverloadGuardConfig = Object.freeze({
  enabled: true,
  maxParticles: 15_000,
});
export const MIN_MAX_PARTICLES = 1_000;
export const MAX_MAX_PARTICLES = 1_000_000;

const KEY = "alo:overload-guard";

export function clampMaxParticles(n: number): number {
  if (!Number.isFinite(n)) return OVERLOAD_GUARD_DEFAULT.maxParticles;
  return Math.min(MAX_MAX_PARTICLES, Math.max(MIN_MAX_PARTICLES, Math.round(n)));
}

export function readOverloadGuard(): OverloadGuardConfig {
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return OVERLOAD_GUARD_DEFAULT;
    const p = JSON.parse(raw) as Partial<OverloadGuardConfig>;
    if (typeof p.enabled !== "boolean" || typeof p.maxParticles !== "number") {
      return OVERLOAD_GUARD_DEFAULT;
    }
    return { enabled: p.enabled, maxParticles: clampMaxParticles(p.maxParticles) };
  } catch {
    return OVERLOAD_GUARD_DEFAULT;
  }
}

export function writeOverloadGuard(c: OverloadGuardConfig): void {
  localStorage.setItem(
    KEY,
    JSON.stringify({ enabled: c.enabled, maxParticles: clampMaxParticles(c.maxParticles) }),
  );
}

// Fire-and-forget: a failed send (mock quirk, host teardown) must never
// break the Preferences UI; the engine just keeps its previous config.
export function applyOverloadGuard(bridge: Bridge, c: OverloadGuardConfig): void {
  void bridge
    .request({
      kind: "engine/set/overload-guard",
      params: { enabled: c.enabled, maxParticles: clampMaxParticles(c.maxParticles) },
    })
    .catch(() => {});
}
