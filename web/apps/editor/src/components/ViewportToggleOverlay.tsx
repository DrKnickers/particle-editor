// ViewportToggleOverlay — a quiet, in-context cluster of viewport display toggles
// anchored bottom-left of the viewport: ground · grid · bloom · reference-lock.
// It is the in-viewport home for these toggles (moved off the toolbar) and the
// discoverable way to lock the reference object so a left-drag pans/orbits instead
// of nudging the model. Reads the engine snapshot (like Toolbar/MenuBar) and writes
// the existing engine/set/* commands — no new state.
import { useEffect, useState } from "react";
import { PanelBottom, Grid2x2, Sun, Lock, LockOpen } from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";

type Props = { bridge: Bridge };

const ICON = { size: 16, strokeWidth: 2, "aria-hidden": true } as const;

function ToggleButton(props: {
  label: string;
  active: boolean;
  disabled?: boolean;
  lock?: boolean;
  icon: React.ReactNode;
  onClick: () => void;
}) {
  const { label, active, disabled, lock, icon, onClick } = props;
  return (
    <button
      type="button"
      className={
        "vp-overlay-btn" +
        (active ? " vp-overlay-btn--active" : "") +
        (lock && active ? " vp-overlay-btn--lock" : "")
      }
      aria-label={label}
      aria-pressed={active}
      disabled={disabled}
      title={label}
      onClick={onClick}
    >
      {icon}
    </button>
  );
}

export function ViewportToggleOverlay({ bridge }: Props) {
  const [state, setState] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        // Seed only — a live `engine/state/changed` may have arrived before this
        // initial fetch resolved; don't clobber it with the (now stale) snapshot.
        if (!cancelled) setState((prev) => prev ?? s);
      })
      .catch(() => {});
    const off = bridge.on("engine/state/changed", (e) => setState(e.payload));
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);

  const ground = state?.ground ?? false;
  const grid = state?.gridVisible ?? false;
  const bloom = state?.bloom ?? false;
  const locked = state?.referenceObjectLocked ?? false;
  const hasRef = (state?.referenceObjectName ?? "") !== "";

  return (
    <div className="vp-overlay pointer-events-auto" role="group" aria-label="Viewport display options">
      <ToggleButton
        label="Show ground"
        active={ground}
        icon={<PanelBottom {...ICON} />}
        onClick={() => {
          void bridge.request({ kind: "engine/set/ground", params: { enabled: !ground } }).catch(() => {});
        }}
      />
      <ToggleButton
        label="Show grid"
        active={grid}
        icon={<Grid2x2 {...ICON} />}
        onClick={() => {
          void bridge.request({ kind: "engine/set/grid-visible", params: { visible: !grid } }).catch(() => {});
        }}
      />
      <ToggleButton
        label="Toggle bloom"
        active={bloom}
        icon={<Sun {...ICON} />}
        onClick={() => {
          void bridge.request({ kind: "engine/set/bloom", params: { enabled: !bloom } }).catch(() => {});
        }}
      />
      <span className="vp-overlay-sep" aria-hidden />
      <ToggleButton
        label={hasRef ? "Lock reference object placement" : "Load a reference object to lock it"}
        active={locked}
        disabled={!hasRef}
        lock
        icon={locked ? <Lock {...ICON} /> : <LockOpen {...ICON} />}
        onClick={() => {
          if (hasRef)
            void bridge
              .request({ kind: "engine/set/reference-object-lock", params: { locked: !locked } })
              .catch(() => {});
        }}
      />
    </div>
  );
}
