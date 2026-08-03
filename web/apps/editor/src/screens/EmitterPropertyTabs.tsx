// EmitterPropertyTabs — lower-left quadrant of the four-quadrant layout.
// Three tabs (Basic / Appearance / Physics)
// driven by Radix Tabs. The Basic tab is fully wired this batch — 18
// form fields commit via `emitters/set-properties { id, patch: { ... } }`.
// Appearance + Physics tabs render "coming soon"
// placeholders; the schema-side DTO carries every field they need.
//
// Replaces the legacy `src/UI/Emitter.cpp` modal (873 LOC, ~150 control
// IDs). Mirrors the legacy tab structure 1:1: Basic / Appearance /
// Physics.
//
// Bridge surface:
//   - On selection change + on `emitters/tree/changed`: fetch via
//     `emitters/get-properties { id }`.
//   - Each field commit: `emitters/set-properties { id, patch: { ... } }`.
//
// Optimistic local update: each commit also applies the patch to local
// `properties` state immediately so the form doesn't flash on
// round-trip. A late-arriving `tree/changed` re-fetch is authoritative.
//
// `useBursts` mutex enabling (mirrors legacy):
//   - `useBursts === true` enables nBursts / burstDelay / nParticlesPerBurst
//     and disables nParticlesPerSecond.
//   - `useBursts === false` enables nParticlesPerSecond and disables
//     nBursts / burstDelay / nParticlesPerBurst.
//
// `randomRotation` enabling: when false, randomRotationDirection /
// Average / Variance disable.
//
// Text input (name) commits on blur — avoids per-keystroke bridge spam.
// Spinners commit per their existing semantics (Enter / blur / arrow /
// wheel / drag-release). Checkboxes commit on change.

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import * as Tabs from "@radix-ui/react-tabs";
import * as Checkbox from "@radix-ui/react-checkbox";
import * as Select from "@radix-ui/react-select";
import { Check, ChevronDown, FolderOpen, LayoutGrid } from "lucide-react";
import { TexturePalettePopover } from "@/screens/TexturePalettePopover";
import type {
  Bridge,
  EmitterPropertiesDto,
  GroupDto,
  Vec3,
  Vec4,
} from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { Tip } from "@/primitives/Tip";
import { Section } from "@/components/Section";
import { requestTreeRefetch } from "@/lib/tree-refetch";

// Blend mode dropdown options — mirrors the legacy `BlendModes[]` table
// at [src/UI/Emitter.cpp:20-31]. The engine has additional blend mode
// values (8, 9, 10, 13) but the legacy UI doesn't expose them via the
// dropdown — keep parity here.
const BLEND_MODE_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "None" },
  { value: 1, label: "Additive" },
  { value: 2, label: "Transparent" },
  { value: 3, label: "Inverse" },
  { value: 4, label: "Depth additive" },
  { value: 5, label: "Depth transparent" },
  { value: 6, label: "Depth inverse" },
  { value: 7, label: "Diffuse transparent" },
  { value: 11, label: "Bump map" },
  { value: 12, label: "Decal bump map" },
];

// BLEND_BUMP (==11) forces face-camera orientation. Mirrors the legacy
// `forceFace = (emitter->blendMode == ParticleSystem::BLEND_BUMP)`
// at [src/UI/Emitter.cpp:167] — only the BLEND_BUMP value triggers
// the cascade, not BLEND_DECAL_BUMPMAP.
const BLEND_BUMP = 11;

// Ground-interaction dropdown options — mirrors the legacy
// `GroundBehaviors[]` table at [src/UI/Emitter.cpp:35-40]. Values are
// the engine enum index (0..3); the `IDS_GROUND_BEHAVIOR_BOUNCE`
// string-id is the 3rd entry (value 2), and legacy cascades enable
// `bounciness` only when this value is picked
// (see [src/UI/Emitter.cpp:190]).
const GROUND_BEHAVIOR_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "None" },
  { value: 1, label: "Disappear" },
  { value: 2, label: "Bounce" },
  { value: 3, label: "Stick" },
];
const GROUND_BEHAVIOR_BOUNCE = 2;

// Emit-from-mesh dropdown options — mirrors the legacy
// `EmitModes[]` table at [src/UI/Emitter.cpp:44-49]. Values match
// `ParticleSystem::EMIT_*` constants at [src/ParticleSystem.h:66-69]:
// EMIT_DISABLE=0, EMIT_RANDOM_VERTEX=1, EMIT_RANDOM_MESH=2,
// EMIT_EVERY_VERTEX=3.
const EMIT_FROM_MESH_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Disable" },
  { value: 1, label: "Random Vertex" },
  { value: 2, label: "Random Mesh" },
  { value: 3, label: "Every Vertex" },
];
const EMIT_FROM_MESH_DISABLE = 0;

// Random-Param group type dropdown options — mirrors the engine
// `GT_*` constants at [src/ParticleSystem.h:20-24]: GT_EXACT=0,
// GT_BOX=1, GT_CUBE=2, GT_SPHERE=3, GT_CYLINDER=4.
const GROUP_TYPE_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Exact" },
  { value: 1, label: "Box" },
  { value: 2, label: "Cube" },
  { value: 3, label: "Sphere" },
  { value: 4, label: "Cylinder" },
];
const GT_EXACT = 0;
const GT_BOX = 1;
const GT_CUBE = 2;
const GT_SPHERE = 3;
const GT_CYLINDER = 4;

// Random-param group ordering — `EmitterPropertiesDto.groups` is the
// on-wire projection of `ParticleSystem::Emitter::groups[NUM_GROUPS]`.
// Engine constants at [src/ParticleSystem.h:28-30]:
//   GROUP_SPEED    = 0  → "Initial speed"   (rendered in PhysicsTab)
//   GROUP_LIFETIME = 1  → "Lifetime"        (NOT rendered; schema
//                                            retained for round-trip
//                                            fidelity)
//   GROUP_POSITION = 2  → "Initial position" (rendered in PhysicsTab)
// PhysicsTab indexes groups[0] and groups[2] explicitly; index 1 is
// preserved on the wire but absent from the inspector.

type Props = {
  bridge: Bridge;
};

