// AtlasPickerPanel — atlas frame grid + click-to-assign (Task 8 + 9 + 12).
//
// Displays the texture atlas for the selected emitter's colorTexture as a
// side×side cell grid.  Hover previews a cell; the selection.frame field from
// AtlasContext highlights the currently-assigned frame with the amber token
// --atlas-selected.  Clicking a cell assigns the frame to all selected index
// keys (Task 9 — with a confirm dialog for differing values).  Preview fetches
// are mod-stack-keyed (Task 12) so a mod switch invalidates stale results.
//
// Placeholder precedence (top = highest priority):
//   1. no colorTexture           → "No color texture set."
//   2. atlas too large           → "Atlas too large to display (N×N)."
//   3. textureSize < 4 (side<2)  → "Single frame — no atlas to pick from."
//   4. preview missing           → "Texture not found."
//   5. preview broken            → "Texture could not be read."
//   6. focusedTrack !== "index"  → "Select keys on the index channel…"
//   happy path                  → grid + preview box

import { useCallback, useEffect, useRef, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useAtlasContext } from "@/lib/atlas-context";
import { ToolPanel } from "@/components/ToolPanel";
import { AtlasConfirmModal } from "@/components/AtlasConfirmModal";
import {
  gridSide,
  frameCount,
  isAtlasTooLarge,
  resolveFrame,
  cellRect,
  columnsFromTemplate,
  fitGridLayout,
} from "@/lib/atlas-grid";

// Smart-grid sizing constants (§ "maximize available space"): cell gap (matches
// the grid's `gap-1` = 4px), and the min/max square thumbnail size.
const GRID_GAP = 4;
const GRID_MIN_CELL = 44;
const GRID_MAX_CELL = 160;
import { getPreviewCached, useTextureEpoch } from "@/lib/atlas-preview-cache";
import { useModStack } from "@/lib/mod-stack";

// ─── types ───────────────────────────────────────────────────────────────────

type PreviewState =
  | { kind: "loading" }
  | { kind: "ok"; dataUri: string; srcW: number; srcH: number }
  | { kind: "missing" }
  | { kind: "broken" };

// ─── component ───────────────────────────────────────────────────────────────

