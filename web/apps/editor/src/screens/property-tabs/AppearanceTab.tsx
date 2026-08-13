import type { Bridge, EmitterPropertiesDto, Vec4 } from "@particle-editor/bridge-schema";
import { Section } from "@/components/Section";
import { Spinner } from "@/primitives/Spinner";
import {
  BLEND_BUMP,
  BLEND_MODE_OPTIONS,
  FieldCheckbox,
  FieldSelect,
  FieldSpinner,
  NOOP_BRIDGE,
  TexturePickerField,
} from "./fields";

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