export function EmitterPropertyTabs({ bridge }: Props) {
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [properties, setProperties] = useState<EmitterPropertiesDto | null>(null);
  // Discard stale responses if selection changes mid-flight.
  const inFlightFor = useRef<number | null>(null);

  // Seed selection from the engine snapshot.
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((snap) => {
        if (cancelled) return;
        setSelectedId(snap.selectedEmitterId);
      })
      .catch(() => { /* placeholder branch handles null */ });
    return () => { cancelled = true; };
  }, [bridge]);

  // Track live selection.
  useEffect(() => {
    const off = bridge.on("emitters/selected", (e) => {
      setSelectedId(e.payload.id);
    });
    return off;
  }, [bridge]);

  // Fetch helper. Discards responses for stale selection.
  const fetchProps = useCallback(
    (id: number | null, coalesceTreeRefetch = false) => {
      if (id === null) {
        setProperties(null);
        inFlightFor.current = null;
        return;
      }
      inFlightFor.current = id;
      const req = { kind: "emitters/get-properties", params: { id } } as const;
      const request = coalesceTreeRefetch ? requestTreeRefetch(bridge, req) : bridge.request(req);
      request
        .then((res) => {
          if (inFlightFor.current !== id) return;
          setProperties(res.properties);
        })
        .catch(() => {
          if (inFlightFor.current !== id) return;
          setProperties(null);
        });
    },
    [bridge],
  );

  // Re-fetch on selection change.
  useEffect(() => {
    fetchProps(selectedId);
  }, [fetchProps, selectedId]);

  // Re-fetch on tree mutations.
  useEffect(() => {
    const off = bridge.on("emitters/tree/changed", () => {
      fetchProps(selectedId, true);
    });
    return off;
  }, [bridge, fetchProps, selectedId]);

  // Commit helper — fires the bridge patch + optimistic local update.
  const commit = useCallback(
    (patch: Partial<EmitterPropertiesDto>) => {
      if (selectedId === null) return;
      // Optimistic local update so the spinner doesn't flash back to
      // the old value before the engine re-emits.
      setProperties((p) => (p === null ? p : { ...p, ...patch }));
      void bridge
        .request({
          kind: "emitters/set-properties",
          params: { id: selectedId, patch },
        })
        .catch(() => {
          // On failure, re-fetch the authoritative value so we don't
          // leave the form stuck on a value the engine refused.
          fetchProps(selectedId);
        });
    },
    [bridge, selectedId, fetchProps],
  );

  // Browse helper — opens the host-side native
  // texture dialog and resolves to the picked basename ("" if cancelled
  // or in browser/mock mode). TexturePickerField commits a non-empty
  // result through `commit`, same as the text input.
  const browseTexture = useCallback(
    async (slot: "color" | "bump"): Promise<string> => {
      try {
        const res = await bridge.request({
          kind: "textures/browse",
          params: { slot },
        });
        return res.filename ?? "";
      } catch {
        return "";
      }
    },
    [bridge],
  );

  // The tab strip is always mounted so the user can see the
  // Basic/Appearance/Physics structure (and pre-click a tab) before any
  // emitter is selected. The per-Content `renderBody` helper swaps in a
  // placeholder when no selection / loading, so only the active tab's
  // body shows the placeholder — three call sites, never duplicated.
  const renderBody = (content: (p: EmitterPropertiesDto) => ReactNode): ReactNode => {
    if (selectedId === null) {
      return (
        <div
          data-testid="emitter-property-tabs-placeholder"
          className="flex h-full items-center justify-center p-4 text-center text-xs text-text-3"
        >
          Select an emitter to edit its properties
        </div>
      );
    }
    if (properties === null) {
      return (
        <div className="flex h-full items-center justify-center p-4 text-xs text-text-3">
          Loading…
        </div>
      );
    }
    return content(properties);
  };

  return (
    <Tabs.Root
      data-testid="emitter-property-tabs"
      defaultValue="basic"
      className="flex h-full flex-col"
    >
      <Tabs.List
        className="flex shrink-0 border-b border-border bg-bg"
        aria-label="Emitter property tabs"
      >
        <TabTrigger value="basic" label="Basic" />
        <TabTrigger value="appearance" label="Appearance" />
        <TabTrigger value="physics" label="Physics" />
      </Tabs.List>
      {/* All three tabs render <div className="inspector"> inside, which
          owns the padding — so the Tabs.Content wrappers omit Tailwind
          padding to avoid doubling. */}
      <Tabs.Content
        value="basic"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-basic-content"
      >
        {renderBody((p) => <BasicTab properties={p} onCommit={commit} />)}
      </Tabs.Content>
      <Tabs.Content
        value="appearance"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-appearance-content"
      >
        {renderBody((p) => (
          <AppearanceTab
            properties={p}
            onCommit={commit}
            onBrowseTexture={browseTexture}
            bridge={bridge}
          />
        ))}
      </Tabs.Content>
      <Tabs.Content
        value="physics"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-physics-content"
      >
        {renderBody((p) => <PhysicsTab properties={p} onCommit={commit} />)}
      </Tabs.Content>
    </Tabs.Root>
  );
}

function TabTrigger({ value, label }: { value: string; label: string }) {
  return (
    <Tabs.Trigger
      value={value}
      data-testid={`tab-trigger-${value}`}
      className="flex-1 cursor-pointer border-b-2 border-transparent px-3 py-2 text-xs text-text-2 transition motion-reduce:transition-none data-[state=active]:border-accent data-[state=active]:text-text hover:text-text focus-ring"
    >
      {label}
    </Tabs.Trigger>
  );
}

// ─── Basic tab ──────────────────────────────────────────────────────

// Tri-state Generation mode — declared at module scope so MODE_ORDER /
// navigate can reference the type without re-declaring it inside the
// component body.
type GenerationMode = "bursts" | "continuous" | "weather";

// Roving-tabindex navigation order for the Generation radiogroup. The
// arrow handlers cycle through this list (ArrowUp = -1, ArrowDown = +1)
// with modulo wrap so Bursts ↔ Continuous ↔ Weather is a closed loop.
const MODE_ORDER: GenerationMode[] = ["bursts", "continuous", "weather"];
const navigate = (from: GenerationMode, dir: -1 | 1): GenerationMode => {
  const idx = MODE_ORDER.indexOf(from);
  const next = (idx + dir + MODE_ORDER.length) % MODE_ORDER.length;
  return MODE_ORDER[next];
};

// Local RadioRow helper — captures the per-radio chrome (role,
// aria-checked, roving tabIndex, the dot+label spans, and the
// keyboard handler) so each radio site in BasicTab is a five-line
// usage instead of a 17-line block. ArrowUp/ArrowDown delegate to
// `onArrowNav` (`-1` for previous, `+1` for next); Enter/Space
// preserve the existing selection behaviour.
function RadioRow({
  checked,
  label,
  tabIndex,
  onSelect,
  onArrowNav,
  testId,
}: {
  checked: boolean;
  label: string;
  tabIndex: number;
  onSelect: () => void;
  /** Called with -1 for ArrowUp (previous), +1 for ArrowDown (next). */
  onArrowNav: (direction: -1 | 1) => void;
  /** Optional data-testid on the radio row (record-cursor / a11y drivers). */
  testId?: string;
}) {
  return (
    <div
      role="radio"
      aria-checked={checked}
      tabIndex={tabIndex}
      className="radio-row"
      data-testid={testId}
      onClick={onSelect}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          onSelect();
        } else if (e.key === "ArrowUp") {
          e.preventDefault();
          onArrowNav(-1);
        } else if (e.key === "ArrowDown") {
          e.preventDefault();
          onArrowNav(1);
        }
      }}
    >
      <span className="radio-dot" />
      <span>{label}</span>
    </div>
  );
}

