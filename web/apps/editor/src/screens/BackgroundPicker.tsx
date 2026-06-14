// BackgroundPicker — right-side panel that picks the engine background.
//
// Two ways to set the background:
//   1. Game dome — a real in-game/in-mod skydome chosen by GameObject
//      Name for a battle context (Land/Space), with an independent primary and
//      secondary dome. Names are enumerated live from the game/mod's
//      *Skydomes.xml via `engine/query/skydome-list`; selection drives
//      `engine/set/skydome-environment`. This is the faithful path (the editor
//      loads the real .alo + runs each sub-mesh's own game shader).
//   2. Simple background (fallback when no game dome is selected):
//        - Solid colour (slot 0)        → `engine/set/background`
//        - Custom skydome texture (9-11) → native picker → `skydome-custom-path`
//      Picking a simple background clears the game-dome selection, and vice
//      versa, so the two are mutually exclusive from the user's view.
//
// State: one-shot `engine/state/snapshot` at mount + a live
// `engine/state/changed` subscription (so external bridge mutations reflect).
//
// Browser/mock mode: `skydome-list` returns a small canned set and the custom
// native picker resolves `{ ok: false }` — enough to validate selection state +
// dispatch surface against the schema without a real install.

import { useEffect, useRef, useState } from "react";
import type {
  Bridge,
  EngineStateDto,
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

const CUSTOM_SLOTS: readonly number[] = [9, 10, 11];
const NONE = ""; // empty Name = no dome in that slot

/** Pull just the file basename out of an absolute path. Handles both
 *  Windows-style and POSIX-style separators since the engine doesn't
 *  normalise them on the wire. */
function basename(path: string): string {
  if (!path) return "";
  const norm = path.replace(/\\/g, "/");
  const i = norm.lastIndexOf("/");
  return i >= 0 ? norm.slice(i + 1) : norm;
}

/**
 * BackgroundPickerBody — the picker content (game-dome section + simple
 * background fallback). Extracted so both the legacy default-export wrapper
 * and the BackgroundDropdown popover mount the same markup.
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
  const selectedSlot = snapshot?.skydomeSlot ?? 0;
  const backgroundHex = snapshot ? colorrefToHex(snapshot.background) : "#000000";
  const customPaths = snapshot?.skydomeCustomPaths ?? ["", "", ""];

  // A game dome is active when either slot carries a Name; that takes engine
  // render precedence over the simple-background slot below.
  const gameDomeActive = primaryName !== NONE || secondaryName !== NONE;

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

  // --- simple-background fallback handlers (also clear the game dome) ---
  const clearGameDome = () => {
    if (gameDomeActive) setEnvironment(context, NONE, NONE);
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

  const handleCustomClick = (slot: number, isEmpty: boolean) => {
    clearGameDome();
    if (isEmpty) {
      void (async () => {
        const r = await bridge.request({
          kind: "file/open",
          params: { filter: "skydome" },
        });
        if (!r.ok || !r.path) return;
        await bridge.request({
          kind: "engine/set/skydome-custom-path",
          params: { slot, path: r.path },
        });
        await bridge.request({ kind: "engine/set/skydome-slot", params: { slot } });
      })();
      return;
    }
    void bridge.request({ kind: "engine/set/skydome-slot", params: { slot } });
  };

  const simpleSolidSelected = !gameDomeActive && selectedSlot === 0;

  return (
    <div className="flex flex-col gap-4">
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
      </section>

      {/* ── Simple background (fallback when no game dome) ────────────── */}
      <section className="flex flex-col gap-2">
        <span className="text-xs font-medium uppercase tracking-wide text-text-3">
          Simple background
        </span>

        {/* Solid colour */}
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

        {/* Custom skydome textures (slots 9-11) */}
        <div className="grid grid-cols-3 gap-2">
          {CUSTOM_SLOTS.map((slot) => {
            const idx = slot - 9;
            const path = customPaths[idx] ?? "";
            const isEmpty = path === "";
            const selected = !gameDomeActive && selectedSlot === slot;
            const label = isEmpty ? "Browse..." : basename(path);
            return (
              <button
                key={slot}
                type="button"
                onClick={() => handleCustomClick(slot, isEmpty)}
                className={`relative aspect-square overflow-hidden rounded-md border-2 transition ${
                  selected
                    ? "border-accent"
                    : isEmpty
                      ? "border-dashed border-border-2 hover:border-border-2"
                      : "border-border hover:border-border-2"
                }`}
                aria-label={isEmpty ? `Custom slot ${idx + 1} (empty)` : `Custom slot ${idx + 1}: ${label}`}
                aria-pressed={selected}
              >
                {isEmpty ? (
                  <div className="flex h-full w-full flex-col items-center justify-center gap-1 bg-bg-2 text-text-3">
                    <span className="text-2xl leading-none">+</span>
                    <span className="text-xs">Browse...</span>
                  </div>
                ) : (
                  <>
                    <div className="absolute inset-0 bg-panel-2" />
                    <span className="absolute inset-x-0 bottom-0 truncate bg-bg/80 px-1 py-0.5 text-center text-xs text-text backdrop-blur-sm">
                      {label}
                    </span>
                    <span className="absolute right-1 top-1 flex size-5 items-center justify-center rounded-full bg-bg-2/80 text-xs text-text-2">
                      ↺
                    </span>
                  </>
                )}
                {selected && !isEmpty && (
                  <span className="absolute left-1 top-1 flex size-5 items-center justify-center rounded-full bg-accent text-xs text-white">
                    ✓
                  </span>
                )}
              </button>
            );
          })}
        </div>
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
      bridge={bridge}
      occlusionId="tool-panel:background"
    >
      <BackgroundPickerBody bridge={bridge} />
    </ToolPanel>
  );
}
