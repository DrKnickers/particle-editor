// BackgroundPicker — right-side slide-in panel that replaces the legacy
// `SkydomePickerProc` modal. Picks the engine background:
//   - Slot 0: solid colour (drives `engine/set/background`)
//   - Slots 1-8: bundled skydome textures (drives `engine/set/skydome-slot`)
//   - Slots 9-11: user-supplied custom skydomes. An empty-slot click
//     chains the native picker (`file/open` with `filter: "skydome"`,
//     defaulting to `*.dds;*.tga`) through
//     `engine/set/skydome-custom-path` + `engine/set/skydome-slot`.
//     Populated slots just re-activate the existing path.
//
// State subscription:
//   - One-shot `engine/state/snapshot` at mount for the initial DTO.
//   - Live `engine/state/changed` subscription so external mutations
//     (e.g. Playwright driving the bridge, devtools poking
//     `window.bridge`) reflect immediately.
//
// Browser-mode only: the bundled-tile gradients are static CSS swatches,
// not real skydome thumbnails. The native host will eventually serve
// real previews; until then this is enough to validate selection state +
// dispatch surface against the schema. Custom-slot clicks resolve to
// `{ ok: false }` in MockBridge (no native picker in the browser).

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

type BundledSlot = {
  readonly slot: number;
  readonly name: string;
  readonly gradient: string;
  readonly swatch: string;
};

export const BUNDLED_SLOTS: readonly BundledSlot[] = [
  { slot: 1, name: "Storm",          gradient: "linear-gradient(180deg, #2a3340 0%, #4a5568 100%)", swatch: "#2a3340" },
  { slot: 2, name: "Murky Clouds",   gradient: "linear-gradient(180deg, #5b6878 0%, #7a8696 100%)", swatch: "#5b6878" },
  { slot: 3, name: "Smog Clouds",    gradient: "linear-gradient(180deg, #8a8474 0%, #a39a82 100%)", swatch: "#8a8474" },
  { slot: 4, name: "Blue Horizon",   gradient: "linear-gradient(180deg, #5da3d4 0%, #9bc4e0 100%)", swatch: "#5da3d4" },
  { slot: 5, name: "Blue Sky",       gradient: "linear-gradient(180deg, #4a90e2 0%, #87cefa 100%)", swatch: "#4a90e2" },
  { slot: 6, name: "Orange Horizon", gradient: "linear-gradient(180deg, #d97a3a 0%, #f4a261 100%)", swatch: "#d97a3a" },
  { slot: 7, name: "Orange Sky",     gradient: "linear-gradient(180deg, #e07a3a 0%, #ffc56e 100%)", swatch: "#e07a3a" },
  { slot: 8, name: "Volcanic Storm", gradient: "linear-gradient(180deg, #2c1810 0%, #6b2c1f 100%)", swatch: "#2c1810" },
] as const;

const CUSTOM_SLOTS: readonly number[] = [9, 10, 11];

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
 * BackgroundPickerBody — the slot-grid + colour-picker chain markup that
 * used to live inside <ToolPanel>. Extracted so both the legacy
 * default-export wrapper and the new BackgroundDropdown popover can
 * mount the same content. No onClose: the host (popover or ToolPanel)
 * handles its own dismissal.
 */
export function BackgroundPickerBody({ bridge }: BodyProps) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);
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

  const selectedSlot = snapshot?.skydomeSlot ?? 0;
  const backgroundHex = snapshot ? colorrefToHex(snapshot.background) : "#000000";
  const customPaths = snapshot?.skydomeCustomPaths ?? ["", "", ""];

  const handleSolidColorClick = () => {
    // Switch the engine to the solid-colour slot, then trigger the
    // native colour picker. We *also* switch to slot 0 here so a fresh
    // open from any other slot lands on solid-colour as the user expects.
    void bridge.request({ kind: "engine/set/skydome-slot", params: { slot: 0 } });
    colorInputRef.current?.click();
  };

  const handleColorChange = (hex: string) => {
    void bridge.request({
      kind: "engine/set/background",
      params: { rgb: hexToColorref(hex) },
    });
  };

  const handleBundledClick = (slot: number) => {
    void bridge.request({ kind: "engine/set/skydome-slot", params: { slot } });
  };

  const handleCustomClick = (slot: number, isEmpty: boolean) => {
    if (isEmpty) {
      // Chain native picker → write the chosen path into the slot →
      // activate the slot. Each step awaits the previous; abort on
      // cancellation or failure (MockBridge returns ok:false in browser
      // mode, native returns ok:false on user-cancel). `filter: "skydome"`
      // pops the dialog with `*.dds;*.tga` as the default filter so the
      // user isn't fighting an `.alo`-by-default dropdown.
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

  return (
    <div className="flex flex-col gap-3">
        {/* Solid-colour row */}
        <div className="grid grid-cols-3 gap-2">
          <button
            type="button"
            onClick={handleSolidColorClick}
            className={`relative col-span-3 flex h-16 items-center justify-center rounded-md border-2 transition ${
              selectedSlot === 0 ? "border-accent" : "border-border hover:border-border-2"
            }`}
            style={{ backgroundColor: backgroundHex }}
            aria-label="Solid colour"
            aria-pressed={selectedSlot === 0}
          >
            <span className="rounded bg-bg/70 px-2 py-0.5 text-xs text-text backdrop-blur-sm">
              Solid colour
            </span>
            {selectedSlot === 0 && (
              <span className="absolute right-1 top-1 flex size-5 items-center justify-center rounded-full bg-accent text-xs text-white">
                ✓
              </span>
            )}
          </button>

          {/* Hidden native colour input. Clicking the solid-colour tile
              triggers it programmatically. */}
          <input
            ref={colorInputRef}
            type="color"
            value={backgroundHex}
            onChange={(e) => handleColorChange(e.target.value)}
            className="sr-only pointer-events-none absolute"
            tabIndex={-1}
            aria-hidden="true"
          />
        </div>

        {/* Bundled slots 1-8 */}
        <div className="grid grid-cols-3 gap-2">
          {BUNDLED_SLOTS.map(({ slot, name, gradient }) => {
            const selected = selectedSlot === slot;
            return (
              <button
                key={slot}
                type="button"
                onClick={() => handleBundledClick(slot)}
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

        {/* Custom slots 9-11 */}
        <div className="grid grid-cols-3 gap-2">
          {CUSTOM_SLOTS.map((slot) => {
            const idx = slot - 9;
            const path = customPaths[idx] ?? "";
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
    </div>
  );
}

/**
 * BackgroundPicker — thin <ToolPanel> wrapper around BackgroundPickerBody.
 * Kept as the default export so the existing vitest spec
 * (BackgroundPicker.test.tsx) and any remaining slide-in callsite still
 * compile. The new toolbar dropdown (BackgroundDropdown) mounts
 * BackgroundPickerBody directly inside a Radix Popover and never reaches
 * this wrapper.
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
