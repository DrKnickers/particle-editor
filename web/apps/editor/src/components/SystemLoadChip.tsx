// SystemLoadChip — predictive system-total overload warning at the top of
// the emitter tree (overload-indicator-consistency spec, Part 2).
//
// The per-row ⚠ glyph is per-emitter / per-single-instance; the #138 gate
// compares SYSTEM total × placed-instance count. This chip covers the two
// multipliers the glyph can't: visible exactly when the NEXT spawn attempt
// would be refused — (instances + 1) × systemLoad > cap, guard enabled.
// (Current-placed-state semantics would be self-erasing: the engine
// CLEARS any over-cap placed state via the edit-time check, so the only
// persistent over-cap state is instances = 0.)
//
// `instances` comes from the 4 Hz stats/tick — subscribing HERE confines
// the 4 Hz re-render to this leaf instead of the whole tree. Browser
// MockBridge emits no stats/tick → instances stays 0 → the chip still
// works as a per-instance authoring signal in browser dev.
//
// Styling: amber-tinted band + normal text colour (readable in both
// themes for free — the #121 light-mode amber-text lesson), with the
// TriangleAlert in the same amber as the per-row glyph.
import { useEffect, useState } from "react";
import { TriangleAlert } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { fmtCount } from "@/lib/chain-load";
import { useOverloadGuardConfig } from "@/lib/overload-guard";

export function SystemLoadChip({
  bridge,
  systemLoad,
}: {
  bridge: Bridge;
  // estimateSystemLoad(tree.root) — computed by EmitterTree's memo, the
  // same value useEstimatedLoadPush pushes to the engine.
  systemLoad: number;
}) {
  const guard = useOverloadGuardConfig();
  const [instances, setInstances] = useState(0);
  useEffect(() => {
    const off = bridge.on("stats/tick", (e) => setInstances(e.payload.instances));
    return off;
  }, [bridge]);
  // A zero-load effect (no emitters yet, or all disabled) has nothing to warn about.
  if (!guard.enabled || systemLoad <= 0) return null;
  const projected = (instances + 1) * systemLoad;
  if (projected <= guard.maxParticles) return null;
  return (
    <div
      role="status"
      data-testid="system-load-chip"
      className="mb-1 flex shrink-0 items-center gap-1.5 rounded-sm bg-warning/15 px-2 py-1 text-xs text-text-2"
    >
      <TriangleAlert className="size-3.5 shrink-0 text-amber-400" aria-hidden />
      <span className="tabular-nums">
        {instances === 0 ? (
          <>This effect ≈ {fmtCount(systemLoad)} particles — over the {fmtCount(guard.maxParticles)} preview limit</>
        ) : (
          <>Another instance would exceed the preview limit (≈ {fmtCount(projected)} of {fmtCount(guard.maxParticles)})</>
        )}
      </span>
    </div>
  );
}