export function AtlasPickerPanel({
  bridge,
  onClose,
  closing,
}: {
  bridge: Bridge;
  onClose: () => void;
  closing?: boolean;
}) {
  const emitterId     = useAtlasContext((c) => c.emitterId);
  const focusedTrack  = useAtlasContext((c) => c.focusedTrack);
  const interpolation = useAtlasContext((c) => c.interpolation);
  const frame         = useAtlasContext((c) => c.selection.frame);
  const keyTimes      = useAtlasContext((c) => c.selection.keyTimes);
  const stack         = useModStack();
  const textureEpoch  = useTextureEpoch((s) => s.epoch); // re-fetch preview on a texture reload

  const [textureSize, setTextureSize]   = useState(1);
  const [colorTexture, setColorTexture] = useState("");
  const [preview, setPreview]           = useState<PreviewState>({ kind: "loading" });
  const [hover, setHover]               = useState<number | null>(null);
  const [focusIndex, setFocusIndex]     = useState<number | null>(null); // keyboard cursor
  const [confirmTarget, setConfirmTarget] = useState<{ frame: number; emitterId: number; keyTimes: number[] } | null>(null);
  const [announcement, setAnnouncement] = useState("");
  const [pulse, setPulse] = useState(false);
  const gridRef = useRef<HTMLDivElement | null>(null);
  const focusInGridRef = useRef(false);   // does a grid cell currently hold focus?
  const restoreFocusRef = useRef(false);  // re-home focus after an atlas change?
  const roRef = useRef<ResizeObserver | null>(null);
  const [gridW, setGridW] = useState(0); // measured content width of the scroll region
  const pulseTimer = useRef<number | undefined>(undefined);
  const emitterIdRef = useRef<number | null>(emitterId);
  useEffect(() => { emitterIdRef.current = emitterId; }, [emitterId]);
  useEffect(() => () => { if (pulseTimer.current) clearTimeout(pulseTimer.current); }, []);
  useEffect(() => () => roRef.current?.disconnect(), []);

  // Measure the scroll region's content width so the grid can size thumbnails to
  // fill the dock (re-fires on dock resize). A *callback ref* (not an effect)
  // attaches the observer when the scroll element actually mounts — the grid is
  // conditionally rendered (only once the preview loads), so an effect with []
  // deps would run before the element exists and never re-attach. `clientWidth`
  // includes the `p-3` padding — subtract it. jsdom lacks ResizeObserver (tests
  // stub the column count for keyboard nav and don't depend on this).
  const SCROLL_PAD = 24; // p-3 => 12px each side
  const setScrollEl = useCallback((el: HTMLDivElement | null) => {
    roRef.current?.disconnect();
    roRef.current = null;
    if (!el) return;
    const measure = () => setGridW(Math.max(0, el.clientWidth - SCROLL_PAD));
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    roRef.current = ro;
  }, []);

  function firePulse() {
    if (window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches) return;
    setPulse(true);
    if (pulseTimer.current) clearTimeout(pulseTimer.current);
    pulseTimer.current = window.setTimeout(() => setPulse(false), 250);
  }

  // ── fetch emitter properties ─────────────────────────────────────────────

  useEffect(() => {
    if (emitterId === null) return;
    let live = true;
    let inFlight = false;
    let again = false;
    const fetchProps = () => {
      if (inFlight) { again = true; return; } // coalesce an event burst into one round-trip
      inFlight = true;
      void bridge
        .request({ kind: "emitters/get-properties", params: { id: emitterId } })
        .then((r) => {
          inFlight = false;
          if (!live) return;
          setTextureSize(r.properties.textureSize);
          setColorTexture(r.properties.colorTexture);
          if (again) { again = false; fetchProps(); } // trailing fetch for events that arrived mid-flight
        })
        .catch(() => { inFlight = false; /* leave defaults → no-texture placeholder */ });
    };
    fetchProps();
    // Re-fetch on ANY emitter mutation (emitters/tree/changed is a broad
    // broadcast carrying no per-field detail) so a Color-texture swap via
    // emitters/set-properties refreshes the atlas + preview in place — not only
    // on an emitter-selection change. Coalesced above so an edit burst is one
    // round-trip; setTextureSize/setColorTexture bail on unchanged values, so
    // unrelated edits cost a cheap read and no re-render.
    const off = bridge.on("emitters/tree/changed", fetchProps);
    return () => {
      live = false;
      off();
    };
  }, [bridge, emitterId]);

  // ── fetch texture preview ─────────────────────────────────────────────────

  const side     = gridSide(textureSize);
  const tooLarge = isAtlasTooLarge(textureSize);
  const eligible = side >= 2;

  useEffect(() => {
    if (!eligible || tooLarge || !colorTexture) {
      setPreview({ kind: "loading" });
      return;
    }
    let live = true;
    setPreview({ kind: "loading" });
    void getPreviewCached(
      stack,
      colorTexture,
      () => bridge.request({ kind: "textures/get-preview", params: { filename: colorTexture } }),
    )
      .then((r) => {
        if (!live) return;
        if (r.status === "ok") {
          setPreview({ kind: "ok", dataUri: r.dataUri, srcW: r.srcW, srcH: r.srcH });
        } else {
          setPreview({ kind: r.status });
        }
      })
      .catch(() => {
        if (live) setPreview({ kind: "broken" });
      });
    return () => {
      live = false;
    };
  }, [bridge, colorTexture, eligible, stack, tooLarge, textureEpoch]);

  // ── stale-index reset (Risk 3) ─────────────────────────────────────────────
  // On emitter / texture / atlas-size change, reset the keyboard cursor to the
  // (resolved) assigned frame and clear hover. Intentionally NOT keyed on
  // `frame` — the cursor should persist across key-selection changes, resetting
  // only when the emitter/texture/atlas changes. (This project has no ESLint, so
  // no exhaustive-deps directive is needed.)
  useEffect(() => {
    if (focusInGridRef.current) restoreFocusRef.current = true;
    setFocusIndex(frame === null ? null : resolveFrame(frame, side));
    setHover(null);
  }, [emitterId, colorTexture, side]);

  // ── derived display values ────────────────────────────────────────────────

  const offIndex     = focusedTrack !== "index";

  // ── click-to-assign ──────────────────────────────────────────────────────

  async function assignAll(frameF: number, targetEmitterId: number | null, targetKeyTimes: number[]): Promise<boolean> {
    if (targetEmitterId === null || targetKeyTimes.length === 0) return false;
    const results = await Promise.allSettled(
      targetKeyTimes.map((t) =>
        bridge.request({
          kind: "emitters/set-track-key",
          params: { id: targetEmitterId, track: "index", oldTime: t, newTime: t, newValue: frameF },
        }),
      ),
    );
    const ok = results.every((r) => r.status === "fulfilled");
    if (!ok)
      console.warn("[atlas] some index frames could not be set; grid reflects the committed state.");
    // Highlight follows committed data via tree/changed → CurveEditorPanel republish.
    return ok;
  }

  // Gate the pulse + live-announce on a confirmed commit, guarded against an
  // emitter switch mid-await (§4.5).
  async function commitAssign(frameF: number, targetEmitterId: number | null, targetKeyTimes: number[]) {
    const started = targetEmitterId;
    const ok = await assignAll(frameF, targetEmitterId, targetKeyTimes);
    if (ok && emitterIdRef.current === started) {
      setFocusIndex(frameF);
      firePulse();
      setAnnouncement(`Assigned frame ${frameF}`);
    }
  }

  function onCellClick(k: number) {
    if (offIndex || keyTimes.length === 0) return;
    if (frame === null && keyTimes.length > 1) {
      if (emitterId !== null) setConfirmTarget({ frame: k, emitterId, keyTimes });
      return;
    }
    void commitAssign(k, emitterId, keyTimes);
  }

  const totalCells   = side * side;
  const fc           = frameCount(textureSize);
  // Show "M of N" only when the atlas has unused cells (frameCount < textureSize).
  const meta         = totalCells === Math.max(1, Math.floor(Number.isFinite(textureSize) ? textureSize : 1))
    ? `${side}×${side} · ${fc}`
    : `${side}×${side} · ${fc} of ${textureSize}`;
  const highlight    = frame === null ? null : resolveFrame(frame, side);
  // Clamp the keyboard cursor against the live grid (backstop for Risk 3).
  const safeFocus    = focusIndex !== null && focusIndex < totalCells ? focusIndex : null;
  // Clamp hover too, so a stale hover index (e.g. mid texture-swap, before the
  // reset effect clears it) can never feed cropStyle an out-of-range frame.
  const safeHover    = hover !== null && hover < totalCells ? hover : null;
  // The single tabbable cell: cursor, else the assigned frame, else 0 (never null).
  const rovingTarget = safeFocus ?? highlight ?? 0;
  const previewFrame = safeHover ?? safeFocus ?? highlight; // §3.1 precedence (?? — frame 0 valid)
  // Smart grid sizing (Approach A): ~sqrt(n) columns filling the measured width,
  // with a min-cell floor so large atlases stay dense.
  const layout = fitGridLayout(totalCells, gridW, GRID_GAP, GRID_MIN_CELL, GRID_MAX_CELL);

  // ── keyboard navigation (§4.2) ─────────────────────────────────────────────

  function focusCell(k: number) {
    const el = gridRef.current?.querySelector<HTMLElement>(`[data-frame="${k}"]`);
    el?.scrollIntoView?.({ block: "nearest", behavior: "auto" }); // instant; jsdom-tolerant
    el?.focus?.();
  }

  // If the atlas changed while a grid cell held keyboard focus, the focused cell
  // can unmount (e.g. a shrinking atlas) and drop focus to <body>. Re-home focus
  // onto the new roving cell. Gated on focusInGridRef (set/cleared by the grid's
  // focus/blur handlers in the render below) so opening the panel, or a texture
  // change triggered from elsewhere (focus already moved out of the grid), never
  // steals focus.
  useEffect(() => {
    if (!restoreFocusRef.current) return;
    restoreFocusRef.current = false;
    focusCell(rovingTarget);
  }, [rovingTarget, colorTexture, side]);

  function moveTo(next: number) {
    setFocusIndex(next);
    focusCell(next);
  }

  function liveColumns(): number {
    const tpl = gridRef.current ? getComputedStyle(gridRef.current).gridTemplateColumns : "";
    return columnsFromTemplate(tpl, totalCells, side);
  }

  function onGridKeyDown(e: React.KeyboardEvent<HTMLDivElement>) {
    if (offIndex) return;
    const cur = rovingTarget;
    const cols = liveColumns();
    let next: number | null = null;
    switch (e.key) {
      case "ArrowLeft":  next = Math.max(0, cur - 1); break;
      case "ArrowRight": next = Math.min(totalCells - 1, cur + 1); break;
      case "ArrowUp":    next = cur - cols >= 0 ? cur - cols : null; break;
      case "ArrowDown":  next = cur + cols < totalCells ? cur + cols : null; break;
      case "Home":       next = 0; break;
      case "End":        next = totalCells - 1; break;
      case "Enter":
      case " ":
        e.preventDefault();
        onCellClick(cur);
        return;
      default:
        return;
    }
    e.preventDefault(); // arrows/Home/End must not scroll the overflow container
    if (next !== null && next !== cur) moveTo(next);
  }

  // ── body content ─────────────────────────────────────────────────────────

  let body: React.ReactNode;

  if (!colorTexture) {
    body = <Placeholder>No color texture set.</Placeholder>;
  } else if (tooLarge) {
    body = <Placeholder>Atlas too large to display ({side}×{side}).</Placeholder>;
  } else if (!eligible) {
    body = <Placeholder>Single frame — no atlas to pick from.</Placeholder>;
  } else if (preview.kind === "missing") {
    body = <Placeholder>Texture not found.</Placeholder>;
  } else if (preview.kind === "broken") {
    body = <Placeholder>Texture could not be read.</Placeholder>;
  } else if (offIndex) {
    body = (
      <Placeholder>Select keys on the index channel to assign frames.</Placeholder>
    );
  } else {
    body = (
      <>
        {/* Pinned preview box */}
        <div className="shrink-0 p-3">
          <PreviewBox
            preview={preview}
            side={side}
            frame={previewFrame}
            rawFrame={frame}
            total={totalCells}
            pulse={pulse}
          />
        </div>
        {/* Scrollable cell grid */}
        {/* `scrollbar-gutter: stable` reserves the scrollbar space at all times
            so the measured width doesn't change when the vertical scrollbar
            appears/disappears — without it the ResizeObserver re-fit loops
            (grid grows -> scrollbar -> narrower -> smaller grid -> no scrollbar
            -> wider -> repeat), which reads as flicker. */}
        <div
          ref={setScrollEl}
          data-testid="atlas-scroll"
          className="min-h-0 flex-1 overflow-y-auto p-3"
          style={{ scrollbarGutter: "stable" }}
        >
          <div
            ref={gridRef}
            role="listbox"
            aria-label="Atlas frames"
            aria-multiselectable={false}
            onKeyDown={onGridKeyDown}
            onFocusCapture={() => { focusInGridRef.current = true; }}
            onBlurCapture={(e) => {
              // Clear only on a deliberate move to another real element; a cell
              // unmounting under focus blurs with relatedTarget null (→ <body>),
              // which we keep so the restore effect can re-home focus.
              if (e.relatedTarget) focusInGridRef.current = false;
            }}
            className="grid justify-center gap-1"
            style={{ gridTemplateColumns: `repeat(${layout.cols}, ${layout.cell}px)` }}
          >
            {Array.from({ length: totalCells }, (_, k) => (
              <Cell
                key={k}
                k={k}
                side={side}
                preview={preview}
                selected={k === highlight}
                focused={k === rovingTarget}
                onHover={setHover}
                onClick={onCellClick}
              />
            ))}
          </div>
        </div>
      </>
    );
  }

  // ── header meta visibility ────────────────────────────────────────────────

  const showMeta = eligible && !tooLarge && !!colorTexture;

  return (
    <ToolPanel title="Atlas Frames" onClose={onClose} variant="docked" closing={closing}>
      {/* Full-height flex column that negates ToolPanel's body padding so
          the pinned preview and scrollable grid can fill the available space. */}
      <div className="-m-3 flex h-full flex-col overflow-hidden">
        {/* Sub-header: atlas meta (grid dimensions + frame count) and
            interpolation badge. Shown only when the atlas is displayable. */}
        {showMeta && (
          <div className="flex shrink-0 items-center gap-2 border-b border-border px-3 py-1 text-xs text-text-3">
            <span data-testid="atlas-meta" className="min-w-0 flex-1 truncate">
              {meta}
            </span>
            {interpolation && (
              <span className="shrink-0 rounded border border-border px-1">
                {interpolation}
              </span>
            )}
          </div>
        )}
        {body}
      </div>
      <AtlasConfirmModal
        open={confirmTarget !== null}
        count={confirmTarget?.keyTimes.length ?? 0}
        frame={confirmTarget?.frame ?? 0}
        onConfirm={() => {
          const t = confirmTarget!;
          setConfirmTarget(null);
          void commitAssign(t.frame, t.emitterId, t.keyTimes).then(() => focusCell(t.frame));
        }}
        onCancel={() => {
          const t = confirmTarget;
          setConfirmTarget(null);
          if (t) focusCell(t.frame); // return focus to the interacted cell, not <body>
        }}
      />
      <div aria-live="polite" className="sr-only">{announcement}</div>
    </ToolPanel>
  );
}

