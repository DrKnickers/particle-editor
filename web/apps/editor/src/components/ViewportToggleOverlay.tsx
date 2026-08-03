// ViewportToggleOverlay — a quiet, in-context cluster of viewport display toggles
// anchored bottom-left of the viewport: ground · grid · bloom · reference-lock.
// It is the in-viewport home for these toggles (moved off the toolbar) and the
// discoverable way to lock the reference object so a left-drag pans/orbits instead
// of nudging the model. Reads the engine snapshot (like Toolbar/MenuBar) and writes
// the existing engine/set/* commands — no new state.
import { PanelBottom, Grid2x2, Sun, Lock, LockOpen } from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { useEngineField } from "@/lib/use-engine-snapshot";
import { pillScrimMode, pillBackdropColor } from "@/lib/colorref";
import { Tip } from "@/primitives/Tip";

type Props = { bridge: Bridge };

const ICON = { size: 16, strokeWidth: 2, "aria-hidden": true } as const;

type ViewportToggleState = Pick<
  EngineStateDto,
  | "ground"
  | "gridVisible"
  | "bloom"
  | "referenceObjectLocked"
  | "referenceObjectName"
  | "groundColor"
  | "background"
>;

const selectViewportToggleState = (s: EngineStateDto): ViewportToggleState => ({
  ground: s.ground,
  gridVisible: s.gridVisible,
  bloom: s.bloom,
  referenceObjectLocked: s.referenceObjectLocked,
  referenceObjectName: s.referenceObjectName,
  groundColor: s.groundColor,
  background: s.background,
});

const sameViewportToggleState = (a: ViewportToggleState, b: ViewportToggleState) =>
  a.ground === b.ground &&
  a.gridVisible === b.gridVisible &&
  a.bloom === b.bloom &&
  a.referenceObjectLocked === b.referenceObjectLocked &&
  a.referenceObjectName === b.referenceObjectName &&
  a.groundColor === b.groundColor &&
  a.background === b.background;

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
    // Tip is the app-wide tooltip primitive (replacing native `title`).
    // The inline-flex span carries the Tip so the lock's *disabled* state still
    // shows its "load a reference object" hint (a disabled button fires no
    // pointer events, so Radix can't trigger off it directly).
    <Tip content={label}>
      <span className="inline-flex">
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
          onClick={onClick}
        >
          {icon}
        </button>
      </span>
    </Tip>
  );
}

export function ViewportToggleOverlay({ bridge }: Props) {
  const state = useEngineField(bridge, selectViewportToggleState, sameViewportToggleState);

  const ground = state?.ground ?? false;
  const grid = state?.gridVisible ?? false;
  const bloom = state?.bloom ?? false;
  const locked = state?.referenceObjectLocked ?? false;
  const hasRef = (state?.referenceObjectName ?? "") !== "";

  // Adaptive scrim: tracks the colour behind the pill — the effective (host-
  // computed) ground colour (the solid colour, or a textured floor's averaged
  // colour) when the ground plane is shown, otherwise the viewport background.
  // Light chip over a dark/neutral backdrop, dark over a bright one. Defaults dark
  // until the snapshot resolves (no flash).
  const scrimMode = state == null ? "dark" : pillScrimMode(pillBackdropColor(state));

  return (
    <div
      className="vp-overlay pointer-events-auto"
      role="group"
      aria-label="Viewport display options"
      data-scrim={scrimMode}
    >
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
