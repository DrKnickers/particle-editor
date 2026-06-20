// BackgroundPicker — right-side panel that picks the engine background.
//
// Two ways to set the background:
//   1. Game dome — a real in-game/in-mod skydome chosen by GameObject
//      Name for a battle context (Land/Space), with an independent primary and
//      secondary dome. Names are enumerated live from the game/mod's
//      *Skydomes.xml via `engine/query/skydome-list`; selection drives
//      `engine/set/skydome-environment`. This is the faithful path (the editor
//      loads the real .alo + runs each sub-mesh's own game shader).
//   2. Solid colour (the fallback when no game dome renders) → `engine/set/background`.
//
// Picking the solid colour clears the game-dome selection, and choosing a dome
// supersedes the solid colour, so the two are mutually exclusive from the
// user's view.
//
// A dome that's *selected* isn't necessarily *rendering*: if its .alo
// won't load, the engine silently falls back to the solid background. The
// snapshot now carries a per-slot load status (skydomePrimaryStatus /
// skydomeSecondaryStatus) so the picker surfaces that failure and an
// active-mode indicator instead of showing the dome as if it applied. (The
// former "custom skydome texture" slots were removed here in S49 — the new UI
// is Game dome + Solid colour only.)
//
// State: one-shot `engine/state/snapshot` at mount + a live
// `engine/state/changed` subscription (so external bridge mutations reflect).
//
// Browser/mock mode: `skydome-list` returns a small canned set; selecting a
// name in MOCK_MISSING_DOMES drives the load-failed status path — enough to
// validate the surfacing logic against the schema without a real install.

import { useEffect, useRef, useState } from "react";
import type {
  Bridge,
  EngineStateDto,
  SkydomeSlotStatus,
} from "@particle-editor/bridge-schema";
import { colorrefToHex, hexToColorref } from "@/lib/colorref";
import { ToolPanel } from "@/components/ToolPanel";

type Props = {
  bridge: Bridge;
  onClose: () => void;
};

type BodyProps = {
  bridge: Bridge;
};

const NONE = ""; // empty Name = no dome in that slot

/**
 * BackgroundPickerBody — the picker content (game-dome section + solid-colour
 * fallback). Extracted so both the legacy default-export wrapper and the
 * BackgroundDropdown popover mount the same markup.
 */
