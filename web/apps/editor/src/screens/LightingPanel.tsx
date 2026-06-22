// LightingPanel — modeless tool window for the three engine lights,
// ambient tint, and shadow tint. Ported from the native `LightingDlgProc`
// Win32 dialog, which (along with the `--legacy-ui` opt-out) was removed in
// — so this React panel is now the sole lighting surface.
//
// Sections (top-to-bottom):
//   1. Sun light (expanded by default, <details> collapsible):
//      intensity, azimuth, altitude, diffuse, specular.
//   2. Fill light 1 (collapsed by default): intensity, azimuth,
//      altitude, diffuse (no specular — matches legacy "fills are
//      diffuse-only").
//   3. Fill light 2 (collapsed by default): same as Fill 1.
//   4. Ambient (always visible): ColorButton.
//   5. Shadow (always visible): ColorButton.
//   6. Footer: Mirror Sun button (copies sun colour to both fills via
//      two `engine/set/light` calls), Reset button (resets all lights
//      to component-baked defaults).
//
// Bridge surface (existing — zero schema additions in this batch):
//   - engine/set/light  { which, ...LightDto }
//   - engine/set/ambient { color: Vec4 }
//   - engine/set/shadow  { color: Vec4 }
//
// Intensity mapping. The legacy panel stores `intensity` separately
// from the diffuse/specular COLORREFs and folds them at push time:
//     L.Diffuse  = (R/255 * intensity, G/255 * intensity, B/255 * intensity, 1)
//     L.Specular = (R/255 * intensity, G/255 * intensity, B/255 * intensity, 1)
// (the same fold the legacy `MakeLight` performed at push time).
// The engine only stores the post-multiplied Vec4. The panel keeps a
// local intensity (defaulted to 1 on first mount per light) plus the
// colour value displayed in the ColorButton; on commit it multiplies
// the displayed RGB by intensity and ships the resulting Vec4. Changing
// intensity re-multiplies the existing colour so the user's chosen hue
// is preserved.
//
// Force Align (Group D): the legacy "Force Align Fill Lights"
// checkbox snaps fill1/fill2 azimuth from the sun's azimuth. The
// constraint is purely UI-side (engine just consumes the final
// engine/set/light dispatches); flag is session-only — legacy
// persists it as a REG_DWORD, that's a follow-up polish. Constants
// come from the legacy Win32 UI: fill1 += 120°, fill2 += 210°,
// both at -10° altitude. When the checkbox is ON, fill az/alt
// spinners are disabled and the sun's az drives fill az.
const FORCE_ALIGN_FILL1_AZ_OFFSET = 120;
const FORCE_ALIGN_FILL2_AZ_OFFSET = 210;
const FORCE_ALIGN_FILL_ALT        = -10;

import { useEffect, useState } from "react";
import { Tip } from "@/primitives/Tip";
import type {
  Bridge,
  Color,
  LightDto,
  LightingSettingsDto,
  LightSettingsDto,
  LightWhich,
  Vec4,
} from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { ColorButton } from "@/primitives/ColorButton";
import { ToolPanel } from "@/components/ToolPanel";
import { BloomSection } from "@/screens/BloomSection";
import type { RgbColor } from "@/primitives/palette-store";

type Props = {
  bridge: Bridge;
  onClose: () => void;
  /** True while the dock is sliding shut (logically closed, still mounted
   *  for the exit animation). Forwarded to ToolPanel so it marks itself
   *  data-state="closing" and stops presenting as an open, closeable
   *  dialog. See PanelLayout's `dockClosing`. */
  closing?: boolean;
};

// (Z angle, tilt) → unit direction vector. Mirrors `DirectionFromZTilt`
// (legacy Win32 UI) — same convention so values entered in this panel
// match the legacy dialog.
function directionFromAzAlt(zDeg: number, tiltDeg: number): Vec4 {
  const z = (zDeg * Math.PI) / 180;
  const t = (tiltDeg * Math.PI) / 180;
  const c = Math.cos(t);
  return [c * Math.cos(z), c * Math.sin(z), Math.sin(t), 0] as const;
}

/** Build a `LightDto` from user-facing inputs, folding `intensity`
 *  into the diffuse/specular Vec4 channels exactly as the legacy
 *  `MakeLight` (native Win32 UI, removed in) did. */
