import type { EmitterPropertiesDto, GroupDto, Vec3 } from "@particle-editor/bridge-schema";
import { Section } from "@/components/Section";
import { Spinner } from "@/primitives/Spinner";
import {
  FieldCheckbox,
  FieldSelect,
  FieldSpinner,
  GROUND_BEHAVIOR_BOUNCE,
  GROUND_BEHAVIOR_OPTIONS,
  GroupBody,
} from "./fields";

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
//
// Exported for direct testing — matches the AppearanceTab pattern.
// Radix Tabs in jsdom doesn't reliably switch via fireEvent (known
// pointer-event flake), so vitest mounts PhysicsTab directly.
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