export function BasicTab({
  properties,
  onCommit,
}: {
  properties: EmitterPropertiesDto;
  onCommit: (patch: Partial<EmitterPropertiesDto>) => void;
}) {
  // Tri-state Generation mode derived from (useBursts, isWeatherParticle).
  // Legacy parity: weather wins when set (the legacy UI surfaces weather
  // mode regardless of useBursts), then bursts vs continuous splits on
  // the remaining axis. The atomic-patch rationale —
  // each setMode call fires ONE patch carrying both keys so
  // the engine never observes a transient inconsistent state pair.
  const mode: GenerationMode = properties.isWeatherParticle
    ? "weather"
    : properties.useBursts
      ? "bursts"
      : "continuous";

  const setMode = (next: GenerationMode) => {
    switch (next) {
      case "bursts":     onCommit({ useBursts: true, isWeatherParticle: false }); break;
      case "continuous": onCommit({ useBursts: false, isWeatherParticle: false }); break;
      // Weather only sets isWeatherParticle — useBursts is preserved so
      // toggling weather off returns the user to whichever non-weather
      // mode they came from. Matches legacy IDC_RADIO_WEATHER behaviour.
      case "weather":    onCommit({ isWeatherParticle: true }); break;
    }
  };

  // Roving tabIndex — only the active mode is in the tab cycle so
  // shift+Tab doesn't have to step through three radios to escape
  // the group. Matches the WAI-ARIA radio group pattern.
  const tabIndexFor = (m: GenerationMode) => (m === mode ? 0 : -1);

  const burstsEnabled = mode === "bursts";
  const continuousEnabled = mode === "continuous";
  const weatherEnabled = mode === "weather";
  return (
    <div className="inspector basic-tab">
      {/* Name row — custom 60px 1fr grid per design source's
          left_panel.jsx:100. Outside any Section so it always
          shows at the top of the tab. */}
      <div className="form-row name-row">
        <span className="lbl">Name</span>
        <FieldText
          value={properties.name}
          onCommit={(v) => onCommit({ name: v })}
          label="Name"
          wide
          testId="emitter-name-input"
        />
      </div>

      <Section title="Emitter Timing">
        <FieldSpinner
          label="Initial spawn delay:"
          value={properties.initialDelay}
          min={0}
          step={0.1}
          decimals={2}
          unit="s"
          onCommit={(v) => onCommit({ initialDelay: v })}
        />
        <FieldSpinner
          label="Skip time:"
          value={properties.skipTime}
          min={0}
          step={0.1}
          decimals={2}
          unit="s"
          onCommit={(v) => onCommit({ skipTime: v })}
        />
        <FieldSpinner
          label="Freeze time:"
          value={properties.freezeTime}
          min={0}
          step={0.1}
          decimals={2}
          unit="s"
          onCommit={(v) => onCommit({ freezeTime: v })}
        />
      </Section>

      <Section title="Generation">
        {/* Hand-rolled radio rows (not Radix RadioGroup)
            — keeps the visual fidelity tight to the legacy three-row
            stack while still being keyboard-accessible. The wrapper
            div carries role="radiogroup" so screen readers announce
            the three radios as a group; the inner FieldSpinner
            sub-fields aren't role="radio" and so don't interfere with
            ARIA semantics. Roving tabIndex + ArrowUp/ArrowDown matches
            the WAI-ARIA radio group pattern. */}
        <div role="radiogroup" aria-label="Generation mode">
          <RadioRow
            checked={burstsEnabled}
            label="Bursts"
            tabIndex={tabIndexFor("bursts")}
            onSelect={() => setMode("bursts")}
            onArrowNav={(d) => setMode(navigate("bursts", d))}
            testId="radio-bursts"
          />
          <FieldSpinner
            label="Bursts:"
            value={properties.nBursts}
            min={1}
            step={1}
            decimals={0}
            disabled={!burstsEnabled}
            testId="spinner-n-bursts"
            onCommit={(v) => onCommit({ nBursts: Math.round(v) })}
          />
          <FieldSpinner
            label="Burst delay:"
            value={properties.burstDelay}
            min={0}
            step={0.1}
            decimals={2}
            unit="s"
            disabled={!burstsEnabled}
            testId="spinner-burst-delay"
            onCommit={(v) => onCommit({ burstDelay: v })}
          />
          <FieldSpinner
            label="Particles/burst:"
            value={properties.nParticlesPerBurst}
            min={1}
            step={1}
            decimals={0}
            disabled={!burstsEnabled}
            testId="spinner-particles-per-burst"
            onCommit={(v) => onCommit({ nParticlesPerBurst: Math.round(v) })}
          />

          <RadioRow
            checked={continuousEnabled}
            label="Continuous stream"
            tabIndex={tabIndexFor("continuous")}
            onSelect={() => setMode("continuous")}
            onArrowNav={(d) => setMode(navigate("continuous", d))}
          />
          <FieldSpinner
            label="Particles/second:"
            value={properties.nParticlesPerSecond}
            min={0}
            step={1}
            decimals={0}
            disabled={!continuousEnabled}
            testId="spinner-particles-per-second"
            onCommit={(v) => onCommit({ nParticlesPerSecond: Math.round(v) })}
          />

          <RadioRow
            checked={weatherEnabled}
            label="Weather particle"
            tabIndex={tabIndexFor("weather")}
            onSelect={() => setMode("weather")}
            onArrowNav={(d) => setMode(navigate("weather", d))}
          />
          {/* NOTE: Continuous and Weather both bind to nParticlesPerSecond
              but carry distinct aria-labels ("Particles/second:" vs
              "Particles:") so getByLabelText still distinguishes them. */}
          <FieldSpinner
            label="Particles:"
            value={properties.nParticlesPerSecond}
            min={0}
            step={1}
            decimals={0}
            disabled={!weatherEnabled}
            onCommit={(v) => onCommit({ nParticlesPerSecond: Math.round(v) })}
          />
          <FieldSpinner
            label="Distance from camera:"
            value={properties.weatherCubeDistance}
            min={0}
            step={0.1}
            unit="units"
            disabled={!weatherEnabled}
            onCommit={(v) => onCommit({ weatherCubeDistance: v })}
          />
          <FieldSpinner
            label="Cube size:"
            value={properties.weatherCubeSize}
            min={0}
            step={0.1}
            unit="units"
            disabled={!weatherEnabled}
            onCommit={(v) => onCommit({ weatherCubeSize: v })}
          />
        </div>

        {/* Lifetime fields moved here from Emitter Timing to match
            legacy IDD_EMITTER_PROPS1 (.rc:449,461,466). Minimum lifetime
            uses displayInvertedPercent: the stored ratio (0..1) displays
            as `100 - val*100` rounded — matches IDC_SPINNER14 at
            [Emitter.cpp:487,795]. */}
        <FieldSpinner
          label="Maximum lifetime:"
          value={properties.lifetime}
          min={0}
          step={0.1}
          decimals={2}
          unit="s"
          testId="spinner-max-lifetime"
          onCommit={(v) => onCommit({ lifetime: v })}
        />
        <FieldSpinner
          label="Minimum lifetime:"
          value={properties.randomLifetimePerc}
          displayInvertedPercent
          unit="%"
          onCommit={(v) => onCommit({ randomLifetimePerc: v })}
        />

      </Section>

      <Section title="Connection">
        <FieldCheckbox
          label="Link particles to instance"
          checked={properties.linkToSystem}
          onCheckedChange={(v) => onCommit({ linkToSystem: v })}
          inlineLabel
          testId="basic-link-to-system"
        />
        <FieldSelect
          label="Emit mode:"
          value={properties.emitFromMesh}
          options={EMIT_FROM_MESH_OPTIONS}
          onCommit={(v) => onCommit({ emitFromMesh: v })}
          testId="basic-emit-from-mesh-trigger"
          widthBoost="x2"
        />
        <FieldSpinner
          label="Emit offset:"
          value={properties.emitFromMeshOffset}
          step={0.1}
          unit="units"
          disabled={properties.emitFromMesh === EMIT_FROM_MESH_DISABLE}
          onCommit={(v) => onCommit({ emitFromMeshOffset: v })}
        />
      </Section>
    </div>
  );
}

// ─── Field row primitives ──────────────────────────────────────────
//
// Form rows use the design's `.form-row` 3-column grid
// (label / input / unit) from components.css. The optional third
// column carries the unit hint (e.g. "s", "%"); empty for fields
// that don't have one.

