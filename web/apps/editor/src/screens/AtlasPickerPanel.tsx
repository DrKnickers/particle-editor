// AtlasPickerPanel — atlas frame grid + click-to-assign.
//
// Displays the texture atlas for the selected emitter's colorTexture as a
// side×side cell grid.  Hover previews a cell; the selection.frame field from
// AtlasContext highlights the currently-assigned frame with the amber token
// --atlas-selected.  Clicking a cell assigns the frame to all selected index
// keys (with a confirm dialog for differing values).  Preview fetches
// are mod-stack-keyed so a mod switch invalidates stale results.
//
// Placeholder precedence (top = highest priority):
//   1. no colorTexture           → "No color texture set."
//   2. atlas too large           → "Atlas too large to display (N×N)."
//   3. textureSize < 4 (side<2)  → "Single frame — no atlas to pick from."
//   4. preview missing           → "Texture not found."
//   5. preview broken            → "Texture could not be read."
//   6. focusedTrack !== "index"  → "Select keys on the index channel…"
//   happy path                  → grid + preview box

import { memo, useCallback, useEffect, useRef, useState } from "react";
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
  fitGridLayout,
} from "@/lib/atlas-grid";
import { useDockAnim } from "@/lib/dock-anim";
import { runWhenIdle } from "@/lib/run-after-paint";

// Smart-grid sizing constants (maximize available space): cell gap (matches
// the grid's `gap-1` = 4px) and the min/max square thumbnail size. The grid
// reflows RESPONSIVELY to the measured panel width (~√n columns capped so a cell
// never falls below GRID_MIN_CELL), so a wide dock shows more columns and a
// narrow one fewer — see fitGridLayout. The width is FROZEN during the dock
// slide (see the ResizeObserver's animating-guard below) so it lays out at the
// settled width, never the narrow mid-slide width.
const GRID_GAP = 4;
const GRID_MIN_CELL = 44;
const GRID_MAX_CELL = 160;
import { getPreviewCached, useTextureEpoch } from "@/lib/atlas-preview-cache";
import { useModStack } from "@/lib/mod-stack";
import { emitPerfTrace, makePerfSpanId } from "@/lib/perf-trace";

// Module-level cache of the last settled grid width. The panel UNMOUNTS when the
// dock closes, so component state is lost; persisting the width here lets a
// re-open render at its prior settled width immediately (no first-frame narrow
// transient before the ResizeObserver fires). null until the first real measure.
let lastAtlasGridW: number | null = null;

// First-ever-open default for the grid content width, used ONLY before any real
// measure exists (the cache above is null). It must equal the width the cold-start
// slide will SETTLE at, or the grid lays out at the wrong column count and snaps
// when the settle measure lands (the reported "first open reflows, fixed after"):
// a too-wide seed picks an extra column for a 64-cell atlas, then the settle drops
// it — a visible column snap. The cold-start width is DETERMINISTIC: on a fresh
// session the panel library has no remembered dock size, so the slide always
// expands the dock to its pixel floor (DOCK_MIN_PX = 260 in PanelLayout). Inside
// that 260px panel the usable grid width is 260 − ToolPanel left border (1) − this
// scroll region's p-3 (SCROLL_PAD = 24) − its two `scrollbar-gutter: both-edges`
// gutters (2 × the themed 10px scrollbar = 20) ≈ 215. (The ToolPanel body reserves
// NO gutter — AtlasPickerPanel passes bodyScroll={false} since it owns its scroll —
// so it doesn't shift the width.) Seeding 215 makes the first render pick the
// settle's column count (4 for a 64-cell atlas) at the settle's exact cell size, so
// there's no snap and no nudge; re-opens seed from the cached real width above —
// this only governs the first-ever open before any measure. Verified empirically
// (faithful DOM replica in real Chromium): min-dock settle = 215px, 4 cols, +1px
// of panel-centre.
const COLD_START_GRIDW = 215;

// Module-level cache of the last fetched emitter props (textureSize/colorTexture).
// The panel UNMOUNTS on dock close, so the first render of a RE-OPEN would
// otherwise show the loading placeholder until the emitters/get-properties
// round-trip resolves — and that grid mount lands DURING the dock-slide tween,
// contending with it. Seeding the initial state from this cache (when the id
// matches) renders the grid SYNCHRONOUSLY on the first frame, before the slide
// starts, so the tween runs uncontended. The fetch still runs to confirm/refresh.
// null until the first successful fetch.
let lastEmitterProps: { id: number; textureSize: number; colorTexture: string } | null = null;

/** Test-only: clear the module-level caches (seeded emitter props + the last
 *  settled grid width) so neither can leak across independent test cases — a
 *  width-mocking test would otherwise seed gridW (→ a different column count)
 *  into a later keyboard-nav test. */
