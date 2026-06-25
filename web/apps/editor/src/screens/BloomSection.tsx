// BloomSection — bloom enable + strength / cutoff / size, rendered as a
// `ToolPanel.Section` inside the docked Lighting pane. The standalone
// BloomPanel was folded into Lighting (one right-dock slot);
// this component carries the former BloomPanel's logic so LightingPanel
// stays focused on the lights.
//
// Owns its own engine-state subscription (snapshot on mount +
// engine/state/changed) so it stays self-contained — the parent panel
// doesn't have to thread bloom state through. Mirrors the legacy
// `BloomDlgProc` (legacy Win32 UI, removed in) for the React UI.
//
// Bridge surface (existing — zero schema additions):
//   - engine/set/bloom            { enabled }
//   - engine/set/bloom-strength   { v }
//   - engine/set/bloom-cutoff     { v }
//   - engine/set/bloom-size       { v }
//   - engine/query/bloom-available () -> boolean

import { useEffect, useState } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { ToolPanel } from "@/components/ToolPanel";

type Props = {
  bridge: Bridge;
  /** Collapsed by default (it sits below the always-visible lighting
   *  sections); pass true to open it on mount. */
  defaultOpen?: boolean;
};

export function BloomSection({ bridge, defaultOpen = false }: Props) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);
  const [available, setAvailable] = useState<boolean>(true);

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) setSnapshot(s);
      })
      .catch((err) => console.warn("[BloomSection] snapshot failed:", err));
    bridge
      .request({ kind: "engine/query/bloom-available", params: {} })
      .then((v) => {
        if (!cancelled) setAvailable(v);
      })
      .catch((err) => console.warn("[BloomSection] bloom-available failed:", err));
    const off = bridge.on("engine/state/changed", (e) => {
      setSnapshot(e.payload);
      // Re-derive availability from the snapshot; the dedicated query
      // fires once on mount, subsequent changes ride the state event.
      setAvailable(e.payload.bloomAvailable);
    });
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);

  const enabled = snapshot?.bloom ?? false;
  const strength = snapshot?.bloomStrength ?? 0;
  const cutoff = snapshot?.bloomCutoff ?? 0;
  const size = snapshot?.bloomSize ?? 0;

  const disabled = !available;

  const setEnabled = (v: boolean) => {
    void bridge.request({ kind: "engine/set/bloom", params: { enabled: v } });
  };
  const setStrength = (v: number) => {
    void bridge.request({ kind: "engine/set/bloom-strength", params: { v } });
  };
  const setCutoff = (v: number) => {
    void bridge.request({ kind: "engine/set/bloom-cutoff", params: { v } });
  };
  const setSize = (v: number) => {
    void bridge.request({ kind: "engine/set/bloom-size", params: { v } });
  };

  return (
    <ToolPanel.Section title="Bloom" defaultOpen={defaultOpen}>
      <label className="flex items-center gap-2 text-xs text-text">
        <input
          type="checkbox"
          checked={enabled}
          onChange={(e) => setEnabled(e.target.checked)}
          disabled={disabled}
          aria-label="Enable bloom"
          className="size-3 accent-[var(--accent-strong)] disabled:opacity-40 disabled:cursor-not-allowed"
        />
        <span>Enable bloom</span>
      </label>
      {disabled && (
        <p className="text-[10px] text-text-3">
          (Bloom is not supported on this device.)
        </p>
      )}
      <ToolPanel.Row label="Strength">
        <Spinner
          value={strength}
          onChange={setStrength}
          min={0}
          max={5}
          step={0.05}
          disabled={disabled}
          aria-label="Bloom strength"
        />
      </ToolPanel.Row>
      <ToolPanel.Row label="Cutoff">
        <Spinner
          value={cutoff}
          onChange={setCutoff}
          min={0}
          max={1}
          step={0.01}
          disabled={disabled}
          aria-label="Bloom cutoff"
        />
      </ToolPanel.Row>
      <ToolPanel.Row label="Size">
        <Spinner
          value={size}
          onChange={setSize}
          min={0}
          max={32}
          step={0.5}
          disabled={disabled}
          aria-label="Bloom size"
        />
      </ToolPanel.Row>
    </ToolPanel.Section>
  );
}
