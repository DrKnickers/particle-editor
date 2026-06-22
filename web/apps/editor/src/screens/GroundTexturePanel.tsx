// GroundTexturePanel — modeless tool window for the ground plane:
// show/hide master toggle plus a grid of texture slots. Ported from the
// native `GroundTexturePickerProc` in the legacy main.cpp; the Win32 dialog
// and the `--legacy-ui` opt-out were removed in, so this React panel
// is now the sole ground-texture surface.
//
// Slot layout (mirrors Engine::kGroundTextureCount=8 / kGroundSolidColorSlot=4):
//   - Slot 0: Dirt   (bundled, default)
//   - Slot 1: Grass  (bundled)
//   - Slot 2: Sand   (bundled)
//   - Slot 3: Snow   (bundled)
//   - Slot 4: Solid colour — wide tile + ColorButton popover
//   - Slot 5: Custom 1 (user-picked DDS/TGA texture)
//   - Slot 6: Custom 2
//   - Slot 7: Custom 3
//
// Bridge surface:
//   - engine/set/ground                  { enabled }
//   - engine/set/ground-texture          { slot }
//   - engine/set/ground-solid-color      { rgb }
//   - engine/set/ground-slot-custom-path { slot, path }   (custom slots)
//   - file/open with `filter: "ground"`                   (native picker)
//
// Custom-slot behaviour. Click on an empty custom slot chains the
// native picker (`file/open` with `filter: "ground"`, defaulting to
// `*.dds;*.tga`) through `engine/set/ground-slot-custom-path` +
// `engine/set/ground-texture { slot }`. Mirrors BackgroundPicker's
// custom-skydome flow. In browser mode the picker resolves to
// `{ ok: false }` so the chain aborts silently — there's no native
// picker to invoke without the host. Populated custom slots just
// switch via `engine/set/ground-texture { slot }`.

import { useEffect, useRef, useState } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { ToolPanel } from "@/components/ToolPanel";
import { colorrefToHex, hexToColorref } from "@/lib/colorref";

type Props = {
  bridge: Bridge;
  onClose: () => void;
};

type BodyProps = {
  bridge: Bridge;
};

type BundledSlot = {
  readonly slot: number;
  readonly name: string;
  readonly gradient: string;
};

const SOLID_COLOR_SLOT = 4;

export const BUNDLED_GROUND_SLOTS: readonly BundledSlot[] = [
  { slot: 0, name: "Dirt",  gradient: "linear-gradient(180deg, #6b5b3a 0%, #8c7a54 100%)" },
  { slot: 1, name: "Grass", gradient: "linear-gradient(180deg, #3a7a3a 0%, #5ca35c 100%)" },
  { slot: 2, name: "Sand",  gradient: "linear-gradient(180deg, #c2a872 0%, #e1c89a 100%)" },
  { slot: 3, name: "Snow",  gradient: "linear-gradient(180deg, #e0e0e8 0%, #ffffff 100%)" },
] as const;

const CUSTOM_SLOTS: readonly number[] = [5, 6, 7];

function basename(path: string): string {
  if (!path) return "";
  const norm = path.replace(/\\/g, "/");
  const i = norm.lastIndexOf("/");
  return i >= 0 ? norm.slice(i + 1) : norm;
}

/**
 * GroundTexturePanelBody — the slot-grid + solid-colour-picker markup
 * that used to live inside <ToolPanel>. Extracted so both the legacy
 * default-export wrapper and the new GroundDropdown popover can mount
 * the same content. No onClose: the host (popover or ToolPanel) handles
 * its own dismissal.
 */