// ─── sub-components ───────────────────────────────────────────────────────────

function Placeholder({ children }: { children: React.ReactNode }) {
  return (
    <div className="flex flex-1 items-center justify-center p-6 text-center text-xs text-text-3">
      {children}
    </div>
  );
}

/** Compute CSS background-* properties to crop the texture to cell k. */
function cropStyle(
  k: number,
  side: number,
  p: Extract<PreviewState, { kind: "ok" }>,
): React.CSSProperties {
  const r = cellRect(k, side, p.srcW, p.srcH);
  // backgroundSize: the full image is `side` cells wide/tall.
  // backgroundPosition: position the relevant cell into view.
  const posX =
    side > 1
      ? `${(r.left / (p.srcW - r.width)) * 100}%`
      : "0%";
  const posY =
    side > 1
      ? `${(r.top / (p.srcH - r.height)) * 100}%`
      : "0%";
  return {
    backgroundImage:    `url(${p.dataUri})`,
    backgroundRepeat:   "no-repeat",
    backgroundSize:     `${side * 100}% ${side * 100}%`,
    backgroundPosition: `${posX} ${posY}`,
  };
}

function Cell({
  k,
  side,
  preview,
  selected,
  focused,
  onHover,
  onClick,
}: {
  k: number;
  side: number;
  preview: PreviewState;
  selected: boolean;
  focused: boolean;
  onHover: (k: number | null) => void;
  onClick?: (k: number) => void;
}) {
  const style: React.CSSProperties =
    preview.kind === "ok" ? cropStyle(k, side, preview) : {};
  if (selected) {
    // amber via border + box-shadow ring so the blue focus OUTLINE can nest (§4.4)
    style.borderColor = "var(--atlas-selected)";
    style.boxShadow = "0 0 0 1px var(--atlas-selected), 0 0 12px rgba(255,176,0,.4)";
  }

  return (
    <div
      data-testid="atlas-cell"
      data-frame={k}
      data-selected={selected ? "true" : "false"}
      role="option"
      aria-selected={selected}
      aria-label={`Frame ${k}`}
      tabIndex={focused ? 0 : -1}
      className={`group relative aspect-square rounded bg-bg-2 transition ${
        selected ? "border-2" : "border border-border"
      } hover:z-10 hover:brightness-110 hover:scale-[1.08] motion-reduce:hover:scale-100 hover:shadow-[inset_0_0_0_2px_rgba(255,255,255,0.7)] focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1 focus-visible:outline-[var(--accent)]`}
      style={style}
      onMouseEnter={() => onHover(k)}
      onMouseLeave={() => onHover(null)}
      onClick={() => onClick?.(k)}
    >
      {/* Frame number: hidden by default, revealed on hover; amber badge when assigned */}
      <span
        className={`pointer-events-none absolute bottom-0.5 left-0.5 rounded-sm px-1 text-[9px] leading-tight transition-opacity ${
          selected
            ? "bg-[var(--atlas-selected)] font-bold text-black opacity-100"
            : "bg-black/70 text-[#eee] opacity-0 group-hover:opacity-100"
        }`}
      >
        {k}
      </span>
    </div>
  );
}