function buildLightDto(
  zDeg: number,
  tiltDeg: number,
  diffuse: RgbColor,
  specular: RgbColor,
  intensity: number,
): LightDto {
  const dir = directionFromAzAlt(zDeg, tiltDeg);
  const scale = (c: number) => (c / 255) * intensity;
  return {
    diffuse: [scale(diffuse.r), scale(diffuse.g), scale(diffuse.b), 1.0],
    specular: [scale(specular.r), scale(specular.g), scale(specular.b), 1.0],
    position: dir,
    // SetLight derives direction internally from position; the wire
    // value is overwritten engine-side.
    direction: [0, 0, 0, 0],
  };
}

/** Each light's editable state. Held in React so intensity can be
 *  edited as a separate axis from the displayed colour. */
type LightFormState = {
  intensity: number;
  az: number;
  alt: number;
  diffuse: RgbColor;
  specular: RgbColor; // unused for fills
};

// Defaults preserved from the legacy Win32 lighting dialog's constants
// (e.g. its sun-intensity default). These drive the Reset button and also
// serve as the seed when the engine snapshot hasn't arrived yet.
const SUN_DEFAULTS: LightFormState = {
  intensity: 0.5,
  az: 0,
  alt: 45,
  diffuse: { r: 180, g: 180, b: 190 },
  specular: { r: 190, g: 190, b: 200 },
};
const FILL1_DEFAULTS: LightFormState = {
  intensity: 0.5,
  az: 120,
  alt: -10,
  diffuse: { r: 60, g: 80, b: 160 },
  specular: { r: 0, g: 0, b: 0 },
};
const FILL2_DEFAULTS: LightFormState = {
  ...FILL1_DEFAULTS,
  az: 210,
};
// Sun ambient/shadow defaults preserved from the legacy Win32 dialog.
const AMBIENT_DEFAULT: RgbColor = { r: 40, g: 40, b: 50 };
const SHADOW_DEFAULT: RgbColor = { r: 100, g: 100, b: 110 };

function rgbToColorButtonValue(rgb: RgbColor): RgbColor {
  return rgb;
}

/** Unpack a packed COLORREF (`Color`, low byte = R) into an RgbColor.
 *  The `settings/lighting` DTO carries colours in this form (the raw
 *  registry value), unlike the engine snapshot's pre-multiplied Vec4. */
function colorrefToRgb(c: Color): RgbColor {
  return {
    r: c & 0xff,
    g: (c >> 8) & 0xff,
    b: (c >> 16) & 0xff,
  };
}

/** Pack an RgbColor into a COLORREF (`Color`, low byte = R) — the inverse
 *  of colorrefToRgb, for the registry write-back. */
function rgbToColorref(rgb: RgbColor): Color {
  return (rgb.r & 0xff) | ((rgb.g & 0xff) << 8) | ((rgb.b & 0xff) << 16);
}

/** LightFormState → the registry's raw-split DTO (intensity/colour kept
 *  separate, angles in degrees, colours as packed COLORREFs). */
function toLightSettings(s: LightFormState): LightSettingsDto {
  return {
    intensity: s.intensity,
    az: s.az,
    alt: s.alt,
    diffuse: rgbToColorref(s.diffuse),
    specular: rgbToColorref(s.specular),
  };
}

