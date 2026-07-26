// TexturePalettePopover — frequently-used texture palette.
//
// A Radix Popover (mirroring GroundDropdown/BackgroundDropdown) anchored to
// a trigger passed as `children` — in practice the palette button on
// TexturePickerField. Shows this mod's Pinned + Recent textures (backed by
// the C++ TexturePalette::Store) as a Color/Bump-filtered thumbnail grid.
// Clicking a thumbnail applies it (and closes); the star pins/unpins.
//
// Slot-aware: the filter defaults to the slot the palette was opened for
// (Color field → Color, Bump field → Bump). Thumbnails are fetched lazily
// per cell (`textures/palette/thumbnail`) and the host caches the decode.
// No active mod ⇒ the list is empty and the popover shows an honest hint.

import * as Popover from "@radix-ui/react-popover";
import { useEffect, useState, type ReactElement } from "react";
import type { Bridge, PaletteEntry } from "@particle-editor/bridge-schema";
import { AnimatedPopover } from "@/components/AnimatedPopover";
import { Tip } from "@/primitives/Tip";

type Slot = "color" | "bump";

type Props = {
  bridge: Bridge;
  slot: Slot;
  onApply: (filename: string) => void;
  /** Optional tooltip for the trigger. Rendered here (Tooltip.Trigger
   *  wrapping Popover.Trigger — the Radix-blessed nesting) because a Tip
   *  placed around the child at the call site would sit under
   *  Popover.Trigger asChild and swallow the trigger props. Side/occlusion
   *  are fixed for the single production caller (TexturePickerField in the
   *  right dock, which opens toward the viewport). */
  tip?: string;
  children: ReactElement;
};