function PreviewBox({
  preview,
  side,
  frame,
  rawFrame,
  total,
  pulse,
}: {
  preview: PreviewState;
  side: number;
  frame: number | null;
  rawFrame: number | null;
  total: number;
  pulse: boolean;
}) {
  const style: React.CSSProperties =
    preview.kind === "ok" && frame !== null
      ? cropStyle(frame, side, preview)
      : {};
  const pulseStyle: React.CSSProperties = pulse
    ? { boxShadow: "0 0 0 2px var(--atlas-selected), 0 0 16px rgba(255,176,0,.6)" }
    : {};

  return (
    <div
      data-testid="atlas-hero"
      className="relative flex aspect-square w-full items-center justify-center overflow-hidden rounded border border-border bg-bg-2 text-center text-xs text-text-3"
      style={{ ...style, ...pulseStyle, transition: "box-shadow 250ms ease" }}
    >
      {frame !== null ? (
        <>
          <span className="absolute left-1.5 top-1.5 rounded bg-[var(--atlas-selected)] px-1.5 text-[11px] font-bold text-black">
            {frame}
          </span>
          <span className="absolute inset-x-0 bottom-0 bg-gradient-to-t from-black/80 to-transparent px-2 pb-1 pt-3 text-[13px] font-semibold text-white">
            Frame {frame} / {total}
          </span>
        </>
      ) : (
        <span>
          {rawFrame === null
            ? "Hover or select a frame"
            : `Frame ${rawFrame} — outside the ${side}×${side} atlas (in-game sampling is off-grid)`}
        </span>
      )}
    </div>
  );
}