export function __resetAtlasPropsCache(): void {
  lastEmitterProps = null;
  lastAtlasGridW = null;
}

// ─── types ───────────────────────────────────────────────────────────────────

type PreviewState =
  | { kind: "loading" }
  | { kind: "ok"; dataUri: string; srcW: number; srcH: number }
  | { kind: "missing" }
  | { kind: "broken" };

// ─── persisted "show texture alpha" preference ────────────────────────────────
// Mirrors the localStorage pref pattern (e.g. model-shadows.ts). Default OFF:
// additive frames (alpha ~0) are visible by default, which is the common need.

const SHOW_ALPHA_KEY = "atlas.showAlpha";

/** Read the persisted toggle; defaults to OFF (false) when absent/unreadable. */
function readShowAlpha(): boolean {
  try {
    return localStorage.getItem(SHOW_ALPHA_KEY) === "1";
  } catch {
    return false;
  }
}

/** Persist the toggle (stored as "1"/"0"); silent on private-mode/quota errors. */
function writeShowAlpha(on: boolean): void {
  try {
    localStorage.setItem(SHOW_ALPHA_KEY, on ? "1" : "0");
  } catch {
    /* private-mode / quota — in-memory UI state still reflects the choice */
  }
}

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

  // Seed from the module-level cache when it matches the CURRENT emitter, so a
  // RE-OPEN renders the grid synchronously on the first frame (before the dock
  // slide) instead of waiting on the get-properties round-trip. A FIRST open of
  // an emitter (no matching cache) falls back to the defaults and the grid
  // renders once the fetch resolves.
  const [textureSize, setTextureSize]   = useState(() =>
    lastEmitterProps && lastEmitterProps.id === emitterId ? lastEmitterProps.textureSize : 1);
  const [colorTexture, setColorTexture] = useState(() =>
    lastEmitterProps && lastEmitterProps.id === emitterId ? lastEmitterProps.colorTexture : "");
  // Both alpha modes are prefetched and held independently so flipping the Alpha
  // toggle is a synchronous swap — no per-toggle bridge round-trip and no loading
  // flash (that uncached host re-decode was the reported ~3s lag). The displayed
  // `preview` is derived from these two + showAlpha (below).
  const [flatPrev, setFlatPrev]         = useState<PreviewState>({ kind: "loading" }); // alpha OFF (color channel)
  const [rawPrev,  setRawPrev]          = useState<PreviewState>({ kind: "loading" }); // alpha ON  (real alpha)
  // Honor-alpha toggle. Default OFF: particle atlases are usually ADDITIVE (the
  // visible content lives in RGB while alpha ~0), so with normal blending those
  // frames look "missing". OFF forces every pixel fully opaque (alpha=255) so the
  // additive RGB shows; ON renders the texture's real alpha. Persisted like other
  // UI prefs.
  const [showAlpha, setShowAlpha]       = useState<boolean>(readShowAlpha);
  const [hover, setHover]               = useState<number | null>(null);
  const [focusIndex, setFocusIndex]     = useState<number | null>(null); // keyboard cursor
  const [confirmTarget, setConfirmTarget] = useState<{ frame: number; emitterId: number; keyTimes: number[] } | null>(null);
  const [announcement, setAnnouncement] = useState("");
  const [pulse, setPulse] = useState(false);
  // Measured content width the responsive grid sizes from. Seeded from the
  // module-level cache so a re-open lays out at the prior settled width on the
  // first frame; COLD_START_GRIDW (the deterministic dock-min content width) is
  // the first-ever-open default so even that first open picks the settle's column
  // count — no 5→4 snap (see COLD_START_GRIDW above).
  const [gridW, setGridW] = useState<number>(() => lastAtlasGridW ?? COLD_START_GRIDW);
  const gridRef = useRef<HTMLDivElement | null>(null);
  const focusInGridRef = useRef(false);   // does a grid cell currently hold focus?
  const restoreFocusRef = useRef(false);  // re-home focus after an atlas change?
  const roRef = useRef<ResizeObserver | null>(null);
  const lastMeasureRef = useRef<(() => void) | null>(null); // latest measure fn, called at slide settle
  const pulseTimer = useRef<number | undefined>(undefined);
  const decodedPreviewUrisRef = useRef<Set<string>>(new Set());
  const emitterIdRef = useRef<number | null>(emitterId);
  useEffect(() => { emitterIdRef.current = emitterId; }, [emitterId]);
  useEffect(() => { writeShowAlpha(showAlpha); }, [showAlpha]);
  useEffect(() => () => { if (pulseTimer.current) clearTimeout(pulseTimer.current); }, []);
  useEffect(() => () => roRef.current?.disconnect(), []);

  // Measure the scroll region's content width so the grid reflows to fill the
  // dock. A *callback ref* (not an effect) attaches the observer when the scroll
  // element actually mounts — the grid is conditionally rendered (only once the
  // preview loads), so an effect with [] deps would run before the element
  // exists and never re-attach. `clientWidth` includes the `p-3` padding (12px
  // each side) — subtract it. While a dock slide is in flight the measure NO-OPs
  // (holds the cached final width) so the grid never re-fits to the narrow
  // mid-slide width and snaps; it re-fits once at the settle (see the dock-anim
  // subscription below). jsdom lacks ResizeObserver (and reports clientWidth 0),
  // so the init `gridW` is kept there.
  const SCROLL_PAD = 24; // p-3 => 12px each side
  const setScrollEl = useCallback((el: HTMLDivElement | null) => {
    roRef.current?.disconnect();
    roRef.current = null;
    lastMeasureRef.current = null;
    if (!el) return;
    const measure = () => {
      // SLIDE: hold the cached final width; do not re-fit to the narrow
      // mid-slide width (that collapse-then-snap is the bug this guards).
      if (useDockAnim.getState().animating) return;
      const w = Math.max(0, el.clientWidth - SCROLL_PAD);
      if (w > 0) { setGridW(w); lastAtlasGridW = w; } // cache the settled width for the next open
    };
    lastMeasureRef.current = measure;
    measure(); // initial fit at attach (suppressed if mounting mid-slide → keeps the cached init)
    if (typeof ResizeObserver === "undefined") return; // jsdom — keep init gridW
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    roRef.current = ro;
  }, []);

  // Re-fit ONCE at the slide settle. During the slide the RO callbacks no-op
  // (the animating guard above), so the grid shows the cached final layout the
  // whole time — no single-column, no snap. On the animating true→false edge we
  // call the latest measure() so the grid lands at the real settled width.
  useEffect(() => {
    let wasAnimating = useDockAnim.getState().animating;
    return useDockAnim.subscribe((s) => {
      if (wasAnimating && !s.animating) lastMeasureRef.current?.();
      wasAnimating = s.animating;
    });
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
      const startMs = performance.now();
      const spanId = makePerfSpanId("atlas.fetch_props", emitterId);
      emitPerfTrace({
        eventName: "atlas.fetch_props",
        eventType: "span_start",
        spanId,
        emitterId,
        rendererStartMs: startMs,
      });
      void bridge
        .request({ kind: "emitters/get-properties", params: { id: emitterId } })
        .then((r) => {
          inFlight = false;
          const endMs = performance.now();
          emitPerfTrace({
            eventName: "atlas.fetch_props",
            eventType: "span_end",
            spanId,
            emitterId,
            rendererStartMs: startMs,
            rendererEndMs: endMs,
            durationMs: Math.max(0, endMs - startMs),
            status: "ok",
            textureSize: r.properties.textureSize,
            hasColorTexture: Boolean(r.properties.colorTexture),
          });
          if (!live) return;
          setTextureSize(r.properties.textureSize);
          setColorTexture(r.properties.colorTexture);
          // Cache for the next RE-OPEN's synchronous first-render seed (above).
          if (emitterId !== null)
            lastEmitterProps = { id: emitterId, textureSize: r.properties.textureSize, colorTexture: r.properties.colorTexture };
          if (again) { again = false; fetchProps(); } // trailing fetch for events that arrived mid-flight
        })
        .catch((err) => {
          inFlight = false;
          const endMs = performance.now();
          emitPerfTrace({
            eventName: "atlas.fetch_props",
            eventType: "span_end",
            spanId,
            emitterId,
            rendererStartMs: startMs,
            rendererEndMs: endMs,
            durationMs: Math.max(0, endMs - startMs),
            status: "error",
            error: err instanceof Error ? err.message : String(err),
          });
          /* leave defaults -> no-texture placeholder */
        });
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

  // Prefetch BOTH alpha modes whenever the texture/atlas (or mod-stack/epoch)
  // changes — NOT on the showAlpha toggle. Each mode paints into its own state as
  // soon as it resolves; the toggle then selects between two in-hand previews
  // (instant, no fetch). showAlpha is read here only to PRIORITISE the active
  // mode's fetch for first paint, and is intentionally absent from the deps so a
  // toggle never re-runs this effect (no ESLint here, so no exhaustive-deps note).
  useEffect(() => {
    if (!eligible || tooLarge || !colorTexture) {
      setFlatPrev({ kind: "loading" });
      setRawPrev({ kind: "loading" });
      return;
    }
    let live = true;
    setFlatPrev({ kind: "loading" });
    setRawPrev({ kind: "loading" });
    const load = (flattenAlpha: boolean, set: (p: PreviewState) => void) =>
      getPreviewCached(
        stack,
        // Fold the alpha mode into the cache key so flattened and raw previews of
        // the same texture don't collide.
        `${flattenAlpha ? "flat" : "raw"}::${colorTexture}`,
        () => {
          const mode = flattenAlpha ? "flat" : "raw";
          const startMs = performance.now();
          const spanId = makePerfSpanId("atlas.preview_fetch", mode, colorTexture);
          emitPerfTrace({
            eventName: "atlas.preview_fetch",
            eventType: "span_start",
            spanId,
            texture: colorTexture,
            previewMode: mode,
            rendererStartMs: startMs,
          });
          return bridge
            .request({ kind: "textures/get-preview", params: { filename: colorTexture, flattenAlpha } })
            .then((r) => {
              const endMs = performance.now();
              emitPerfTrace({
                eventName: "atlas.preview_fetch",
                eventType: "span_end",
                spanId,
                texture: colorTexture,
                previewMode: mode,
                rendererStartMs: startMs,
                rendererEndMs: endMs,
                durationMs: Math.max(0, endMs - startMs),
                status: r.status === "ok" ? "ok" : r.status,
                srcW: r.status === "ok" ? r.srcW : undefined,
                srcH: r.status === "ok" ? r.srcH : undefined,
              });
              return r;
            })
            .catch((err) => {
              const endMs = performance.now();
              emitPerfTrace({
                eventName: "atlas.preview_fetch",
                eventType: "span_end",
                spanId,
                texture: colorTexture,
                previewMode: mode,
                rendererStartMs: startMs,
                rendererEndMs: endMs,
                durationMs: Math.max(0, endMs - startMs),
                status: "error",
                error: err instanceof Error ? err.message : String(err),
              });
              throw err;
            });
        },
      )
        .then((r) => {
          if (!live) return;
          set(r.status === "ok"
            ? { kind: "ok", dataUri: r.dataUri, srcW: r.srcW, srcH: r.srcH }
            : { kind: r.status });
        })
        .catch((err) => {
          // Log the failing mode — the two modes resolve independently and only
          // the ACTIVE mode's status reaches the placeholder cascade, so an
          // inactive-mode failure is otherwise invisible until the user toggles
          // into it. (Mirrors assignAll's console.warn for partial failures.)
          console.warn(`[atlas] preview fetch failed (${flattenAlpha ? "color" : "alpha"} mode) for ${colorTexture}:`, err);
          if (live) set({ kind: "broken" });
        });
    // Active mode first (synchronous) so first paint isn't gated behind the other
    // mode's decode. The INACTIVE mode is deferred to the first idle slot after
    // first paint so it doesn't compete with the active mode's decode (perf-audit
    // P1b). showAlpha is NOT in this effect's deps, so a toggle never re-runs it —
    // the inactive load runs exactly once (from the idle callback), so there's no
    // duplicate fetch; a toggle before idle fires briefly shows the loading
    // placeholder (bounded by the idle timeout).
    let cancelInactive = () => {};
    if (showAlpha) {
      void load(false, setRawPrev);                                          // active: raw
      cancelInactive = runWhenIdle(() => { void load(true, setFlatPrev); });  // inactive: flat
    } else {
      void load(true, setFlatPrev);                                          // active: flat
      cancelInactive = runWhenIdle(() => { void load(false, setRawPrev); });  // inactive: raw
    }
    return () => { live = false; cancelInactive(); };
  }, [bridge, colorTexture, eligible, stack, tooLarge, textureEpoch]);

  // The displayed preview is the active mode's prefetched result — a synchronous
  // pick, so toggling Alpha is instant (no bridge round-trip, no loading flash).
  const preview: PreviewState = showAlpha ? rawPrev : flatPrev;

  // Warm the browser's image DECODE cache for BOTH modes as soon as their data
  // URIs load. The data is already prefetched, but the browser only decodes a
  // PNG the first time it's painted — so the very first Alpha toggle would pay a
  // one-off decode for the not-yet-shown mode. Decoding both off-screen up front
  // makes that first toggle instant. Best-effort (ignored if decode is absent).
  useEffect(() => {
    for (const p of [flatPrev, rawPrev]) {
      if (p.kind === "ok") {
        if (decodedPreviewUrisRef.current.has(p.dataUri)) continue;
        decodedPreviewUrisRef.current.add(p.dataUri);
        const img = new Image();
        img.src = p.dataUri;
        const decodePromise = img.decode?.();
        if (!decodePromise) continue;
        const startMs = performance.now();
        const spanId = makePerfSpanId("atlas.browser_decode");
        emitPerfTrace({
          eventName: "atlas.browser_decode",
          eventType: "span_start",
          spanId,
          rendererStartMs: startMs,
        });
        void decodePromise
          .then(() => {
            const endMs = performance.now();
            emitPerfTrace({
              eventName: "atlas.browser_decode",
              eventType: "span_end",
              spanId,
              rendererStartMs: startMs,
              rendererEndMs: endMs,
              durationMs: Math.max(0, endMs - startMs),
              status: "ok",
            });
          })
          .catch((err) => {
            const endMs = performance.now();
            emitPerfTrace({
              eventName: "atlas.browser_decode",
              eventType: "span_end",
              spanId,
              rendererStartMs: startMs,
              rendererEndMs: endMs,
              durationMs: Math.max(0, endMs - startMs),
              status: "error",
              error: err instanceof Error ? err.message : String(err),
            });
          });
      }
    }
  }, [flatPrev, rawPrev]);

  // ── stale-index reset ─────────────────────────────────────────────
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

  // ── cold-start slide readiness ─────────────────────────────────────────────
  // gridMounted: whether the cell grid is actually mounted — the EXACT condition
  // under which the <div role="listbox"> + cells render below (okPreview truthy ⇒
  // preview.kind === "ok" ⇒ colorTexture present). On a COLD first open the grid
  // mounts only after the async get-properties + preview round-trips resolve. This
  // feeds telemetry (atlas.grid_mounted) and the dock-anim grid signal; the dock
  // OPEN slide itself now gates on terminalFirstPaint (below), NOT this, so a
  // placeholder/error open slides promptly. Both signals are cleared on unmount so
  // the next open starts from a clean "not ready" state and re-waits.
  const gridMounted = preview.kind === "ok" && eligible && !tooLarge && !offIndex;
  // Terminal first paint: ANY non-loading first-paint outcome — the grid OR a
  // placeholder. The prefetch effect's early-return (!colorTexture/tooLarge/
  // !eligible) leaves preview.kind === "loading", and offIndex shows a non-grid
  // state too, so those placeholder cases are folded in explicitly. The dock gate
  // releases on THIS so a placeholder/error open no longer waits for a full grid
  // mount (perf-audit P1b).
  const terminalFirstPaint =
    !colorTexture || tooLarge || !eligible || offIndex || preview.kind !== "loading";
  useEffect(() => {
    if (gridMounted) {
      emitPerfTrace({
        eventName: "atlas.grid_mounted",
        eventType: "instant",
        emitterId,
        texture: colorTexture,
        textureSize,
        side,
      });
    }
    useDockAnim.getState().setAtlasGridMounted(gridMounted);
  }, [colorTexture, emitterId, gridMounted, side, textureSize]);
  useEffect(() => {
    useDockAnim.getState().setAtlasTerminalFirstPaint(terminalFirstPaint);
  }, [terminalFirstPaint]);
  useEffect(() => () => {
    useDockAnim.getState().setAtlasGridMounted(false);
    useDockAnim.getState().setAtlasTerminalFirstPaint(false);
  }, []);

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
  // emitter switch mid-await. On a FAILED commit (a set-track-key write was
  // rejected, or there was no resolvable target to write to), announce the failure
  // too — otherwise a keyboard / screen-reader user gets silence where a sighted user
  // at least sees no amber pulse. No toast infra in this project, so the aria-live
  // region is the channel (parity with the success path). Both branches bail if the
  // emitter switched mid-await.
  async function commitAssign(frameF: number, targetEmitterId: number | null, targetKeyTimes: number[]) {
    const started = targetEmitterId;
    const ok = await assignAll(frameF, targetEmitterId, targetKeyTimes);
    if (emitterIdRef.current !== started) return; // emitter switched mid-await — drop
    if (ok) {
      setFocusIndex(frameF);
      firePulse();
      setAnnouncement(`Assigned frame ${frameF}`);
    } else {
      setAnnouncement(`Could not assign frame ${frameF}`);
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
  // STABLE click handler passed to cells so React.memo(Cell) can skip ALL cells
  // on an alpha toggle (which re-renders only the grid container). A "latest
  // ref" keeps the wrapper identity fixed while always invoking the current
  // onCellClick (no stale closure over frame/keyTimes/emitterId). `setHover`
  // (onHover) is already stable, so with this both cell callbacks are stable.
  const onCellClickRef = useRef(onCellClick);
  onCellClickRef.current = onCellClick;
  const stableCellClick = useRef((k: number) => onCellClickRef.current(k)).current;

  const totalCells   = side * side;
  const fc           = frameCount(textureSize);
  // Show "M of N" only when the atlas has unused cells (frameCount < textureSize).
  const meta         = totalCells === Math.max(1, Math.floor(Number.isFinite(textureSize) ? textureSize : 1))
    ? `${side}×${side} · ${fc}`
    : `${side}×${side} · ${fc} of ${textureSize}`;
  const highlight    = frame === null ? null : resolveFrame(frame, side);
  // Clamp the keyboard cursor against the live grid (backstop for the stale-index reset).
  const safeFocus    = focusIndex !== null && focusIndex < totalCells ? focusIndex : null;
  // Clamp hover too, so a stale hover index (e.g. mid texture-swap, before the
  // reset effect clears it) can never feed cropStyle an out-of-range frame.
  const safeHover    = hover !== null && hover < totalCells ? hover : null;
  // The single tabbable cell: cursor, else the assigned frame, else 0 (never null).
  const rovingTarget = safeFocus ?? highlight ?? 0;
  const previewFrame = safeHover ?? safeFocus ?? highlight; // precedence (?? — frame 0 valid)
  // Responsive grid sizing: ~sqrt(n) columns filling the measured
  // (and slide-frozen) width, with a min-cell floor so large atlases stay dense.
  const layout = fitGridLayout(totalCells, gridW, GRID_GAP, GRID_MIN_CELL, GRID_MAX_CELL);

  // ── keyboard navigation ─────────────────────────────────────────────

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

  function onGridKeyDown(e: React.KeyboardEvent<HTMLDivElement>) {
    if (offIndex) return;
    const cur = rovingTarget;
    // The grid reflows responsively, so up/down must move by the LIVE column
    // count — the same `layout.cols` the grid renders with (its fixed
    // `${layout.cell}px` tracks are exactly this many columns).
    const cols = layout.cols;
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
    // The hero renders ONE frame via a <canvas> crop (no giant CSS raster). The
    // cell grid reflows responsively to the measured panel width (more columns
    // when wide, fewer when narrow). During the dock slide the measure is frozen
    // at the cached FINAL width and re-fit once at settle, so there's no
    // single-column transient and no settle-snap.
    //
    // The active mode's atlas image lives on the GRID CONTAINER (one element),
    // exposed via the --atlas-url custom property the cells reference. Toggling
    // alpha re-renders ONLY this container — the N cells' props are unchanged, so
    // React.memo skips them and the browser just re-resolves --atlas-url and
    // repaints the shared cell raster once.
    const okPreview = preview.kind === "ok" ? preview : null;
    // The container carries the SAME gray as the cells (bg-bg-2). Its gap-1 gaps
    // therefore match the cells, and in alpha mode the transparent frame pixels
    // reveal that uniform gray. bg-bg-2 is a theme token, so the gray adapts to
    // dark/light automatically — no per-mode backing, no checkerboard.
    body = (
      <>
        {/* Pinned preview box. Its bg-bg-2 backing shows through the canvas's
            transparent (non-frame) pixels, matching the cells' uniform gray. */}
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
        {/* Scrollable cell grid. The grid reflows responsively to this region's
            measured width (via the callback-ref ResizeObserver). During a dock
            slide the measure no-ops and the grid holds its cached FINAL width,
            re-fitting once at the settle — so it never collapses to a single
            column mid-slide and snaps. `scrollbar-gutter: stable BOTH-EDGES`
            reserves the scrollbar space SYMMETRICALLY (a matching empty strip on
            the left mirrors the scrollbar on the right). Two reasons: (1) the
            width stays constant whether or not the vertical scrollbar is showing,
            so the RO never re-fit-loops (grid grows → scrollbar → narrower →
            smaller grid → no scrollbar → wider → repeat = flicker); (2) the
            symmetric reservation centres the grid in the panel — a one-sided
            `stable` gutter pushes the centred tracks ~half a gutter off-centre
            (and a full gutter when no scrollbar shows), visibly misaligning the
            grid from the full-width hero above it (measured: −7px vs 0px). */}
        <div
          ref={setScrollEl}
          data-testid="atlas-scroll"
          className="atlas-grid-scroll min-h-0 flex-1 overflow-y-auto p-3"
          style={{ scrollbarGutter: "stable both-edges" }}
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
            className="mx-auto grid justify-center gap-1 bg-bg-2"
            // Responsive column count (`layout.cols`, ~√n capped to the measured
            // width — frozen during the dock slide). Cells are FIXED-px
            // (layout.cell, from the same frozen fit), so the grid is a STATIC
            // block: during the slide its size doesn't change, and the dock
            // panel's overflow:hidden simply CLIPS it as it widens (revealing it)
            // instead of 1fr cells resizing/reflowing live. max-width caps the
            // grid to GRID_MAX_CELL per column so small atlases get big,
            // space-filling cells. The fixed-px column tracks would otherwise
            // left-pack inside the full-width grid box, so `justify-center`
            // centres the tracks horizontally and `mx-auto` centres the box once
            // the max-width cap makes it narrower than the region. The active
            // atlas image is exposed as --atlas-url for the cells; the container's
            // own bg-bg-2 (matching the cells) fills the gaps in both modes.
            style={{
              gridTemplateColumns: `repeat(${layout.cols}, ${layout.cell}px)`,
              maxWidth: `${layout.cols * GRID_MAX_CELL + (layout.cols - 1) * GRID_GAP}px`,
              ...(okPreview
                ? ({ ["--atlas-url"]: `url(${okPreview.dataUri})` } as React.CSSProperties)
                : {}),
            }}
          >
            {okPreview &&
              Array.from({ length: totalCells }, (_, k) => (
                <Cell
                  key={k}
                  k={k}
                  side={side}
                  srcW={okPreview.srcW}
                  srcH={okPreview.srcH}
                  selected={k === highlight}
                  focused={k === rovingTarget}
                  onHover={setHover}
                  onClick={stableCellClick}
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
    <ToolPanel title="Atlas Frames" onClose={onClose} variant="docked" closing={closing} bodyScroll={false}>
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
            <button
              type="button"
              data-testid="atlas-alpha-toggle"
              aria-pressed={showAlpha}
              title="Show texture alpha (off shows additive RGB)"
              onClick={() => setShowAlpha((v) => !v)}
              className={`shrink-0 rounded border px-1 transition-colors focus-ring ${
                showAlpha
                  ? "border-[var(--accent)] bg-[var(--accent)] text-black hover:bg-[var(--accent-2)]"
                  : "border-border text-text-3 hover:bg-hover hover:text-text"
              }`}
            >
              Alpha
            </button>
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

/** Compute the cell's CSS background-* crop for frame k.
 *
 * The atlas image is NOT embedded here — it lives on the grid container as the
 * --atlas-url custom property (one image element shared by every cell). The cell
 * only positions that shared raster to its frame, with NO background-color of its
 * own: the uniform gray backing (bg-bg-2) is on the cell element + the container.
 * In color mode the opaque flat crop covers it; in alpha mode the crop's
 * transparent frame pixels reveal the gray. Because srcW/srcH are identical
 * across both alpha modes (same texture dims), this style is STABLE across an
 * alpha toggle — the cell never re-renders on toggle. */
function cropStyle(
  k: number,
  side: number,
  srcW: number,
  srcH: number,
): React.CSSProperties {
  const r = cellRect(k, side, srcW, srcH);
  // backgroundSize: the full image is `side` cells wide/tall.
  // backgroundPosition: position the relevant cell into view.
  const posX = side > 1 ? `${(r.left / (srcW - r.width)) * 100}%` : "0%";
  const posY = side > 1 ? `${(r.top / (srcH - r.height)) * 100}%` : "0%";
  return {
    backgroundImage: "var(--atlas-url)",
    backgroundRepeat: "no-repeat",
    backgroundSize: `${side * 100}% ${side * 100}%`,
    backgroundPosition: `${posX} ${posY}`,
  };
}

// React.memo so a grid-container re-render on the alpha toggle (which only swaps
// --atlas-url and the container backing) SKIPS every cell — the cell's props
// (k, side, srcW, srcH, selected, focused) don't change across a toggle, so the
// browser just re-resolves var(--atlas-url) and repaints the shared raster once.
const Cell = memo(function Cell({
  k,
  side,
  srcW,
  srcH,
  selected,
  focused,
  onHover,
  onClick,
}: {
  k: number;
  side: number;
  srcW: number;
  srcH: number;
  selected: boolean;
  focused: boolean;
  onHover: (k: number | null) => void;
  onClick?: (k: number) => void;
}) {
  const style: React.CSSProperties = cropStyle(k, side, srcW, srcH);
  if (selected) {
    // amber via border + box-shadow ring so the blue focus OUTLINE can nest
    style.borderColor = "var(--atlas-selected)";
    style.boxShadow = "0 0 0 1px var(--atlas-selected), 0 0 12px color-mix(in srgb, var(--atlas-selected) 40%, transparent)";
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
      className={`group relative aspect-square rounded-[var(--radius-sm)] bg-bg-2 transition hover:scale-[1.06] hover:z-10 motion-reduce:hover:scale-100 ${
        selected ? "border-2" : "border border-border"
      } focus-ring`}
      style={style}
      onMouseEnter={() => onHover(k)}
      onMouseLeave={() => onHover(null)}
      onClick={() => onClick?.(k)}
    >
      {/* Hover affordance = a subtle scale-lift (on the cell) + this tint
          overlay (~22%). Geometry carries it on loud thumbnails and stacks cleanly
          with the amber selection ring + blue focus outline (those are colour
          cues; the lift scales the whole cell, rings and all). No outset shadow —
          the grid scroll container would clip it. */}
      <span
        aria-hidden
        className="pointer-events-none absolute inset-0 rounded-[inherit] bg-[var(--overlay-hover)] opacity-0 transition-opacity group-hover:opacity-100 motion-reduce:transition-none"
      />
      {/* Frame-index badge: ALWAYS visible so each thumbnail names its atlas index
          at a glance. Amber pill when assigned (matches the selection ring); a
          FIXED translucent scrim otherwise — it sits over arbitrary sprite imagery,
          so it must not theme-flip (dark scrim + light text reads on both themes). */}
      <span
        data-testid="atlas-cell-badge"
        className={`pointer-events-none absolute bottom-0.5 left-0.5 rounded-sm px-1 text-[9px] leading-tight ${
          selected
            ? "bg-[var(--atlas-selected)] font-semibold text-black"
            : "bg-[var(--overlay-scrim)] text-[var(--overlay-scrim-fg)]"
        }`}
      >
        {k}
      </span>
    </div>
  );
});

/** Paint frame `frame`'s crop of the atlas image onto the hero canvas at its
 *  display size — drawing ONLY the one frame (vs the old CSS background that
 *  rasterized the whole atlas upscaled to `side × hero`). The canvas is left
 *  TRANSPARENT where the frame isn't drawn (clearRect, no backing fill), so the
 *  hero element's own bg-bg-2 shows through transparent frame pixels — matching
 *  the cells' uniform gray in both modes (no checkerboard).
 *  No-ops gracefully if there's no 2d context (jsdom) or no image yet. */
function drawHero(
  canvas: HTMLCanvasElement,
  img: HTMLImageElement,
  frame: number,
  side: number,
) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return; // jsdom has no real 2d context — no-op
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const cssW = canvas.clientWidth || canvas.width;
  const cssH = canvas.clientHeight || canvas.height;
  const w = Math.max(1, Math.round(cssW * dpr));
  const h = Math.max(1, Math.round(cssH * dpr));
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  ctx.clearRect(0, 0, w, h); // transparent → reveals the hero element's bg-bg-2
  // Source rect = frame's cell in the atlas (same maths as the CSS crop).
  const r = cellRect(frame, side, img.naturalWidth, img.naturalHeight);
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(img, r.left, r.top, r.width, r.height, 0, 0, w, h);
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
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const imgRef = useRef<HTMLImageElement | null>(null);
  const imgUriRef = useRef<string | null>(null); // dataUri the loaded img belongs to
  const [imgReady, setImgReady] = useState(false);

  const dataUri = preview.kind === "ok" ? preview.dataUri : null;

  // Load the active preview's dataUri into an HTMLImageElement once per dataUri
  // (cached via imgUriRef so the same atlas isn't reloaded on a frame / alpha
  // toggle). decode() resolves when the bitmap is ready to draw.
  useEffect(() => {
    if (!dataUri) { imgRef.current = null; imgUriRef.current = null; setImgReady(false); return; }
    if (imgUriRef.current === dataUri && imgRef.current) { setImgReady(true); return; }
    let live = true;
    setImgReady(false);
    const img = new Image();
    img.src = dataUri;
    const ready = () => { if (!live) return; imgRef.current = img; imgUriRef.current = dataUri; setImgReady(true); };
    if (typeof img.decode === "function") {
      img.decode().then(ready).catch(() => { /* fall back to onload below */ });
    }
    img.onload = ready;
    return () => { live = false; };
  }, [dataUri]);

  // (Re)draw when the image is ready, the frame, or the preview changes. (An
  // alpha toggle swaps the active preview's dataUri, which re-runs the img-load
  // effect → imgReady toggles → this redraws — so no showAlpha dep is needed.)
  // Guarded inside drawHero against a missing 2d context (jsdom).
  useEffect(() => {
    const canvas = canvasRef.current;
    const img = imgRef.current;
    if (!canvas || !img || !imgReady) return;
    if (preview.kind !== "ok" || frame === null) return;
    drawHero(canvas, img, frame, side);
  }, [imgReady, frame, side, preview.kind]);

  const pulseStyle: React.CSSProperties = pulse
    ? { boxShadow: "0 0 0 2px var(--atlas-selected), 0 0 16px color-mix(in srgb, var(--atlas-selected) 60%, transparent)" }
    : {};

  const showCanvas = preview.kind === "ok" && frame !== null;

  return (
    <div
      data-testid="atlas-hero"
      className="relative flex aspect-square w-full items-center justify-center overflow-hidden rounded border border-border bg-bg-2 text-center text-xs text-text-3"
      style={{ ...pulseStyle, transition: "box-shadow var(--motion-slow-in) var(--ease-entrance)" }}
    >
      {showCanvas && (
        <canvas
          ref={canvasRef}
          className="absolute inset-0"
          style={{ width: "100%", height: "100%" }}
        />
      )}
      {frame !== null ? (
        <>
          <span className="absolute left-1.5 top-1.5 rounded bg-[var(--atlas-selected)] px-1.5 text-[11px] font-semibold text-black">
            {frame}
          </span>
          <span className="absolute inset-x-0 bottom-0 bg-gradient-to-t from-black/80 to-transparent px-2 pb-1 pt-3 text-xs font-semibold text-white">
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