export function TexturePalettePopover({ bridge, slot, onApply, tip, children }: Props) {
  const trigger = <Popover.Trigger asChild>{children}</Popover.Trigger>;
  return (
    <Popover.Root>
      {tip ? (
        <Tip content={tip} side="left">
          {trigger}
        </Tip>
      ) : (
        trigger
      )}
      <Popover.Portal>
        {/* #683: at small window sizes the palette grew past the window and
            the lower tiles were unreachable — Radix flips/shifts on collision
            but never shrinks content. Cap the CONTAINER to Radix's measured
            available space (the #228 skydome-picker lesson) and scroll inside;
            collisionPadding keeps a visible gutter at the window edge. */}
        <AnimatedPopover
          align="end"
          sideOffset={6}
          collisionPadding={12}
          aria-label="Texture palette"
          className="z-50 w-[560px] max-w-[var(--radix-popover-content-available-width)] max-h-[var(--radix-popover-content-available-height)] overflow-y-auto rounded-token border border-border-2 bg-panel p-3 shadow-[var(--shadow-soft)]"
        >
          <PaletteBody bridge={bridge} initialSlot={slot} onApply={onApply} />
        </AnimatedPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}

function PaletteBody({
  bridge,
  initialSlot,
  onApply,
}: {
  bridge: Bridge;
  initialSlot: Slot;
  onApply: (filename: string) => void;
}) {
  const [filter, setFilter] = useState<Slot>(initialSlot);
  const [hasMod, setHasMod] = useState(true);
  const [pins, setPins] = useState<PaletteEntry[]>([]);
  const [recents, setRecents] = useState<PaletteEntry[]>([]);
  const [status, setStatus] = useState<string | null>(null);

  const reload = (next: Slot) =>
    bridge
      .request({ kind: "textures/palette/list", params: { slot: next } })
      .then((r) => {
        setHasMod(r.hasMod);
        setPins(r.pins);
        setRecents(r.recents);
      })
      .catch(() => {
        /* leave prior state on transient failure */
      });

  // (Re)load whenever the filter changes — also the initial open, since
  // Radix mounts the content fresh each time the popover opens.
  useEffect(() => {
    let cancelled = false;
    void bridge
      .request({ kind: "textures/palette/list", params: { slot: filter } })
      .then((r) => {
        if (cancelled) return;
        setHasMod(r.hasMod);
        setPins(r.pins);
        setRecents(r.recents);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [bridge, filter]);

  const handleTogglePin = async (filename: string) => {
    setStatus(null);
    const r = await bridge.request({
      kind: "textures/palette/toggle-pin",
      params: { filename },
    });
    if (!r.ok) {
      setStatus("Pins full (max 12) — unpin one first.");
      return;
    }
    // Re-query so a toggled entry moves between the Pinned/Recent sections.
    await reload(filter);
  };

  return (
    <div className="flex flex-col gap-2">
      <div className="flex gap-1" role="group" aria-label="Texture slot filter">
        {(["color", "bump"] as Slot[]).map((s) => (
          <button
            key={s}
            type="button"
            onClick={() => setFilter(s)}
            aria-pressed={filter === s}
            className={`rounded px-3 py-1 text-xs transition focus-ring ${
              filter === s
                ? "bg-accent-strong text-white"
                : "bg-bg-2 text-text-2 hover:bg-bg-3"
            }`}
          >
            {s === "color" ? "Color" : "Bump"}
          </button>
        ))}
      </div>

      {!hasMod ? (
        <p className="px-1 py-6 text-center text-xs text-text-3">
          No mod selected — the palette tracks textures per mod.
        </p>
      ) : (
        <>
          <PaletteSection
            label="Pinned"
            entries={pins}
            bridge={bridge}
            onApply={onApply}
            onTogglePin={handleTogglePin}
          />
          <PaletteSection
            label="Recent"
            entries={recents}
            bridge={bridge}
            onApply={onApply}
            onTogglePin={handleTogglePin}
          />
        </>
      )}

      {status && (
        <p role="status" className="px-1 text-xs text-warning-fg">
          {status}
        </p>
      )}
    </div>
  );
}

function PaletteSection({
  label,
  entries,
  bridge,
  onApply,
  onTogglePin,
}: {
  label: string;
  entries: PaletteEntry[];
  bridge: Bridge;
  onApply: (filename: string) => void;
  onTogglePin: (filename: string) => void;
}) {
  // Pad to at least one full row of 4 so an empty/short section keeps the
  // faithful legacy grid shape (dashed empty cells).
  const padTo = Math.max(4, Math.ceil(entries.length / 4) * 4);
  const empties = padTo - entries.length;

  return (
    <div className="flex flex-col gap-1">
      <span className="text-[11px] font-medium uppercase tracking-wide text-text-3">
        {label}
      </span>
      <div className="grid grid-cols-4 gap-2">
        {entries.map((e) => (
          <PaletteCell
            key={e.filename}
            entry={e}
            bridge={bridge}
            onApply={onApply}
            onTogglePin={onTogglePin}
          />
        ))}
        {Array.from({ length: empties }, (_, i) => (
          <div
            key={`empty-${i}`}
            className="aspect-square rounded border border-dashed border-border-2/60"
            aria-hidden="true"
          />
        ))}
      </div>
    </div>
  );
}

function PaletteCell({
  entry,
  bridge,
  onApply,
  onTogglePin,
}: {
  entry: PaletteEntry;
  bridge: Bridge;
  onApply: (filename: string) => void;
  onTogglePin: (filename: string) => void;
}) {
  // undefined = still loading; otherwise the decoded URI + why-no-image status.
  // The host distinguishes a missing file (typo'd path) from a broken
  // texture (present but won't decode) so we can show different placeholders.
  type Thumb = { dataUri: string | null; status: "ok" | "missing" | "broken" };
  const [thumb, setThumb] = useState<Thumb | undefined>(undefined);

  useEffect(() => {
    let cancelled = false;
    void bridge
      .request({ kind: "textures/palette/thumbnail", params: { filename: entry.filename } })
      .then((r) => {
        if (!cancelled) setThumb({ dataUri: r.dataUri, status: r.status });
      })
      .catch(() => {
        // A transport/decode failure is, for the user, indistinguishable from a
        // corrupt texture — show the broken placeholder rather than nothing.
        if (!cancelled) setThumb({ dataUri: null, status: "broken" });
      });
    return () => {
      cancelled = true;
    };
  }, [bridge, entry.filename]);

  return (
    <div className="relative">
      {/* Tip wraps the Popover.Close (not the button inside it) — Tooltip
          trigger around another Radix asChild trigger is the blessed
          nesting; both forward their props down to the button. */}
      <Tip content={entry.filename}>
        <Popover.Close asChild>
          <button
            type="button"
            onClick={() => onApply(entry.filename)}
            aria-label={`Apply ${entry.filename}`}
            data-testid={`palette-apply-${entry.filename}`}
            className="relative block aspect-square w-full overflow-hidden rounded border border-border-2 transition motion-reduce:transition-none hover:border-accent focus-ring-inset"
          >
            {thumb?.dataUri ? (
              <img src={thumb.dataUri} alt="" className="absolute inset-0 h-full w-full object-cover" />
            ) : (
              <div
                data-testid={`palette-thumb-placeholder-${entry.filename}`}
                data-thumb-status={thumb ? thumb.status : "loading"}
                className={`absolute inset-0 flex flex-col items-center justify-center gap-0.5 ${
                  thumb?.status === "broken"
                    ? "bg-danger/10 text-danger-fg"
                    : thumb?.status === "missing"
                      ? "bg-bg-2 text-text-3"
                      : "bg-bg-2"
                }`}
              >
                {thumb && (
                  <>
                    <span aria-hidden="true" className="text-base leading-none">
                      {thumb.status === "broken" ? "⚠" : "?"}
                    </span>
                    <span className="text-[9px] uppercase tracking-wide">
                      {thumb.status === "broken" ? "broken" : "missing"}
                    </span>
                  </>
                )}
              </div>
            )}
            <span className="absolute inset-x-0 bottom-0 truncate bg-bg/80 px-1 py-0.5 text-left text-[10px] text-text backdrop-blur-sm">
              {entry.filename}
            </span>
          </button>
        </Popover.Close>
      </Tip>
      <button
        type="button"
        onClick={() => onTogglePin(entry.filename)}
        aria-label={`${entry.pinned ? "Unpin" : "Pin"} ${entry.filename}`}
        className={`absolute right-0.5 top-0.5 flex size-5 items-center justify-center rounded text-sm focus-ring ${
          entry.pinned ? "text-warning-fg" : "text-text-3 hover:text-text"
        }`}
      >
        {entry.pinned ? "★" : "☆"}
      </button>
    </div>
  );
}