function FieldText({
  label,
  value,
  onCommit,
  wide,
  testId,
}: {
  label: string;
  value: string;
  onCommit: (value: string) => void;
  /** When true, render just the <input> (no .form-row wrapper, no
   *  label span). Caller owns the outer row container and the label.
   *  Used by the Name row, which needs the design source's custom
   *  60px 1fr grid template. */
  wide?: boolean;
  /** Optional data-testid on the <input> (record-cursor / a11y drivers). */
  testId?: string;
}) {
  // Local text state so the user can type freely; commit on blur or
  // Enter to avoid per-keystroke bridge spam.
  const [text, setText] = useState(value);
  const lastProp = useRef(value);
  // Sync from prop when external value changes (and we're not editing).
  if (lastProp.current !== value) {
    lastProp.current = value;
    setText(value);
  }
  const input = (
    <input
      type="text"
      value={text}
      onChange={(e) => setText(e.target.value)}
      onBlur={() => {
        if (text !== value) onCommit(text);
      }}
      onKeyDown={(e) => {
        if (e.key === "Enter") {
          (e.currentTarget as HTMLInputElement).blur();
        } else if (e.key === "Escape") {
          setText(value);
          (e.currentTarget as HTMLInputElement).blur();
        }
      }}
      className="text-input"
      aria-label={label}
      spellCheck={false}
      autoComplete="off"
      data-testid={testId}
    />
  );
  if (wide) {
    return input;
  }
  return (
    <div className="form-row form-row-text">
      <span className="lbl">{label}</span>
      {input}
    </div>
  );
}

