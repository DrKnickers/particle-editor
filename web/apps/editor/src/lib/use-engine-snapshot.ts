// use-engine-snapshot.ts — subscribe a component to the engine state DTO.
//
// Seeds from a one-shot `engine/state/snapshot`, then tracks every
// `engine/state/changed` broadcast; returns null until the first snapshot
// resolves. Errors are SWALLOWED (the consumer degrades to its defaults) —
// this is the toolbar-dropdown contract. Do NOT add a console.warn here: the
// picker BODIES that warn keep their own richer subscriptions; this hook is
// only for the lightweight trigger dropdowns (BackgroundDropdown /
// GroundDropdown / ReferenceObjectDropdown), which had three byte-identical
// copies of this effect (DRY audit web-screens-0).

import { useEffect, useState } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";

export function useEngineSnapshot(bridge: Bridge): EngineStateDto | null {
  const [snap, setSnap] = useState<EngineStateDto | null>(null);
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) setSnap(s);
      })
      .catch(() => {
        /* ignore */
      });
    const off = bridge.on("engine/state/changed", (e) => setSnap(e.payload));
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);
  return snap;
}