export function GroundTexturePanelBody({ bridge }: BodyProps) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) setSnapshot(s);
      })
      .catch((err) => console.warn("[GroundTexturePanel] snapshot failed:", err));
    const off = bridge.on("engine/state/changed", (e) => {
      setSnapshot(e.payload);
    });
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);

  const colorInputRef = useRef<HTMLInputElement | null>(null);

  const groundOn = snapshot?.ground ?? false;
  const selectedSlot = snapshot?.groundTexture ?? 0;
  const groundZ = snapshot?.groundZ ?? 0;
  const solidHex = snapshot ? colorrefToHex(snapshot.groundSolidColor) : "#888888";
  // Custom-slot paths live at the array's tail. The bridge DTO carries
  // all 8 slots indexed by slot number; we read 5..7 directly.
  const customPaths = snapshot?.groundSlotCustomPaths ?? [];
  // Unit grid — viewport scenery, sits with the ground plane (S48: relocated
  // here from the Reference-object picker). Bridge kinds are general grid kinds,
  // unchanged by the move.
  const gridVisible = snapshot?.gridVisible ?? false;
  const gridSpacing = snapshot?.gridSpacing ?? 20;

  const handleToggleGround = (v: boolean) => {
    void bridge.request({ kind: "engine/set/ground", params: { enabled: v } });
  };
  const handleSelectSlot = (slot: number) => {
    void bridge.request({ kind: "engine/set/ground-texture", params: { slot } });
  };
  const handleSolidColorChange = (hex: string) => {
    void bridge.request({
      kind: "engine/set/ground-solid-color",
      params: { rgb: hexToColorref(hex) },
    });
    // Selecting a colour switches to the solid-colour slot as well, so
    // the change is immediately visible without an extra click.
    if (selectedSlot !== SOLID_COLOR_SLOT) {
      void bridge.request({
        kind: "engine/set/ground-texture",
        params: { slot: SOLID_COLOR_SLOT },
      });
    }
  };
  // Clicking the wide solid-colour tile selects the slot AND pops the
  // native colour picker (mirrors BackgroundPicker's proven pattern — an
  // OS dialog, immune to the viewport occlusion a DOM popover hits,
  // and discoverable because the obvious target is the one that opens it).
  const handleSolidColorClick = () => {
    if (selectedSlot !== SOLID_COLOR_SLOT) {
      void bridge.request({
        kind: "engine/set/ground-texture",
        params: { slot: SOLID_COLOR_SLOT },
      });
    }
    colorInputRef.current?.click();
  };
  // Ground-plane height (legacy). Session-only — the engine has no
  // persistence for it, matching the legacy spinner.
  const handleGroundZChange = (z: number) => {
    void bridge.request({ kind: "engine/set/ground-z", params: { z } });
  };
  const handleCustomClick = (slot: number, isEmpty: boolean) => {
    if (isEmpty) {
      // Chain: native picker (DDS/TGA filter) → write the chosen path
      // into the slot → activate the slot. Aborts silently on cancel
      // or failure. Mirrors BackgroundPicker's custom-skydome flow.
      void (async () => {
        const r = await bridge.request({
          kind: "file/open",
          params: { filter: "ground" },
        });
        if (!r.ok || !r.path) return;
        await bridge.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot, path: r.path },
        });
        await bridge.request({
          kind: "engine/set/ground-texture",
          params: { slot },
        });
      })();
      return;
    }
    handleSelectSlot(slot);
  };

  return (
    <>
      <label className="mb-3 flex items-center gap-2 text-xs text-text">
        <input
          type="checkbox"
          checked={groundOn}
          onChange={(e) => handleToggleGround(e.target.checked)}
          aria-label="Show ground"
          className="size-3 accent-sky-500"
        />
        <span>Show ground</span>
      </label>

      {/* Ground-plane height (legacy). Enabled only when the ground
          is shown, in lockstep with the toggle — matches the legacy
          spinner. */}
      <div className="mb-3 flex items-center justify-between gap-2 text-xs text-text">
        <span className={groundOn ? "" : "opacity-40"}>Height</span>
        <Spinner
          value={groundZ}
          onChange={handleGroundZChange}
          min={-100}
          max={100}
          step={0.1}
          decimals={1}
          disabled={!groundOn}
          density="tight"
          unit="units"
          aria-label="Ground height"
        />
      </div>

      {/* Solid-colour slot — wide tile. Clicking it selects the slot and
          pops the native colour picker via the hidden <input type="color">
          below (mirrors BackgroundPicker). */}
      <div className="mb-3">
        <button
          type="button"
          onClick={handleSolidColorClick}
          className={`relative flex h-16 w-full items-center justify-between rounded-md border-2 px-3 transition ${
            selectedSlot === SOLID_COLOR_SLOT
              ? "border-accent"
              : "border-border hover:border-border-2"
          }`}
          style={{ backgroundColor: solidHex }}
          aria-label="Solid colour"
          aria-pressed={selectedSlot === SOLID_COLOR_SLOT}
        >
          <span className="rounded bg-bg/70 px-2 py-0.5 text-xs text-text backdrop-blur-sm">
            Solid colour
          </span>
          {selectedSlot === SOLID_COLOR_SLOT && (
            <span className="flex size-5 items-center justify-center rounded-full bg-accent text-xs text-white">
              ✓
            </span>
          )}
        </button>
        {/* Hidden native colour input. Clicking the solid-colour tile
            triggers it programmatically. */}
        <input
          ref={colorInputRef}
          type="color"
          value={solidHex}
          onChange={(e) => handleSolidColorChange(e.target.value)}
          className="sr-only pointer-events-none absolute"
          tabIndex={-1}
          aria-hidden="true"
        />
      </div>

      {/* Bundled slots 0..3 — 2×2 grid. */}
      <div className="mb-3 grid grid-cols-2 gap-2">
        {BUNDLED_GROUND_SLOTS.map(({ slot, name, gradient }) => {
          const selected = selectedSlot === slot;
          return (
            <button
              key={slot}
              type="button"
              onClick={() => handleSelectSlot(slot)}
              className={`relative aspect-square overflow-hidden rounded-md border-2 transition ${
                selected ? "border-accent" : "border-border hover:border-border-2"
              }`}
              aria-label={name}
              aria-pressed={selected}
            >
              <div className="absolute inset-0" style={{ background: gradient }} />
              <span className="absolute inset-x-0 bottom-0 truncate bg-bg/80 px-1 py-0.5 text-center text-xs text-text backdrop-blur-sm">
                {name}
              </span>
              {selected && (
                <span className="absolute right-1 top-1 flex size-5 items-center justify-center rounded-full bg-accent text-xs text-white">
                  ✓
                </span>
              )}
            </button>
          );
        })}
      </div>

      {/* Custom slots 5..7 — 3-column grid, browse placeholders. */}
      <div className="grid grid-cols-3 gap-2">
        {CUSTOM_SLOTS.map((slot) => {
          const path = customPaths[slot] ?? "";
          const isEmpty = path === "";
          const selected = selectedSlot === slot;
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
              aria-label={isEmpty ? `Custom slot ${slot - 4} (empty)` : `Custom slot ${slot - 4}: ${label}`}
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

      {/* Unit grid — viewport scenery; belongs with the ground plane (S48,
          relocated from the Reference-object picker). Same bridge kinds. */}
      <label className="mb-3 mt-3 flex items-center gap-2 text-xs text-text">
        <input
          type="checkbox"
          checked={gridVisible}
          onChange={(e) =>
            void bridge.request({ kind: "engine/set/grid-visible", params: { visible: e.target.checked } })
          }
          aria-label="Grid visible"
          className="size-3 accent-sky-500"
        />
        <span>Show grid</span>
      </label>
      <div className="mb-3 flex items-center justify-between gap-2 text-xs text-text">
        <span className={gridVisible ? "" : "opacity-40"}>Grid spacing</span>
        <Spinner
          value={gridSpacing}
          onChange={(v) =>
            void bridge.request({ kind: "engine/set/grid-spacing", params: { spacing: v } })
          }
          min={1}
          step={5}
          decimals={0}
          disabled={!gridVisible}
          density="tight"
          aria-label="Grid spacing"
        />
      </div>
    </>
  );
}

/**
 * GroundTexturePanel — thin <ToolPanel> wrapper around
 * GroundTexturePanelBody. Kept as the default export so the existing
 * vitest spec (GroundTexturePanel.test.tsx) and any remaining slide-in
 * callsite still compile. The new toolbar dropdown (GroundDropdown)
 * mounts GroundTexturePanelBody directly inside a Radix Popover and
 * never reaches this wrapper.
 */
export function GroundTexturePanel({ bridge, onClose }: Props) {
  return (
    <ToolPanel
      title="Ground Texture"
      onClose={onClose}
    >
      <GroundTexturePanelBody bridge={bridge} />
    </ToolPanel>
  );
}
