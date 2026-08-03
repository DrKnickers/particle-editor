import { useEffect, useRef } from "react";
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { estimateSystemLoad } from "./chain-load";

// Epsilon below which an estimate change is considered noise and suppressed.
// Avoids bridge spam from floating-point drift while still catching any
// user-meaningful parameter edit (which shifts the estimate by at least 1).
const EPS = 0.5;

/** Recompute the system-total alive estimate on every tree update and
 *  push it to the engine (engine/set/estimated-load) when it changed —
 *  the engine multiplies by its live placed-instance count for the
 *  preemptive overload gate. Pushed here (where the ⚠ data is computed)
 *  so the gate's number equals the glyph's. Pushes on mount too;
 *  BridgeDispatcher caches + reapplies across SetEngine. */
export function useEstimatedLoadPush(
  bridge: Bridge,
  tree: { root: EmitterTreeNode } | null,
): void {
  const last = useRef<number | null>(null);
  useEffect(() => {
    if (tree === null) return;
    const perInstance = estimateSystemLoad(tree.root);
    if (last.current !== null && Math.abs(perInstance - last.current) < EPS) return;
    last.current = perInstance;
    void bridge
      .request({ kind: "engine/set/estimated-load", params: { perInstance } })
      .catch(() => {
        /* mock / no-engine: harmless */
      });
  }, [bridge, tree]);
}
