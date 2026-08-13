import { useCallback, useRef, useState } from "react";
import * as Tabs from "@radix-ui/react-tabs";
import * as Checkbox from "@radix-ui/react-checkbox";
import * as Select from "@radix-ui/react-select";
import { Check, ChevronDown, FolderOpen, LayoutGrid } from "lucide-react";
import { TexturePalettePopover } from "@/screens/TexturePalettePopover";
import type { Bridge, GroupDto, Vec3 } from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { Tip } from "@/primitives/Tip";

// Blend mode dropdown options — mirrors the legacy `BlendModes[]` table
// at [src/UI/Emitter.cpp:20-31]. The engine has additional blend mode
// values (8, 9, 10, 13) but the legacy UI doesn't expose them via the
// dropdown — keep parity here.
export const BLEND_MODE_OPTIONS: { value: number; label: string }[] = [
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
export const BLEND_BUMP = 11;

// Ground-interaction dropdown options — mirrors the legacy
// `GroundBehaviors[]` table at [src/UI/Emitter.cpp:35-40]. Values are
// the engine enum index (0..3); the `IDS_GROUND_BEHAVIOR_BOUNCE`
// string-id is the 3rd entry (value 2), and legacy cascades enable
// `bounciness` only when this value is picked
// (see [src/UI/Emitter.cpp:190]).
export const GROUND_BEHAVIOR_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "None" },
  { value: 1, label: "Disappear" },
  { value: 2, label: "Bounce" },
  { value: 3, label: "Stick" },
];
export const GROUND_BEHAVIOR_BOUNCE = 2;

// Emit-from-mesh dropdown options — mirrors the legacy
// `EmitModes[]` table at [src/UI/Emitter.cpp:44-49]. Values match
// `ParticleSystem::EMIT_*` constants at [src/ParticleSystem.h:66-69]:
// EMIT_DISABLE=0, EMIT_RANDOM_VERTEX=1, EMIT_RANDOM_MESH=2,
// EMIT_EVERY_VERTEX=3.
export const EMIT_FROM_MESH_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Disable" },
  { value: 1, label: "Random Vertex" },
  { value: 2, label: "Random Mesh" },
  { value: 3, label: "Every Vertex" },
];
export const EMIT_FROM_MESH_DISABLE = 0;

// Random-Param group type dropdown options — mirrors the engine
// `GT_*` constants at [src/ParticleSystem.h:20-24]: GT_EXACT=0,
// GT_BOX=1, GT_CUBE=2, GT_SPHERE=3, GT_CYLINDER=4.
export const GROUP_TYPE_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Exact" },
  { value: 1, label: "Box" },
  { value: 2, label: "Cube" },
  { value: 3, label: "Sphere" },
  { value: 4, label: "Cylinder" },
];
export const GT_EXACT = 0;
export const GT_BOX = 1;
export const GT_CUBE = 2;
export const GT_SPHERE = 3;
export const GT_CYLINDER = 4;

export type GenerationMode = "bursts" | "continuous" | "weather";

// Roving-tabindex navigation order for the Generation radiogroup. The
// arrow handlers cycle through this list (ArrowUp = -1, ArrowDown = +1)
// with modulo wrap so Bursts ↔ Continuous ↔ Weather is a closed loop.
export const MODE_ORDER: GenerationMode[] = ["bursts", "continuous", "weather"];
export const navigate = (from: GenerationMode, dir: -1 | 1): GenerationMode => {
  const idx = MODE_ORDER.indexOf(from);
  const next = (idx + dir + MODE_ORDER.length) % MODE_ORDER.length;
  return MODE_ORDER[next];
};

export function TabTrigger({ value, label }: { value: string; label: string }) {
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

// Local RadioRow helper — captures the per-radio chrome (role,
// aria-checked, roving tabIndex, the dot+label spans, and the
// keyboard handler) so each radio site in BasicTab is a five-line
// usage instead of a 17-line block. ArrowUp/ArrowDown delegate to
// `onArrowNav` (`-1` for previous, `+1` for next); Enter/Space
// preserve the existing selection behaviour.
export function RadioRow({
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

// Form rows use the design's `.form-row` 3-column grid
// (label / input / unit) from components.css. The optional third
// column carries the unit hint (e.g. "s", "%"); empty for fields
// that don't have one.
export function FieldText({
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

export type WidthBoost = "mid" | "wide" | "x2";

export function rowClassFor(widthBoost: WidthBoost | undefined): string {
  return widthBoost === "x2"
    ? "form-row form-row-x2-input"
    : widthBoost === "wide"
      ? "form-row form-row-wide-input"
      : widthBoost === "mid"
        ? "form-row form-row-mid-input"
        : "form-row";
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
  widthBoost?: WidthBoost;
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
  return (
    <div className={rowClassFor(widthBoost)} data-testid={testId}>
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

export function FieldCheckbox({
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

export function FieldSelect({
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
  widthBoost?: WidthBoost;
}) {
  const selected = options.find((o) => o.value === value);
  return (
    <div className={rowClassFor(widthBoost)}>
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

// No-op bridge so AppearanceTab renders in isolation (existing field-label
// / spinner tests) without wiring a real bridge. The palette popover is
// closed at mount, so list/occlusion requests never fire; only a texture
// commit would hit `request`, which harmlessly resolves empty.
export const NOOP_BRIDGE = {
  request: async () => ({}),
  on: () => () => {},
} as unknown as Bridge;

// GroupBody — renders a single random-param group's fields (Type
// selector + type-conditional fields). The parent <Section> carries
// the title; no fieldset/legend chrome here.
//
// `data-testid={`physics-group-${index}`}` is preserved for specs
// that match on the group container.
export function GroupBody({
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

export function Vec3Row({
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