export function BackgroundPickerBody({ bridge }: BodyProps) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);
  const [primaryNames, setPrimaryNames] = useState<string[]>([]);
  const [secondaryNames, setSecondaryNames] = useState<string[]>([]);
  const colorInputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setSnapshot(s); })
      .catch((err) => console.warn("[BackgroundPicker] snapshot failed:", err));
    const off = bridge.on("engine/state/changed", (e) => {
      setSnapshot(e.payload);
    });
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);

  const context = snapshot?.skydomeContext ?? "space";
  const primaryName = snapshot?.skydomePrimaryName ?? NONE;
  const secondaryName = snapshot?.skydomeSecondaryName ?? NONE;
  const primaryStatus: SkydomeSlotStatus = snapshot?.skydomePrimaryStatus ?? "none";
  const secondaryStatus: SkydomeSlotStatus = snapshot?.skydomeSecondaryStatus ?? "none";
  const backgroundHex = snapshot ? colorrefToHex(snapshot.background) : "#000000";
  // The legacy bundled/custom texture-dome slot (0 = Off → solid colour). The
  // new UI no longer manages slots 1-11, but a value persisted from a prior
  // session still drives the engine's RenderSkydome() fallback, so we must read
  // it to report the active mode honestly (see legacyTextureActive below).
  const selectedSlot = snapshot?.skydomeSlot ?? 0;

  // A game dome is *selected* when either slot carries a Name — this drives the
  // controlled dropdowns + the mutual-exclusion clear below. It is actually
  // *rendering* only when at least one slot's .alo loaded (status "ok"); the
  // engine falls back to the simple background otherwise. Keeping these two
  // distinct is the whole fix: a selected-but-failed dome must NOT read as
  // active (the bug this surfaces — it silently showed the simple background).
  const gameDomeSelected = primaryName !== NONE || secondaryName !== NONE;
  const gameDomeRendering = primaryStatus === "ok" || secondaryStatus === "ok";
  // When no game dome renders, the simple background is what shows — either a
  // persisted legacy texture dome (slot != 0) or the solid colour (slot 0). The
  // texture slots are retired from this UI, but a leftover persisted one still
  // renders, so surface it honestly rather than claiming "Solid colour".
  const legacyTextureActive = !gameDomeRendering && selectedSlot !== 0;

  // (Re)enumerate the selectable Names whenever the battle context changes.
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/query/skydome-list", params: { context } })
      .then((r) => {
        if (cancelled) return;
        setPrimaryNames(r.primary ?? []);
        setSecondaryNames(r.secondary ?? []);
      })
      .catch((err) => console.warn("[BackgroundPicker] skydome-list failed:", err));
    return () => { cancelled = true; };
  }, [bridge, context]);

  const setEnvironment = (
    ctx: "land" | "space",
    primary: string,
    secondary: string,
  ) => {
    void bridge.request({
      kind: "engine/set/skydome-environment",
      params: { context: ctx, primaryName: primary, secondaryName: secondary },
    });
  };

  const handleContextChange = (ctx: "land" | "space") => {
    if (ctx === context) return;
    // Switching context invalidates the chosen Names (different lists), so clear
    // them; the enumeration effect repopulates the dropdowns for the new context.
    setEnvironment(ctx, NONE, NONE);
  };

  const handlePrimaryChange = (name: string) =>
    setEnvironment(context, name, secondaryName);

  const handleSecondaryChange = (name: string) =>
    setEnvironment(context, primaryName, name);

  // Always include the current selection as an option, even if it's not in the
  // freshly-enumerated list yet (async load) or at all (a persisted/mod dome
  // absent from this context's list) — otherwise the controlled <select> would
  // silently fall back to "None" and misreport the active dome.
  const withCurrent = (name: string, list: string[]) =>
    name && !list.includes(name) ? [name, ...list] : list;
  const primaryOptions = withCurrent(primaryName, primaryNames);
  const secondaryOptions = withCurrent(secondaryName, secondaryNames);

  // Per-slot "couldn't load" note (mirrors ReferenceObjectPicker's statusNote).
  const failNote = (status: SkydomeSlotStatus) =>
    status === "load-failed" ? "Couldn't load this dome's model." : null;
  const primaryNote = failNote(primaryStatus);
  const secondaryNote = failNote(secondaryStatus);

  // --- solid-colour fallback handlers (also clear the game dome) ---
  const clearGameDome = () => {
    if (gameDomeSelected) setEnvironment(context, NONE, NONE);
  };

  const handleSolidColorClick = () => {
    clearGameDome();
    void bridge.request({ kind: "engine/set/skydome-slot", params: { slot: 0 } });
    colorInputRef.current?.click();
  };

  const handleColorChange = (hex: string) => {
    void bridge.request({
      kind: "engine/set/background",
      params: { rgb: hexToColorref(hex) },
    });
  };

  // The solid colour is selected only when it's actually what renders: no game
  // dome AND no legacy texture slot (a persisted texture dome would render
  // instead, so the solid swatch must not falsely read as active).
  const simpleSolidSelected = !gameDomeRendering && !legacyTextureActive;

  return (
    <div className="flex flex-col gap-4">
      {/* ── Active-mode indicator: what the renderer is actually showing ── */}
      <p
        role="status"
        aria-label="Active background"
        className="flex flex-wrap items-center gap-2 text-xs"
      >
        <span className="text-text-3">Showing:</span>
        <span
          className={`rounded px-2 py-0.5 font-medium ${
            gameDomeRendering ? "bg-accent text-white" : "bg-bg-2 text-text-2"
          }`}
        >
          {gameDomeRendering
            ? "Game dome"
            : legacyTextureActive
              ? "Custom texture"
              : "Solid colour"}
        </span>
        {gameDomeSelected && !gameDomeRendering && (
          <span className="text-warning">— selected dome didn't load</span>
        )}
      </p>

      {/* ── Game dome (real .alo skydome) ────────────────────────────── */}
      <section className="flex flex-col gap-2">
        <div className="flex items-center justify-between">
          <span className="text-xs font-medium uppercase tracking-wide text-text-3">
            Game dome
          </span>
          {/* Land / Space context toggle */}
          <div role="group" aria-label="Battle context" className="flex rounded-md border border-border">
            {(["space", "land"] as const).map((ctx) => (
              <button
                key={ctx}
                type="button"
                onClick={() => handleContextChange(ctx)}
                aria-pressed={context === ctx}
                className={`px-2 py-0.5 text-xs capitalize transition first:rounded-l-md last:rounded-r-md ${
                  context === ctx ? "bg-accent text-white" : "text-text-2 hover:bg-bg-2"
                }`}
              >
                {ctx}
              </button>
            ))}
          </div>
        </div>

        <label className="flex flex-col gap-1 text-xs text-text-2">
          Primary
          <select
            value={primaryName}
            onChange={(e) => handlePrimaryChange(e.target.value)}
            aria-label="Primary dome"
            className="rounded-md border border-border bg-bg-2 px-2 py-1 text-sm text-text"
          >
            <option value={NONE}>None</option>
            {primaryOptions.map((n) => (
              <option key={n} value={n}>{n}</option>
            ))}
          </select>
        </label>
        {primaryNote && (
          <p role="alert" className="text-xs text-warning">{primaryNote}</p>
        )}

        <label className="flex flex-col gap-1 text-xs text-text-2">
          Secondary
          <select
            value={secondaryName}
            onChange={(e) => handleSecondaryChange(e.target.value)}
            aria-label="Secondary dome"
            className="rounded-md border border-border bg-bg-2 px-2 py-1 text-sm text-text"
          >
            <option value={NONE}>None</option>
            {secondaryOptions.map((n) => (
              <option key={n} value={n}>{n}</option>
            ))}
          </select>
        </label>
        {secondaryNote && (
          <p role="alert" className="text-xs text-warning">{secondaryNote}</p>
        )}
      </section>

      {/* ── Solid colour (fallback when no game dome renders) ─────────── */}
      <section className="flex flex-col gap-2">
        <span className="text-xs font-medium uppercase tracking-wide text-text-3">
          Solid colour
        </span>

        <button
          type="button"
          onClick={handleSolidColorClick}
          className={`relative flex h-14 items-center justify-center rounded-md border-2 transition ${
            simpleSolidSelected ? "border-accent" : "border-border hover:border-border-2"
          }`}
          style={{ backgroundColor: backgroundHex }}
          aria-label="Solid colour"
          aria-pressed={simpleSolidSelected}
        >
          <span className="rounded bg-bg/70 px-2 py-0.5 text-xs text-text backdrop-blur-sm">
            Solid colour
          </span>
          {simpleSolidSelected && (
            <span className="absolute right-1 top-1 flex size-5 items-center justify-center rounded-full bg-accent text-xs text-white">
              ✓
            </span>
          )}
        </button>
        <input
          ref={colorInputRef}
          type="color"
          value={backgroundHex}
          onChange={(e) => handleColorChange(e.target.value)}
          className="sr-only pointer-events-none absolute"
          tabIndex={-1}
          aria-hidden="true"
        />
      </section>
    </div>
  );
}

/**
 * BackgroundPicker — thin <ToolPanel> wrapper around BackgroundPickerBody,
 * kept as the default export for the existing slide-in callsite + spec.
 */
export function BackgroundPicker({ bridge, onClose }: Props) {
  return (
    <ToolPanel
      title="Background picker"
      onClose={onClose}
    >
      <BackgroundPickerBody bridge={bridge} />
    </ToolPanel>
  );
}
