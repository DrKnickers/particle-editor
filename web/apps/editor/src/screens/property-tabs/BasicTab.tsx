import type { EmitterPropertiesDto } from "@particle-editor/bridge-schema";
import { Section } from "@/components/Section";
import {
  EMIT_FROM_MESH_DISABLE,
  EMIT_FROM_MESH_OPTIONS,
  FieldCheckbox,
  FieldSelect,
  FieldSpinner,
  FieldText,
  type GenerationMode,
  navigate,
  RadioRow,
} from "./fields";

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