export function LightingPanel({ bridge, onClose, closing }: Props) {
  const [sun, setSun] = useState<LightFormState>(SUN_DEFAULTS);
  const [fill1, setFill1] = useState<LightFormState>(FILL1_DEFAULTS);
  const [fill2, setFill2] = useState<LightFormState>(FILL2_DEFAULTS);
  const [ambient, setAmbient] = useState<RgbColor>(AMBIENT_DEFAULT);
  const [shadow, setShadow] = useState<RgbColor>(SHADOW_DEFAULT);
  // Group D: Force Align Fill Lights. Default ON to match the legacy
  // Win32 UI's force-align default (true). Seeded from the
  // registry below (it rides in the `settings/lighting` DTO); the toggle
  // handler writes it back via `settings/lighting-force-align/set`.
  const [forceAlign, setForceAlign] = useState<boolean>(true);

  // Seed the displayed controls from the RAW lighting settings the host
  // reads from the registry (`settings/lighting`) — intensity and colour
  // are the user's true saved split, not the lossy `intensity × colour`
  // Vec4 recovered from the engine snapshot. Mirrors the legacy `LightingDlgProc`,
  // which read the registry on every open. The host
  // already restored the engine from the same registry at startup, so the
  // displayed values and the rendered scene agree.
  //
  // The pane unmounts when toggled closed, so this re-seeds on reopen
  // (first open + no-edit reopen both show the true saved values;
  // persisting in-session edits across reopen is the deferred lighting
  // write-back item). Under `--test-host` the host returns canonical
  // defaults — not the live registry — so the dialog-lighting a11y golden
  // stays deterministic.
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "settings/lighting", params: {} })
      .then((s) => {
        // Guard a stub / degraded bridge that returns `{}` — keep defaults.
        if (cancelled || !s || !s.sun) return;
        const seed = (l: LightSettingsDto): LightFormState => ({
          intensity: l.intensity,
          az: l.az,
          alt: l.alt,
          diffuse: colorrefToRgb(l.diffuse),
          specular: colorrefToRgb(l.specular),
        });
        const nextSun = seed(s.sun);
        let nextFill1 = seed(s.fill1);
        let nextFill2 = seed(s.fill2);
        // With Force Align on, the fill spinners SHOW the computed angles
        // (sun.az + offset @ FORCE_ALIGN_FILL_ALT) — matching the
        // disabled-spinner display in the legacy UI and
        // the angles the host pushed to the engine.
        if (s.forceAlign) {
          nextFill1 = { ...nextFill1, az: nextSun.az + FORCE_ALIGN_FILL1_AZ_OFFSET, alt: FORCE_ALIGN_FILL_ALT };
          nextFill2 = { ...nextFill2, az: nextSun.az + FORCE_ALIGN_FILL2_AZ_OFFSET, alt: FORCE_ALIGN_FILL_ALT };
        }
        setSun(nextSun);
        setFill1(nextFill1);
        setFill2(nextFill2);
        setAmbient(colorrefToRgb(s.ambient));
        setShadow(colorrefToRgb(s.shadow));
        setForceAlign(s.forceAlign);
      })
      .catch((err) => console.warn("[LightingPanel] settings/lighting failed:", err));
    return () => { cancelled = true; };
  }, [bridge]);

  const pushLight = (which: LightWhich, s: LightFormState) => {
    void bridge.request({
      kind: "engine/set/light",
      params: { which, ...buildLightDto(s.az, s.alt, s.diffuse, s.specular, s.intensity) },
    });
  };

  // Persist the FULL raw split to the registry (native host) / in-memory
  // store (mock). The pushLight/engine-set calls above only touch the live
  // engine; without this write-back the panel's edits and Reset vanish on
  // reopen/restart and a value the legacy dialog persisted (e.g. a stale
  // ambient) would keep re-applying. Callers MUST pass freshly-resolved
  // next-state — never read it back from the `sun`/`fill1`/… closures,
  // which are stale until the next render. Fire-and-forget; the host
  // no-ops under --test-host so a11y goldens are untouched.
  const persistLighting = (next: {
    sun: LightFormState;
    fill1: LightFormState;
    fill2: LightFormState;
    ambient: RgbColor;
    shadow: RgbColor;
    forceAlign: boolean;
  }) => {
    const dto: LightingSettingsDto = {
      sun: toLightSettings(next.sun),
      fill1: toLightSettings(next.fill1),
      fill2: toLightSettings(next.fill2),
      ambient: rgbToColorref(next.ambient),
      shadow: rgbToColorref(next.shadow),
      forceAlign: next.forceAlign,
    };
    void bridge.request({ kind: "settings/lighting/set", params: dto });
  };

  // Group D: cascade helper. When forceAlign is ON, fill1/fill2
  // azimuth follow sun.az with the canonical offsets; altitude is
  // pinned to FORCE_ALIGN_FILL_ALT. Called from updateSun (when sun
  // moves) and from handleForceAlignToggle (when the constraint
  // first engages). Returns the next fill1/fill2 form states so the
  // caller can also commit them to local state in one render.
  const computeAlignedFills = (sunAz: number) => {
    const nextFill1: LightFormState = {
      ...fill1,
      az: sunAz + FORCE_ALIGN_FILL1_AZ_OFFSET,
      alt: FORCE_ALIGN_FILL_ALT,
    };
    const nextFill2: LightFormState = {
      ...fill2,
      az: sunAz + FORCE_ALIGN_FILL2_AZ_OFFSET,
      alt: FORCE_ALIGN_FILL_ALT,
    };
    return { nextFill1, nextFill2 };
  };

  const updateSun = (patch: Partial<LightFormState>) => {
    const next = { ...sun, ...patch };
    setSun(next);
    pushLight("sun", next);
    // Cascade az → fills when forceAlign is engaged. Other patch
    // fields (intensity, diffuse, specular) don't drive fills.
    if (forceAlign && patch.az !== undefined) {
      const { nextFill1, nextFill2 } = computeAlignedFills(next.az);
      setFill1(nextFill1);
      setFill2(nextFill2);
      pushLight("fill1", nextFill1);
      pushLight("fill2", nextFill2);
      persistLighting({ sun: next, fill1: nextFill1, fill2: nextFill2, ambient, shadow, forceAlign });
    } else {
      persistLighting({ sun: next, fill1, fill2, ambient, shadow, forceAlign });
    }
  };
  const updateFill1 = (patch: Partial<LightFormState>) => {
    const next = { ...fill1, ...patch };
    setFill1(next);
    pushLight("fill1", next);
    persistLighting({ sun, fill1: next, fill2, ambient, shadow, forceAlign });
  };
  const updateFill2 = (patch: Partial<LightFormState>) => {
    const next = { ...fill2, ...patch };
    setFill2(next);
    pushLight("fill2", next);
    persistLighting({ sun, fill1, fill2: next, ambient, shadow, forceAlign });
  };

  const handleForceAlignToggle = (enabled: boolean) => {
    setForceAlign(enabled);
    // Persist to the registry so the flag syncs with the legacy UI.
    // Fire-and-forget; the in-memory state is the source of truth for
    // the rest of the session. (Host no-ops the write under --test-host.)
    void bridge.request({
      kind: "settings/lighting-force-align/set",
      params: { enabled },
    });
    // Engaging the constraint snaps the fills immediately so the
    // visible state matches the disabled spinners. Disengaging is
    // non-destructive — fills keep whatever values force-align last
    // forced them to (matching legacy: it doesn't restore prior
    // edits, since edits were blocked while the flag was ON).
    if (enabled) {
      const { nextFill1, nextFill2 } = computeAlignedFills(sun.az);
      setFill1(nextFill1);
      setFill2(nextFill2);
      pushLight("fill1", nextFill1);
      pushLight("fill2", nextFill2);
      persistLighting({ sun, fill1: nextFill1, fill2: nextFill2, ambient, shadow, forceAlign: enabled });
    } else {
      // Disengaging keeps the fills where force-align left them; only the
      // flag changes. (The `…/force-align/set` above already wrote the flag,
      // but persist the full snapshot too so a reopen reads a consistent set.)
      persistLighting({ sun, fill1, fill2, ambient, shadow, forceAlign: enabled });
    }
  };

  // Ambient is pushed with alpha w=1 — game-faithful. The engine folds scene
  // ambient into its SPH lighting as `ambient.xyz * ambient.w`
  // (src/SphericalHarmonics.cpp:76), so w gates the per-vertex mesh ambient
  // floor; per Petroglyph's shaders (reference/foc-shaders/AlamoEngine.fxh)
  // production Mesh*/RSkin* light ambient ONLY via that SPH path, so w=1
  // reproduces the game's mesh brightness. The host startup restore
  // (HostWindow.cpp `ambientToVec4`) and this React `ambientToVec4` push the
  // same w=1, so load == Reset (both stay in lockstep — flip them together or
  // Reset diverges from load again).
  const ambientToVec4 = (rgb: RgbColor): Vec4 =>
    [rgb.r / 255, rgb.g / 255, rgb.b / 255, 1.0] as const;
  // Shadow stays w=0: it is render-inert (the engine echoes it back to this
  // panel via the DTO round-trip but never samples it in a shader), so its
  // alpha gates nothing. Light diffuse/specular keep w=1 via buildLightDto.
  const rgbToVec4 = (rgb: RgbColor): Vec4 =>
    [rgb.r / 255, rgb.g / 255, rgb.b / 255, 0.0] as const;

  const updateAmbient = (rgb: RgbColor) => {
    setAmbient(rgb);
    void bridge.request({
      kind: "engine/set/ambient",
      params: { color: ambientToVec4(rgb) },
    });
    persistLighting({ sun, fill1, fill2, ambient: rgb, shadow, forceAlign });
  };
  const updateShadow = (rgb: RgbColor) => {
    setShadow(rgb);
    void bridge.request({
      kind: "engine/set/shadow",
      params: { color: rgbToVec4(rgb) },
    });
    persistLighting({ sun, fill1, fill2, ambient, shadow: rgb, forceAlign });
  };

  const handleMirrorSun = () => {
    // Copy the sun's diffuse + specular to fill1 and fill2. Per locks,
    // this composes via two `engine/set/light` calls — fill1 and fill2
    // both take the sun's diffuse. Fill specular stays at black per
    // legacy (fills are diffuse-only).
    const newFill1: LightFormState = {
      ...fill1,
      diffuse: sun.diffuse,
      intensity: sun.intensity,
    };
    const newFill2: LightFormState = {
      ...fill2,
      diffuse: sun.diffuse,
      intensity: sun.intensity,
    };
    setFill1(newFill1);
    setFill2(newFill2);
    pushLight("fill1", newFill1);
    pushLight("fill2", newFill2);
    persistLighting({ sun, fill1: newFill1, fill2: newFill2, ambient, shadow, forceAlign });
  };

  const handleReset = () => {
    setSun(SUN_DEFAULTS);
    setFill1(FILL1_DEFAULTS);
    setFill2(FILL2_DEFAULTS);
    setAmbient(AMBIENT_DEFAULT);
    setShadow(SHADOW_DEFAULT);
    pushLight("sun", SUN_DEFAULTS);
    pushLight("fill1", FILL1_DEFAULTS);
    pushLight("fill2", FILL2_DEFAULTS);
    // Push ambient/shadow to the live engine directly — NOT via
    // updateAmbient/updateShadow, whose persist would capture the stale
    // (pre-reset) sun/fill closures. Persist the full default snapshot once.
    void bridge.request({ kind: "engine/set/ambient", params: { color: ambientToVec4(AMBIENT_DEFAULT) } });
    void bridge.request({ kind: "engine/set/shadow",  params: { color: rgbToVec4(SHADOW_DEFAULT) } });
    persistLighting({
      sun: SUN_DEFAULTS,
      fill1: FILL1_DEFAULTS,
      fill2: FILL2_DEFAULTS,
      ambient: AMBIENT_DEFAULT,
      shadow: SHADOW_DEFAULT,
      forceAlign,
    });
  };

  return (
    <ToolPanel title="Lighting" onClose={onClose} variant="docked" closing={closing}>
      <ToolPanel.Section title="Sun" defaultOpen>
        <ToolPanel.Row label="Intensity">
          <Spinner
            value={sun.intensity}
            onChange={(v) => updateSun({ intensity: v })}
            min={0}
            max={2}
            step={0.05}
            aria-label="Sun intensity"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Azimuth">
          <Spinner
            value={sun.az}
            onChange={(v) => updateSun({ az: v })}
            min={-180}
            max={180}
            step={1}
            unit="°"
            aria-label="Sun azimuth"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Altitude">
          <Spinner
            value={sun.alt}
            onChange={(v) => updateSun({ alt: v })}
            min={-90}
            max={90}
            step={1}
            unit="°"
            aria-label="Sun altitude"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Diffuse">
          <ColorButton
            value={rgbToColorButtonValue(sun.diffuse)}
            onChange={(rgb) => updateSun({ diffuse: rgb })}
            aria-label="Sun diffuse colour"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Specular">
          <ColorButton
            value={rgbToColorButtonValue(sun.specular)}
            onChange={(rgb) => updateSun({ specular: rgb })}
            aria-label="Sun specular colour"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      <ToolPanel.Section title="Fill 1">
        <ToolPanel.Row label="Intensity">
          <Spinner
            value={fill1.intensity}
            onChange={(v) => updateFill1({ intensity: v })}
            min={0}
            max={2}
            step={0.05}
            aria-label="Fill 1 intensity"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Azimuth">
          <Spinner
            value={fill1.az}
            onChange={(v) => updateFill1({ az: v })}
            min={-180}
            max={180}
            step={1}
            unit="°"
            disabled={forceAlign}
            aria-label="Fill 1 azimuth"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Altitude">
          <Spinner
            value={fill1.alt}
            onChange={(v) => updateFill1({ alt: v })}
            min={-90}
            max={90}
            step={1}
            unit="°"
            disabled={forceAlign}
            aria-label="Fill 1 altitude"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Diffuse">
          <ColorButton
            value={rgbToColorButtonValue(fill1.diffuse)}
            onChange={(rgb) => updateFill1({ diffuse: rgb })}
            aria-label="Fill 1 diffuse colour"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      <ToolPanel.Section title="Fill 2">
        <ToolPanel.Row label="Intensity">
          <Spinner
            value={fill2.intensity}
            onChange={(v) => updateFill2({ intensity: v })}
            min={0}
            max={2}
            step={0.05}
            aria-label="Fill 2 intensity"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Azimuth">
          <Spinner
            value={fill2.az}
            onChange={(v) => updateFill2({ az: v })}
            min={-180}
            max={180}
            step={1}
            unit="°"
            disabled={forceAlign}
            aria-label="Fill 2 azimuth"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Altitude">
          <Spinner
            value={fill2.alt}
            onChange={(v) => updateFill2({ alt: v })}
            min={-90}
            max={90}
            step={1}
            unit="°"
            disabled={forceAlign}
            aria-label="Fill 2 altitude"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Diffuse">
          <ColorButton
            value={rgbToColorButtonValue(fill2.diffuse)}
            onChange={(rgb) => updateFill2({ diffuse: rgb })}
            aria-label="Fill 2 diffuse colour"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      {/* Force Align governs the two fill lights' azimuth (snaps them to
          sun.az + the canonical offsets and disables their az/alt
          spinners), so it lives directly under the fill sections it
          controls rather than in the footer. */}
      <div className="px-1 py-2">
        <label className="flex select-none items-center gap-1.5 text-xs text-text-2">
          <input
            type="checkbox"
            checked={forceAlign}
            onChange={(e) => handleForceAlignToggle(e.target.checked)}
            className="size-3.5 accent-sky-500"
            aria-label="Force Align Fill Lights"
          />
          Force Align Fill Lights
        </label>
      </div>

      <ToolPanel.Section title="Ambient" alwaysOpen>
        <ToolPanel.Row label="Colour">
          <ColorButton
            value={rgbToColorButtonValue(ambient)}
            onChange={updateAmbient}
            aria-label="Ambient colour"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      <ToolPanel.Section title="Shadow" alwaysOpen>
        <ToolPanel.Row label="Colour">
          <ColorButton
            value={rgbToColorButtonValue(shadow)}
            onChange={updateShadow}
            aria-label="Shadow colour"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      <ToolPanel.Footer>
        {/* T6 + T4: the tooltip only exists while the button is DISABLED
            (Force Align on) — disabled elements fire no pointer events, so
            the Tip rides an inline-block span wrapper. The footer is a
            flex-wrap row, so the span is layout-neutral (it becomes the
            flex item; the button keeps its own box). */}
        <Tip
          content={forceAlign ? "Disabled while Force Align is on" : undefined}
        >
          <span className="inline-block">
            <button
              type="button"
              onClick={handleMirrorSun}
              disabled={forceAlign}
              className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 disabled:cursor-not-allowed disabled:opacity-50 disabled:hover:bg-panel-2"
            >
              Mirror Sun
            </button>
          </span>
        </Tip>
        <button
          type="button"
          onClick={handleReset}
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3"
        >
          Reset
        </button>
      </ToolPanel.Footer>

      {/* Bloom sits at the very bottom, below the footer (Mirror Sun /
          Reset) per user request — it's a post-process effect unrelated
          to the lights, so it's the last thing in the pane. Collapsed by
          default; self-contained engine-state subscription.

          The 9px top margin matches `.panel-section`'s margin-bottom
          (components.css) — the same gap that separates two collapsed
          section headers (e.g. Fill 1 → Fill 2) — so the space above
          Bloom reads consistently with the rest of the pane. */}
      <div className="mt-[9px]">
        <BloomSection bridge={bridge} />
      </div>
    </ToolPanel>
  );
}