// TexturePickerField — a texture filename field (color or bump) with a
// Browse button that opens the host-side native file dialog. Reuses
// FieldText (wide mode = bare input) for the manual-entry + commit-on-
// blur behaviour, and adds the Browse button. `onBrowse(slot)` resolves
// to the picked basename (or "" if cancelled); a non-empty result is
// committed via the same `onCommit` the text input uses.
// The palette button opens the frequently-used
// texture palette (TexturePalettePopover). Every non-empty commit — manual
// blur, Browse, or palette apply — funnels through `commit`, which also
// fires `textures/palette/touch-recent` so recents stay warm (legacy
// parity with Emitter.cpp's three TouchRecent sites).
export function TexturePickerField({
  label,
  value,
  slot,
  onCommit,
  onBrowse,
  bridge,
}: {
  label: string;
  value: string;
  slot: "color" | "bump";
  onCommit: (value: string) => void;
  onBrowse: (slot: "color" | "bump") => Promise<string>;
  bridge: Bridge;
}) {
  const [busy, setBusy] = useState(false);

  // Single commit funnel: apply the value, then record it as used so it
  // lands in the per-mod recents. Empty values (cancelled Browse) neither
  // commit nor track.
  const commit = useCallback(
    (next: string) => {
      onCommit(next);
      if (next) {
        void bridge
          .request({
            kind: "textures/palette/touch-recent",
            params: { filename: next, slot },
          })
          .catch(() => {
            /* tracking is best-effort; never block the commit */
          });
      }
    },
    [bridge, onCommit, slot],
  );

  const handleBrowse = async () => {
    if (busy) return;
    setBusy(true);
    try {
      const picked = await onBrowse(slot);
      if (picked) commit(picked);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="form-row form-row-texture">
      <span className="lbl">{label}</span>
      <FieldText wide label={label} value={value} onCommit={commit} />
      <div className="texture-btns">
        <Tip content="Browse for a texture file" side="left">
          <button
            type="button"
            className="btn-texture-browse"
            onClick={handleBrowse}
            disabled={busy}
            aria-label={`Browse for ${label}`}
          >
            <FolderOpen size={14} aria-hidden="true" />
          </button>
        </Tip>
        {/* The tooltip is rendered by TexturePalettePopover (via its `tip`
            prop) so the Tooltip.Trigger wraps the Popover.Trigger — the
            Radix-blessed nesting; a Tip wrapped around the button here
            would sit under Popover.Trigger asChild and swallow the
            trigger props (Tip doesn't forward unknown props). */}
        <TexturePalettePopover
          bridge={bridge}
          slot={slot}
          onApply={commit}
          tip="Frequently-used textures"
        >
          <button
            type="button"
            className="btn-texture-browse"
            aria-label={`Open texture palette for ${label}`}
            data-testid={`texture-palette-trigger-${slot}`}
          >
            <LayoutGrid size={14} aria-hidden="true" />
          </button>
        </TexturePalettePopover>
      </div>
    </div>
  );
}

export function FieldSpinner({
  label,
  value,
  min,
  max,
  step,
  decimals,
  unit,
  disabled,
  displayInvertedPercent,
  displayScale,
  widthBoost,
  testId,
  onCommit,
}: {
  label: string;
  value: number;
  min?: number;
  max?: number;
  step?: number;
  decimals?: number;
  unit?: string;
  disabled?: boolean;
  /** When true, displays `100 - value*100` (rounded to integer) and
   *  commits `(100 - displayed) / 100`. Forces min=0, max=100. Used for
   *  `randomLifetimePerc` and `randomScalePerc` per legacy IDC_SPINNER13/14
   *  inverted convention (see Emitter.cpp:487, 492). */
  displayInvertedPercent?: boolean;
  /** When set, displays `value * displayScale` and commits `typed /
   *  displayScale`. The engine stores these as a normalised ratio; the
   *  legacy panel applied this scale purely as a display transform. Pass
   *  `min`/`max`/`step`/`decimals` in DISPLAY space. Used for
   *  `randomRotationAverage` (×360, -180..180°) and `randomRotationVariance`
   *  (×100, 0..100) per legacy IDC_SPINNER16/17 (see Emitter.cpp:498-499,
   *  828-829). Mutually exclusive with `displayInvertedPercent`. */
  displayScale?: number;
  /** Optional input-column boost for spinners whose values exceed the
   *  default 58 px width (e.g. "Tail length:" running up to 4-digit
   *  multipliers). "mid" = +25 % (~73 px), "wide" = +50 % (~87 px),
   *  "x2" = doubled (~116 px). */
  widthBoost?: "mid" | "wide" | "x2";
  /** Optional data-testid for a11y surface drivers. Applied to the
   *  outermost .form-row div (targets the spinner row as a unit) AND
   *  forwarded to the inner Spinner, which stamps `${testId}-inc` /
   *  `${testId}-dec` on the step buttons so a --record cursor can drive
   *  the value. Use sparingly — only at callsites that need UIA capture
   *  or record-cursor anchoring. */
  testId?: string;
  onCommit: (value: number) => void;
}) {
  const displayValue = displayInvertedPercent
    ? Math.round(100 - value * 100)
    : displayScale != null
      ? value * displayScale
      : value;
  const handleCommit = (next: number) => {
    if (displayInvertedPercent) {
      onCommit((100 - next) / 100);
    } else if (displayScale != null) {
      onCommit(next / displayScale);
    } else {
      onCommit(next);
    }
  };
  const effectiveMin = displayInvertedPercent ? 0 : min;
  const effectiveMax = displayInvertedPercent ? 100 : max;
  const effectiveStep = displayInvertedPercent ? 1 : step;
  const effectiveDecimals = displayInvertedPercent ? 0 : decimals;
  const rowClass =
    widthBoost === "x2"
      ? "form-row form-row-x2-input"
      : widthBoost === "wide"
        ? "form-row form-row-wide-input"
        : widthBoost === "mid"
          ? "form-row form-row-mid-input"
          : "form-row";
  return (
    <div className={rowClass} data-testid={testId}>
      <span className="lbl">{label}</span>
      {/* The design's .form-row 3rd column carries the unit
          hint, so we suppress the Spinner's inline trailing-unit overlay
          here. Outside .form-row callers still get the inline unit. */}
      <Spinner
        value={displayValue}
        onChange={handleCommit}
        min={effectiveMin}
        max={effectiveMax}
        step={effectiveStep}
        decimals={effectiveDecimals}
        disabled={disabled}
        aria-label={label}
        testId={testId}
      />
      <span className="unit">{unit ?? ""}</span>
    </div>
  );
}

function FieldCheckbox({
  label,
  checked,
  disabled,
  onCheckedChange,
  inlineLabel,
  testId,
}: {
  label: string;
  checked: boolean;
  disabled?: boolean;
  onCheckedChange: (checked: boolean) => void;
  /** Optional data-testid on the checkbox root (record-cursor / a11y drivers). */
  testId?: string;
  /** When true, allow the label to wrap onto multiple lines instead of
   *  truncating with ellipsis — a fallback for an extremely narrow
   *  inspector. With the `.form-row-check` grid (below) the label gets
   *  the full row width minus the checkbox, so truncation is rare; this
   *  stays as a belt-and-suspenders for labels like "Link particles to
   *  instance" at the minimum pane width. */
  inlineLabel?: boolean;
}) {
  // Checkbox rows use the `.form-row-check` grid (`1fr auto`): the label
  // fills the row and the 18px checkbox hugs the right edge. Unlike the
  // spinner `.form-row` (`1fr 58px 40px`), it doesn't reserve the
  // input + unit columns — which previously left ~80px of empty space
  // beside the checkbox and squeezed long labels into a too-narrow col 1.
  // `justify-self-end` keeps the checkbox flush right within its column.
  return (
    <div className={`form-row form-row-check${inlineLabel ? " form-row-check-inline" : ""}`}>
      <span className="lbl">{label}</span>
      <Checkbox.Root
        checked={checked}
        disabled={disabled}
        data-testid={testId}
        onCheckedChange={(v) => onCheckedChange(v === true)}
        className={`flex h-[18px] w-[18px] items-center justify-center rounded border border-border-2 bg-bg-2 transition focus-ring col-2 justify-self-end ${
          disabled ? "cursor-not-allowed opacity-40" : "cursor-pointer hover:border-border-2"
        } data-[state=checked]:border-accent-strong data-[state=checked]:bg-accent-strong`}
        aria-label={label}
      >
        <Checkbox.Indicator>
          {/* White (not text-text) so the checkmark clears WCAG on the
              --accent-strong fill in BOTH themes (text-text is #1f1f1f in
              light → only 2.7:1 on the fill). */}
          <Check size={12} className="text-white" />
        </Checkbox.Indicator>
      </Checkbox.Root>
    </div>
  );
}

function FieldSelect({
  label,
  value,
  options,
  disabled,
  onCommit,
  testId,
  widthBoost,
}: {
  label: string;
  value: number;
  options: { value: number; label: string }[];
  disabled?: boolean;
  onCommit: (value: number) => void;
  testId?: string;
  /** Optional input-column boost so dropdown triggers with long option
   *  labels render without truncation. "mid" = +25 % (~73 px),
   *  "wide" = +50 % (~87 px), "x2" = doubled (~116 px). Maps to
   *  .form-row-mid-input / .form-row-wide-input / .form-row-x2-input
   *  CSS modifiers. */
  widthBoost?: "mid" | "wide" | "x2";
}) {
  const selected = options.find((o) => o.value === value);
  const rowClass =
    widthBoost === "x2"
      ? "form-row form-row-x2-input"
      : widthBoost === "wide"
        ? "form-row form-row-wide-input"
        : widthBoost === "mid"
          ? "form-row form-row-mid-input"
          : "form-row";
  return (
    <div className={rowClass}>
      <span className="lbl">{label}</span>
      <Select.Root
        value={String(value)}
        onValueChange={(v) => onCommit(Number(v))}
        disabled={disabled}
      >
        <Select.Trigger
          data-testid={testId}
          aria-label={label}
          className="flex h-[var(--row-h)] w-full items-center justify-between gap-1 rounded border border-border-2 bg-bg-2 px-2 text-xs text-text transition motion-reduce:transition-none hover:border-border-2 focus-ring disabled:cursor-not-allowed disabled:opacity-40"
        >
          {/* Radix Select.Value strips className, so wrap it in a truncating
              span — white-space:nowrap + overflow-hidden + ellipsis apply to
              the selected-label text through the wrapper. min-w-0 lets this
              flex child shrink so long labels (e.g. "Diffuse transparent")
              ellipsize instead of wrapping onto a second line (#573). */}
          <span className="min-w-0 truncate">
            <Select.Value>{selected?.label ?? ""}</Select.Value>
          </span>
          <Select.Icon className="shrink-0">
            <ChevronDown className="size-3 text-text-3" />
          </Select.Icon>
        </Select.Trigger>
        <Select.Portal>
          <Select.Content
            position="popper"
            sideOffset={4}
            className="z-50 min-w-[160px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-[var(--shadow-soft)] popover-animate-in"
          >
            <Select.Viewport>
              {options.map((opt) => (
                <Select.Item
                  key={opt.value}
                  value={String(opt.value)}
                  data-testid={
                    testId ? `${testId}-option-${opt.value}` : undefined
                  }
                  className="cursor-pointer rounded px-2 py-0.5 text-xs text-text outline-none data-[highlighted]:bg-accent-soft data-[highlighted]:text-accent"
                >
                  <Select.ItemText>{opt.label}</Select.ItemText>
                </Select.Item>
              ))}
            </Select.Viewport>
          </Select.Content>
        </Select.Portal>
      </Select.Root>
      <span className="unit" />
    </div>
  );
}

// ─── Appearance tab ─────────────────────────────────────────────────

// Exported for direct testing — Radix Tabs in jsdom doesn't reliably
// switch via fireEvent (the known pointer-event flake noted in the
// tests), so vitest mounts AppearanceTab directly.
//
// Restructure — five sections matching legacy
// IDD_EMITTER_PROPS2 (`src/UI/EmitterEditor.rc:381-385`):
//   Textures / Random color addition / Tail / Rotation / Rendering.
//
// Field moves vs the prior layout:
//   - Rotation block (random rotation direction, fixed rotation,
//     average, variance) moved IN from the Basic tab.
//   - `affectedByWind` moved OUT to Physics > Initial speed.
//   - `nTriangles` dropped from the inspector entirely;
//     the schema field is retained on the wire.
//
// Semantic flip on "Always face camera" (legacy IDC_CHECK16,
// `.rc:404`): the checkbox label and meaning are inverted from
// `isWorldOriented`. Checkbox checked = "always face camera = yes" =
// `isWorldOriented = false`. When `blendMode === BLEND_BUMP` the
// cascade forces the camera-facing orientation, so the checkbox
// displays as checked + disabled (mirrors the legacy WM_COMMAND
// handler at [src/UI/Emitter.cpp:522-525] which flips
// `isWorldOriented = false` the moment the user picks bump-map; we
// keep the property untouched here so toggling back restores the
// user's prior choice, but the UI reflects the forced state).
// No-op bridge so AppearanceTab renders in isolation (existing field-label
// / spinner tests) without wiring a real bridge. The palette popover is
// closed at mount, so list/occlusion requests never fire; only a texture
// commit would hit `request`, which harmlessly resolves empty.
const NOOP_BRIDGE = {
  request: async () => ({}),
  on: () => () => {},
} as unknown as Bridge;

export function AppearanceTab({
  properties,
  onCommit,
  onBrowseTexture = async () => "",
  bridge = NOOP_BRIDGE,
}: {
  properties: EmitterPropertiesDto;
  onCommit: (patch: Partial<EmitterPropertiesDto>) => void;
  /** Opens the host-side texture dialog; resolves to the picked
   *  basename ("" if cancelled). Defaults to a no-op so existing tests
   *  and any caller that doesn't wire Browse still render cleanly. */
  onBrowseTexture?: (slot: "color" | "bump") => Promise<string>;
  /** Live bridge for the texture palette popover + usage tracking.
   *  Defaults to a no-op so isolated AppearanceTab tests render cleanly. */
  bridge?: Bridge;
}) {
  const forceFace = properties.blendMode === BLEND_BUMP;
  const tailEnabled = properties.hasTail;
  const rotationEnabled = properties.randomRotation;

  // Display 0..1 random-colour values as 0..100% in the spinners
  // (matches the legacy IDC_SPINNER19-26 percentage spinners at
  // [src/UI/Emitter.cpp:243-246]).
  const updateRandomColors = (idx: 0 | 1 | 2 | 3, displayed: number) => {
    const next: [number, number, number, number] = [
      properties.randomColors[0],
      properties.randomColors[1],
      properties.randomColors[2],
      properties.randomColors[3],
    ];
    next[idx] = displayed / 100;
    onCommit({ randomColors: next as unknown as Vec4 });
  };

  return (
    <div className="inspector">
      <Section title="Textures">
        {/* Color/bump texture fields: Browse
            button (host native dialog via textures/browse) + palette
            button (frequently-used per-mod pinned/recent popup). */}
        <TexturePickerField
          label="Color:"
          slot="color"
          value={properties.colorTexture}
          onCommit={(v) => onCommit({ colorTexture: v })}
          onBrowse={onBrowseTexture}
          bridge={bridge}
        />
        <TexturePickerField
          label="Bump:"
          slot="bump"
          value={properties.normalTexture}
          onCommit={(v) => onCommit({ normalTexture: v })}
          onBrowse={onBrowseTexture}
          bridge={bridge}
        />
        <FieldSpinner
          label="Texture elements:"
          value={properties.textureSize}
          min={1}
          step={1}
          decimals={0}
          testId="spinner-texture-elements"
          onCommit={(v) => onCommit({ textureSize: Math.max(1, Math.round(v)) })}
        />
        {/* Minimum scale: adopts displayInvertedPercent —
            matches legacy IDC_SPINNER13 inversion at
            [src/UI/Emitter.cpp:492]. The stored ratio (0..1) displays
            as `100 - val*100` and commits `(100 - displayed)/100`. */}
        <FieldSpinner
          label="Minimum scale:"
          value={properties.randomScalePerc}
          displayInvertedPercent
          unit="%"
          testId="spinner-min-scale"
          onCommit={(v) => onCommit({ randomScalePerc: v })}
        />
      </Section>

      <Section title="Random color addition">
        {/* RGBA — 4-spinner cluster (R/G/B/A as 0..100%). Per-channel
            R / G / B / A micro-labels above each spinner mirror the
            X/Y/Z pattern Vec3 rows use elsewhere. Laid out as
            2 columns × 2 rows (R/G top, B/A bottom) so each spinner
            cell is twice as wide as a 4-up layout — easier to read at
            the inspector's typical column width. */}
        <div className="form-row form-row-cluster items-start">
          <span className="lbl pt-1">RGBA:</span>
          <div className="grid grid-cols-2 gap-1">
            <div className="axis-cell">
              <span className="axis-lbl">R</span>
              <Spinner
                value={properties.randomColors[0] * 100}
                min={0}
                max={100}
                step={1}
                decimals={0}
                unit="%"
                onChange={(v) => updateRandomColors(0, v)}
                aria-label="Red"
              />
            </div>
            <div className="axis-cell">
              <span className="axis-lbl">G</span>
              <Spinner
                value={properties.randomColors[1] * 100}
                min={0}
                max={100}
                step={1}
                decimals={0}
                unit="%"
                onChange={(v) => updateRandomColors(1, v)}
                aria-label="Green"
              />
            </div>
            <div className="axis-cell">
              <span className="axis-lbl">B</span>
              <Spinner
                value={properties.randomColors[2] * 100}
                min={0}
                max={100}
                step={1}
                decimals={0}
                unit="%"
                onChange={(v) => updateRandomColors(2, v)}
                aria-label="Blue"
              />
            </div>
            <div className="axis-cell">
              <span className="axis-lbl">A</span>
              <Spinner
                value={properties.randomColors[3] * 100}
                min={0}
                max={100}
                step={1}
                decimals={0}
                unit="%"
                onChange={(v) => updateRandomColors(3, v)}
                aria-label="Alpha"
              />
            </div>
          </div>
        </div>
        <FieldCheckbox
          label="Grayscale"
          checked={properties.doColorAddGrayscale}
          onCheckedChange={(v) => onCommit({ doColorAddGrayscale: v })}
        />
      </Section>

      <Section title="Tail">
        <FieldCheckbox
          label="Has tail"
          checked={properties.hasTail}
          onCheckedChange={(v) => onCommit({ hasTail: v })}
          testId="appearance-has-tail"
        />
        {/* Tail length uses unit="x" per legacy .rc:421. */}
        <FieldSpinner
          label="Tail length:"
          value={properties.tailSize}
          min={0}
          step={0.1}
          unit="x"
          disabled={!tailEnabled}
          onCommit={(v) => onCommit({ tailSize: v })}
          widthBoost="mid"
          testId="spinner-tail-length"
        />
      </Section>

      <Section title="Rotation">
        {/* Rotation block moved in from the Basic tab.
            The Average/Variance fields are disabled when
            `randomRotation === false` — mirrors legacy
            [src/UI/Emitter.cpp:201-206]. Variance carries a `± °`
            unit prefix per legacy .rc:423. */}
        <FieldCheckbox
          label="Random rotation direction"
          checked={properties.randomRotationDirection}
          onCheckedChange={(v) => onCommit({ randomRotationDirection: v })}
          testId="appearance-random-rotation-dir"
        />
        <FieldCheckbox
          label="Fixed random rotation:"
          checked={properties.randomRotation}
          onCheckedChange={(v) => onCommit({ randomRotation: v })}
          testId="appearance-random-rotation"
        />
        {/* The engine stores these as a normalised ratio; the
            legacy panel displayed average as ×360 (integer −180..180°) and
            variance as ×100 (integer 0..100), committing typed/360 and
            typed/100 (Emitter.cpp:498-499, 828-829). The host serialises the
            raw ratio, so the scale transform lives here. */}
        <FieldSpinner
          label="Rotation average:"
          value={properties.randomRotationAverage}
          displayScale={360}
          min={-180}
          max={180}
          step={1}
          decimals={0}
          unit="°"
          disabled={!rotationEnabled}
          onCommit={(v) => onCommit({ randomRotationAverage: v })}
        />
        <FieldSpinner
          label="Rotation variance:"
          value={properties.randomRotationVariance}
          displayScale={100}
          min={0}
          max={100}
          step={1}
          decimals={0}
          unit="± °"
          disabled={!rotationEnabled}
          testId="spinner-rotation-variance"
          onCommit={(v) => onCommit({ randomRotationVariance: v })}
        />
      </Section>

      <Section title="Rendering">
        {/* "Always face camera" — semantic flip from the legacy "World
            Oriented" checkbox. Checked = "yes, always face camera" =
            `isWorldOriented === false`. BLEND_BUMP cascade forces the
            checkbox checked + disabled. */}
        <FieldCheckbox
          label="Always face camera"
          checked={forceFace ? true : !properties.isWorldOriented}
          disabled={forceFace}
          onCheckedChange={(v) => onCommit({ isWorldOriented: !v })}
        />
        <FieldCheckbox
          label="Heat particle"
          checked={properties.isHeatParticle}
          onCheckedChange={(v) => onCommit({ isHeatParticle: v })}
        />
        <FieldCheckbox
          label="No depth test"
          checked={properties.noDepthTest}
          onCheckedChange={(v) => onCommit({ noDepthTest: v })}
        />
        <FieldSelect
          label="Blend mode:"
          value={properties.blendMode}
          options={BLEND_MODE_OPTIONS}
          onCommit={(v) => onCommit({ blendMode: v })}
          testId="appearance-blend-mode-trigger"
          widthBoost="x2"
        />
      </Section>
    </div>
  );
}

// ─── Physics tab ────────────────────────────────────────────────────

// Exported for direct testing — matches the AppearanceTab pattern.
// Radix Tabs in jsdom doesn't reliably switch via fireEvent (known
// pointer-event flake), so vitest mounts
// PhysicsTab directly.
//
// Restructure — four sections matching legacy
// IDD_EMITTER_PROPS3 (`src/UI/EmitterEditor.rc:347-417`):
//   Initial position / Initial speed / Acceleration / Ground
//   interaction.
//
// Field moves vs the prior layout:
//   - `parentLinkStrength` ("Parent speed inherit:") moved IN from
//     Basic, now under Initial speed. Inline `* 100` / `/ 100` math
//     since this is the only non-inverted display-percent consumer.
//   - `affectedByWind` moved IN from Appearance, now under Initial
//     speed (matches legacy IDD_EMITTER_PROPS3, .rc:350).
//   - `emitFromMesh` + `emitFromMeshOffset` moved OUT to Basic >
//     Connection.
//   - `isWeatherParticle` + `weatherCubeSize` + `weatherCubeDistance`
//     moved OUT to Basic > Generation Weather radio.
//   - `weatherFadeoutDistance` dropped (schema retained).
//   - `groups[1]` (Lifetime random-param) dropped from the render
//     tree; schema array still carries 3 entries, we just don't
//     render index 1.
//
// Weather-mode disable cascade (matches legacy
// [src/UI/Emitter.cpp:175-190]): when `isWeatherParticle === true`,
// the following controls disable — `groups[2]` (Initial position),
// `parentLinkStrength` (Parent speed inherit), `acceleration[0..2]`
// (X/Y/Z), `gravity`, `inwardAcceleration`,
// `objectSpaceAcceleration`, `groundBehavior`, and `bounciness`. The
// following STAY ENABLED under weather: `inwardSpeed`, `groups[0]`
// (Initial speed), `affectedByWind`. Bounciness has an additional
// gate: only enabled when `groundBehavior === GROUND_BEHAVIOR_BOUNCE`
// ([src/UI/Emitter.cpp:190]).
export function PhysicsTab({
  properties,
  onCommit,
}: {
  properties: EmitterPropertiesDto;
  onCommit: (patch: Partial<EmitterPropertiesDto>) => void;
}) {
  const nonWeather = !properties.isWeatherParticle;
  const bouncinessEnabled = nonWeather && properties.groundBehavior === GROUND_BEHAVIOR_BOUNCE;

  const updateAcceleration = (idx: 0 | 1 | 2, v: number) => {
    const next: [number, number, number] = [
      properties.acceleration[0],
      properties.acceleration[1],
      properties.acceleration[2],
    ];
    next[idx] = v;
    onCommit({ acceleration: next as unknown as Vec3 });
  };

  const updateGroup = (idx: number, patch: Partial<GroupDto>) => {
    const next = properties.groups.map((g, i) => (i === idx ? { ...g, ...patch } : g));
    onCommit({ groups: next });
  };

  return (
    <div className="inspector">
      <Section title="Initial position" unit="units">
        <GroupBody index={2} group={properties.groups[2]} onChange={(p) => updateGroup(2, p)} />
      </Section>

      <Section title="Initial speed" unit="units/s">
        <GroupBody index={0} group={properties.groups[0]} onChange={(p) => updateGroup(0, p)} />
        {/* No per-field unit — the "Initial speed" section header carries
            "units/s" for every speed field in the section. */}
        <FieldSpinner
          label="Inward speed:"
          value={properties.inwardSpeed}
          step={0.1}
          onCommit={(v) => onCommit({ inwardSpeed: v })}
        />
        {/* Parent speed inherit — schema field is float in [0,1]; legacy
            displays as integer percent (Emitter.cpp:488 commits
            `GetUIInteger(...) / 100.0f`). Inline `* 100` / `/ 100` math
            here so we don't grow a new FieldSpinner prop for a single
            consumer. If a third consumer emerges, hoist a
            `displayPercentScale` primitive. */}
        <FieldSpinner
          label="Parent speed inherit:"
          value={Math.round(properties.parentLinkStrength * 100)}
          min={0}
          max={100}
          step={1}
          decimals={0}
          unit="%"
          disabled={!nonWeather}
          onCommit={(v) => onCommit({ parentLinkStrength: v / 100 })}
          testId="spinner-parent-link"
        />
        <FieldCheckbox
          label="Affected by wind"
          checked={properties.affectedByWind}
          onCheckedChange={(v) => onCommit({ affectedByWind: v })}
        />
      </Section>

      <Section title="Acceleration" unit="units/s²">
        {/* Acceleration X/Y/Z — 3-spinner cluster. Spans the .form-row
            input + unit columns since 3 spinners don't fit in 92px.
            Combined "X / Y / Z:" label per legacy IDD_EMITTER_PROPS3
            (.rc:350). */}
        <div className="form-row form-row-cluster items-start">
          {/* No unit here — the "Acceleration" section header carries
              "units/s²" for every field in the section (keeping this label short
              so the fixed-width label column doesn't truncate it). */}
          <span className="lbl pt-1">X / Y / Z:</span>
          <div className="grid grid-cols-3 gap-1">
            <div className="axis-cell">
              <span className="axis-lbl">X</span>
              <Spinner
                value={properties.acceleration[0]}
                step={0.1}
                disabled={!nonWeather}
                onChange={(v) => updateAcceleration(0, v)}
                aria-label="Acceleration X"
              />
            </div>
            <div className="axis-cell">
              <span className="axis-lbl">Y</span>
              <Spinner
                value={properties.acceleration[1]}
                step={0.1}
                disabled={!nonWeather}
                onChange={(v) => updateAcceleration(1, v)}
                aria-label="Acceleration Y"
              />
            </div>
            <div className="axis-cell">
              <span className="axis-lbl">Z</span>
              <Spinner
                value={properties.acceleration[2]}
                step={0.1}
                disabled={!nonWeather}
                onChange={(v) => updateAcceleration(2, v)}
                aria-label="Acceleration Z"
              />
            </div>
          </div>
        </div>
        {/* No per-field unit — the "Acceleration" section header carries
            "units/s²" for every acceleration field in the section. */}
        <FieldSpinner
          label="Gravity acceleration:"
          value={properties.gravity}
          step={0.1}
          disabled={!nonWeather}
          onCommit={(v) => onCommit({ gravity: v })}
        />
        <FieldSpinner
          label="Inward acceleration:"
          value={properties.inwardAcceleration}
          step={0.1}
          disabled={!nonWeather}
          onCommit={(v) => onCommit({ inwardAcceleration: v })}
        />
        <FieldCheckbox
          label="Object space acceleration"
          checked={properties.objectSpaceAcceleration}
          disabled={!nonWeather}
          onCheckedChange={(v) => onCommit({ objectSpaceAcceleration: v })}
          inlineLabel
        />
      </Section>

      <Section title="Ground interaction">
        <FieldSelect
          label="Behavior:"
          value={properties.groundBehavior}
          options={GROUND_BEHAVIOR_OPTIONS}
          disabled={!nonWeather}
          onCommit={(v) => onCommit({ groundBehavior: v })}
          testId="physics-ground-behavior-trigger"
          widthBoost="mid"
        />
        {/* Legacy bounciness was an unbounded float (Emitter.cpp:259-266,
            506). Don't clamp to [0,1] — a modder can use >1 (super-elastic) and
            existing files outside [0,1] must round-trip on edit. */}
        <FieldSpinner
          label="Bounciness:"
          value={properties.bounciness}
          step={0.05}
          disabled={!bouncinessEnabled}
          onCommit={(v) => onCommit({ bounciness: v })}
        />
      </Section>
    </div>
  );
}

// GroupBody — renders a single random-param group's fields (Type
// selector + type-conditional fields). The parent <Section> carries
// the title; no fieldset/legend chrome here.
//
// `data-testid={`physics-group-${index}`}` is preserved for specs
// that match on the group container.
function GroupBody({
  index,
  group,
  onChange,
}: {
  index: number;
  group: GroupDto;
  onChange: (patch: Partial<GroupDto>) => void;
}) {
  const updateVec3 = (
    key: "min" | "max" | "val",
    axis: 0 | 1 | 2,
    v: number,
  ) => {
    const cur = group[key];
    const next: [number, number, number] = [cur[0], cur[1], cur[2]];
    next[axis] = v;
    onChange({ [key]: next as unknown as Vec3 } as Partial<GroupDto>);
  };

  return (
    <div data-testid={`physics-group-${index}`} className="space-y-2">
      <FieldSelect
        label="Type:"
        value={group.type}
        options={GROUP_TYPE_OPTIONS}
        onCommit={(v) => onChange({ type: v })}
        testId={`physics-group-${index}-type-trigger`}
        widthBoost="wide"
      />
      {group.type === GT_EXACT && (
        <Vec3Row
          label="Value:"
          value={group.val}
          step={0.1}
          ariaPrefix={`Group ${index + 1} Value`}
          onChange={(axis, v) => updateVec3("val", axis, v)}
        />
      )}
      {group.type === GT_BOX && (
        <>
          <Vec3Row
            label="Min:"
            value={group.min}
            step={0.1}
            ariaPrefix={`Group ${index + 1} Min`}
            onChange={(axis, v) => updateVec3("min", axis, v)}
          />
          <Vec3Row
            label="Max:"
            value={group.max}
            step={0.1}
            ariaPrefix={`Group ${index + 1} Max`}
            onChange={(axis, v) => updateVec3("max", axis, v)}
          />
        </>
      )}
      {group.type === GT_CUBE && (
        <FieldSpinner
          label="Side length:"
          value={group.sideLength}
          min={0}
          step={0.1}
          onCommit={(v) => onChange({ sideLength: v })}
        />
      )}
      {group.type === GT_SPHERE && (
        <>
          <FieldSpinner
            label="Radius:"
            value={group.sphereRadius}
            min={0}
            step={0.1}
            onCommit={(v) => onChange({ sphereRadius: v })}
          />
          {/* `sphereEdge` is an engine boolean (EmitterInstance.cpp:205):
              nonzero → spawn at the full radius (on the surface), zero →
              random radius (throughout the volume). Legacy surfaces it as
              a "Constrain to surface" checkbox; mirror that here. */}
          <FieldCheckbox
            label="Constrain to surface"
            checked={group.sphereEdge !== 0}
            onCheckedChange={(c) => onChange({ sphereEdge: c ? 1 : 0 })}
          />
        </>
      )}
      {group.type === GT_CYLINDER && (
        <>
          {/* Radius + Height on one row for density / legacy parity, using
              the same umbrella-label + axis-cell cluster idiom as Vec3Row
              (the empty umbrella keeps the spinners aligned with the other
              field rows). */}
          <div className="form-row form-row-cluster items-start">
            <span className="lbl pt-1" />
            <div className="grid grid-cols-2 gap-1">
              <div className="axis-cell">
                <span className="axis-lbl">Radius</span>
                <Spinner
                  value={group.cylinderRadius}
                  min={0}
                  step={0.1}
                  onChange={(v) => onChange({ cylinderRadius: v })}
                  aria-label="Cylinder radius"
                />
              </div>
              <div className="axis-cell">
                <span className="axis-lbl">Height</span>
                <Spinner
                  value={group.cylinderHeight}
                  min={0}
                  step={0.1}
                  onChange={(v) => onChange({ cylinderHeight: v })}
                  aria-label="Cylinder height"
                />
              </div>
            </div>
          </div>
          {/* `cylinderEdge` is an engine boolean (EmitterInstance.cpp:215),
              same surface-constraint semantics as `sphereEdge` — surface it
              as legacy's "Constrain to surface" checkbox. */}
          <FieldCheckbox
            label="Constrain to surface"
            checked={group.cylinderEdge !== 0}
            onCheckedChange={(c) => onChange({ cylinderEdge: c ? 1 : 0 })}
          />
        </>
      )}
    </div>
  );
}

function Vec3Row({
  label,
  value,
  step,
  ariaPrefix,
  onChange,
}: {
  label: string;
  value: Vec3;
  step: number;
  ariaPrefix: string;
  onChange: (axis: 0 | 1 | 2, v: number) => void;
}) {
  return (
    <div className="form-row form-row-cluster items-start">
      <span className="lbl pt-1">{label}</span>
      <div className="grid grid-cols-3 gap-1">
        <div className="axis-cell">
          <span className="axis-lbl">X</span>
          <Spinner
            value={value[0]}
            step={step}
            onChange={(v) => onChange(0, v)}
            aria-label={`${ariaPrefix} X`}
          />
        </div>
        <div className="axis-cell">
          <span className="axis-lbl">Y</span>
          <Spinner
            value={value[1]}
            step={step}
            onChange={(v) => onChange(1, v)}
            aria-label={`${ariaPrefix} Y`}
          />
        </div>
        <div className="axis-cell">
          <span className="axis-lbl">Z</span>
          <Spinner
            value={value[2]}
            step={step}
            onChange={(v) => onChange(2, v)}
            aria-label={`${ariaPrefix} Z`}
          />
        </div>
      </div>
    </div>
  );
}
