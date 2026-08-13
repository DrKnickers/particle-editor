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

import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useAtlasContext } from "@/lib/atlas-context";
import { ToolPanel } from "@/components/ToolPanel";
import { AtlasConfirmModal } from "@/components/AtlasConfirmModal";
import {
  gridSide,
  frameCount,
  isAtlasTooLarge,
  resolveFrame,
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
const GRID_MIN_CELL = 44;
const GRID_MAX_CELL = 160;
// Stable empty dead-cell set so the "no dead cells" state never churns identity (React
// bails the re-render when setDeadCells is handed the same reference).
const EMPTY_DEAD_CELLS: ReadonlySet<number> = new Set();
import { getPreviewCached, useTextureEpoch } from "@/lib/atlas-preview-cache";
import { computeDeadCells } from "@/lib/atlas-dead-cells";
import { useModStack } from "@/lib/mod-stack";
import { emitPerfTrace, makePerfSpanId } from "@/lib/perf-trace";
import { requestTreeRefetch } from "@/lib/tree-refetch";
import { AtlasFrameGrid } from "./atlas/AtlasFrameGrid";
import { drawHero, GRID_GAP } from "./atlas/atlas-canvas";
import { useDecodedImage } from "./atlas/useDecodedImage";

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
let lastEmitterProps: { id: number; textureSize: number; colorTexture: string; blendAlphaGated: boolean } | null = null;

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
  const stack         = useModStack((s) => s.stack);
  const stackKey      = stack.join("|");
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
  // Both alpha modes are prefetched and held independently so a blend-mode change
  // is a synchronous swap — no per-change bridge round-trip and no loading flash
  // (that uncached host re-decode was the reported ~3s lag). The displayed
  // `preview` is derived from these two + blendAlphaGated (below).
  const [flatPrev, setFlatPrev]         = useState<PreviewState>({ kind: "loading" }); // alpha OFF (color channel)
  const [rawPrev,  setRawPrev]          = useState<PreviewState>({ kind: "loading" }); // alpha ON  (real alpha)
  // "Effectively empty" frames (max alpha ≈ 0) — they render invisible particles yet preview
  // fine (the picker shows RGB), so the grid dims + disables them. Sampled from rawPrev (the
  // alpha-mode preview, which carries real transparency). ADVISORY + async: empty until the
  // sample resolves, so a frame picked during the load window is still honored — the guards
  // in onCellClick / the confirm modal re-check this set at commit time.
  const [deadCells, setDeadCells]       = useState<ReadonlySet<number>>(EMPTY_DEAD_CELLS);
  // Whether the emitter's blend mode gates visible output on alpha — host-derived
  // (ParticleSystem::blendModeIsAlphaGated), shipped in the emitter-properties DTO.
  // Drives BOTH the displayed preview (alpha vs flat) and dead-cell dimming. Seeded
  // from the module cache so a re-open renders correctly on the first frame.
  const [blendAlphaGated, setBlendAlphaGated] = useState<boolean>(() =>
    lastEmitterProps && lastEmitterProps.id === emitterId ? lastEmitterProps.blendAlphaGated : false);
  const hoverRef                        = useRef<number | null>(null);
  const hoverPreviewRedrawRef             = useRef<(() => void) | null>(null);
  const hoverPreviewRafRef                = useRef<number | null>(null);
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
  // [#572] The grid is ONE <canvas> painted as a static tall image, so scrolling
  // needs NO re-render/redraw — we only keep a handle to the scroll container so
  // keyboard nav can scroll the roving cell into view (adjust scrollTop).
  const scrollElRef = useRef<HTMLDivElement | null>(null);
  const gridRef = useRef<HTMLCanvasElement | null>(null); // the listbox <canvas>
  const focusInGridRef = useRef(false);   // does the grid canvas currently hold focus?
  const restoreFocusRef = useRef(false);  // re-home focus after an atlas change?
  const kbFocusPendingRef = useRef<number | null>(null); // [#572] cell to scroll into view after a keyboard move
  const roRef = useRef<ResizeObserver | null>(null);
  const lastMeasureRef = useRef<(() => void) | null>(null); // latest measure fn, called at slide settle
  const pulseTimer = useRef<number | undefined>(undefined);
  const decodedPreviewUrisRef = useRef<Set<string>>(new Set());
  const bridgeRef = useRef(bridge);
  const stackRef = useRef(stack);
  const emitterIdRef = useRef<number | null>(emitterId);
  const scheduleHoverPreviewRedraw = useCallback(() => {
    if (hoverPreviewRafRef.current !== null) return;
    hoverPreviewRafRef.current = window.requestAnimationFrame(() => {
      hoverPreviewRafRef.current = null;
      hoverPreviewRedrawRef.current?.();
    });
  }, []);
  const registerHoverPreviewRedraw = useCallback((redraw: (() => void) | null) => {
    hoverPreviewRedrawRef.current = redraw;
  }, []);
  bridgeRef.current = bridge;
  stackRef.current = stack;
  useEffect(() => { emitterIdRef.current = emitterId; }, [emitterId]);
  useEffect(() => () => {
    if (hoverPreviewRafRef.current !== null) window.cancelAnimationFrame(hoverPreviewRafRef.current);
  }, []);
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
    scrollElRef.current = el;
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
    const fetchProps = (coalesceTreeRefetch = false) => {
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
      const req = { kind: "emitters/get-properties", params: { id: emitterId } } as const;
      void (coalesceTreeRefetch ? requestTreeRefetch(bridge, req) : bridge.request(req))
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
          setBlendAlphaGated(r.properties.blendAlphaGated);
          // Cache for the next RE-OPEN's synchronous first-render seed (above).
          if (emitterId !== null)
            lastEmitterProps = { id: emitterId, textureSize: r.properties.textureSize, colorTexture: r.properties.colorTexture, blendAlphaGated: r.properties.blendAlphaGated };
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
    const off = bridge.on("emitters/tree/changed", () => fetchProps(true));
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
  // changes — NOT on a blend-mode change. Each mode paints into its own state as
  // soon as it resolves; blendAlphaGated then selects between two in-hand previews
  // (instant, no fetch). blendAlphaGated is read here only to PRIORITISE the active
  // mode's fetch for first paint, and is intentionally absent from the deps so a
  // blend-mode change never re-runs this effect (no ESLint here, so no exhaustive-deps note).
  const previewInputKey = eligible && !tooLarge && colorTexture
    ? `${emitterId ?? "none"}::${textureEpoch}::${stackKey}::${colorTexture}`
    : null;

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
        stackRef.current,
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
          return bridgeRef.current
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
        { bridge: bridgeRef.current, filename: colorTexture, flattenAlpha },
      )
        .then((r) => {
          if (!live) return;
          set(r.status === "ok"
            ? { kind: "ok", dataUri: r.dataUri, srcW: r.srcW, srcH: r.srcH }
            : r.status === "pending"
              ? { kind: "loading" }
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
    // P1b). blendAlphaGated is NOT in this effect's deps: both modes load either
    // way (active sync, inactive on idle), so priority only affects first-paint
    // latency, never correctness. blendAlphaGated is set in the same batch as
    // colorTexture on the get-properties fetch, so this effect re-runs with the
    // fresh value; a pure blend-mode edit leaves colorTexture unchanged (both
    // previews already cached), so the preview flips with no re-fetch.
    let cancelInactive = () => {};
    if (blendAlphaGated) {
      void load(false, setRawPrev);                                          // active: raw
      cancelInactive = runWhenIdle(() => { void load(true, setFlatPrev); });  // inactive: flat
    } else {
      void load(true, setFlatPrev);                                          // active: flat
      cancelInactive = runWhenIdle(() => { void load(false, setRawPrev); });  // inactive: raw
    }
    return () => { live = false; cancelInactive(); };
  }, [colorTexture, eligible, previewInputKey, tooLarge]);

  // Dead-cell detection: once the alpha-mode preview (rawPrev) resolves, sample it off an
  // offscreen canvas and flag effectively-empty frames so the grid can dim + disable them.
  // Keyed on the raw preview's data URI + side, so it re-runs on any texture/mod/epoch change
  // and resets to empty while (re)loading. computeDeadCells is fail-safe — an empty set on any
  // decode/canvas failure (incl. jsdom), so detection simply doesn't dim anything on failure.
  // GATED on blendAlphaGated: only alpha-gated modes render nothing for an alpha-0 frame;
  // additive/inverse/none modes show the RGB, so an "empty-alpha" frame is NOT dead there.
  const rawUri = rawPrev.kind === "ok" ? rawPrev.dataUri : null;
  useEffect(() => {
    if (!blendAlphaGated || !rawUri || side < 2) { setDeadCells(EMPTY_DEAD_CELLS); return; }
    let live = true;
    void computeDeadCells(rawUri, side).then((d) => {
      if (live) setDeadCells(d.size ? d : EMPTY_DEAD_CELLS);
    });
    return () => { live = false; };
  }, [blendAlphaGated, rawUri, side]);

  // The displayed preview follows the emitter's blend mode: alpha-gated modes show
  // the real-alpha preview (rawPrev), others the flat additive preview (flatPrev).
  // Both are prefetched, so the switch on a blend-mode change is a synchronous pick
  // (no bridge round-trip, no loading flash).
  const preview: PreviewState = blendAlphaGated ? rawPrev : flatPrev;

  // [#572] The GRID is ONE <canvas> that draws each frame with a single
  // drawImage from the full-res `preview` image (one paint, no per-cell DOM and
  // no giant CSS data-URI). So the old downscale/`--atlas-url` machinery is gone
  // — the canvas grid takes the full-res preview directly (null while loading).
  const okPreview = preview.kind === "ok" ? preview : null;

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
    hoverRef.current = null;
    scheduleHoverPreviewRedraw();
  }, [emitterId, colorTexture, scheduleHoverPreviewRedraw, side]);

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
    // Effectively-empty frame: block assignment (mouse AND keyboard route through here) and
    // announce, so a keyboard/screen-reader user isn't left with a silent no-op.
    if (deadCells.has(k)) { setAnnouncement(`Frame ${k} is empty — nothing to assign`); return; }
    if (frame === null && keyTimes.length > 1) {
      if (emitterId !== null) setConfirmTarget({ frame: k, emitterId, keyTimes });
      return;
    }
    void commitAssign(k, emitterId, keyTimes);
  }
  // STABLE click handler passed to the grid so React.memo(AtlasFrameGrid) can
  // skip re-rendering on an alpha toggle. A "latest ref" keeps the wrapper
  // identity fixed while always invoking the current onCellClick (no stale
  // closure over frame/keyTimes/emitterId).
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
  // The single tabbable cell: cursor, else the assigned frame, else 0 (never null).
  const rovingTarget = safeFocus ?? highlight ?? 0;
  const previewFrame = safeFocus ?? highlight; // hover is drawn imperatively from hoverRef
  const setHoverPreview = useCallback((next: number | null) => {
    const safeNext = next !== null && next >= 0 && next < totalCells ? next : null;
    if (hoverRef.current === safeNext) return;
    hoverRef.current = safeNext;
    scheduleHoverPreviewRedraw();
  }, [scheduleHoverPreviewRedraw, totalCells]);
  useEffect(() => {
    if (hoverRef.current !== null && hoverRef.current >= totalCells) {
      hoverRef.current = null;
      scheduleHoverPreviewRedraw();
    }
  }, [scheduleHoverPreviewRedraw, totalCells]);
  // Responsive grid sizing: ~sqrt(n) columns filling the measured
  // (and slide-frozen) width, with a min-cell floor so large atlases stay dense.
  const layout = fitGridLayout(totalCells, gridW, GRID_GAP, GRID_MIN_CELL, GRID_MAX_CELL);

  // ── keyboard navigation ─────────────────────────────────────────────

  // [#572] The grid is a single <canvas> listbox, so keyboard focus stays on
  // that ONE element (the aria-activedescendant moves, not DOM focus). "Focusing"
  // a cell therefore means: keep focus on the canvas and scroll the cell's row
  // into view by nudging the scroll container's scrollTop (canvas doesn't reflow,
  // so there's no per-cell element to scrollIntoView). jsdom-tolerant: all reads
  // are 0 there and the scrollTop write is a harmless no-op.
  function scrollFrameIntoView(k: number) {
    const scroller = scrollElRef.current;
    const canvas = gridRef.current;
    if (!scroller || !canvas) return;
    const cols = Math.max(1, layout.cols);
    const step = layout.cell + GRID_GAP;
    const rowTop = Math.floor(k / cols) * step;
    // Cell top in the scroller's CONTENT space. canvas.offsetTop is 0 (its
    // offsetParent is the position:relative grid-box), so it misses the
    // scroller's p-3 (12px) padding above the box; measure the canvas top
    // relative to the scroller instead — otherwise Down/End nav under-scrolls
    // by that padding and leaves the active cell clipped. (jsdom returns 0 for
    // all three terms → top = rowTop, unchanged there.)
    const canvasTopInContent =
      canvas.getBoundingClientRect().top - scroller.getBoundingClientRect().top + scroller.scrollTop;
    const top = canvasTopInContent + rowTop;
    const bottom = top + layout.cell;
    const viewTop = scroller.scrollTop;
    const viewBottom = viewTop + scroller.clientHeight;
    if (top < viewTop) scroller.scrollTop = top;
    else if (bottom > viewBottom) scroller.scrollTop = bottom - scroller.clientHeight;
  }
  function focusCell(k: number) {
    gridRef.current?.focus?.();
    scrollFrameIntoView(k);
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
    // [#572] `next` may be scrolled off-screen (e.g. Home/End jumps). Record it
    // and let the layout effect below scroll it into view after the re-render
    // updates rovingTarget (→ aria-activedescendant + the redrawn focus outline).
    kbFocusPendingRef.current = next;
    setFocusIndex(next);
  }

  // [#572] Scroll the pending keyboard target into view once the re-render has
  // moved rovingTarget. Runs before paint (useLayoutEffect) so the scroll is not
  // visibly deferred. Only fires for keyboard-driven moves (kbFocusPendingRef set
  // by moveTo), never on hover/selection-driven rovingTarget changes.
  useLayoutEffect(() => {
    const pending = kbFocusPendingRef.current;
    if (pending === null) return;
    kbFocusPendingRef.current = null;
    focusCell(pending);
  }, [rovingTarget]);

  function onGridKeyDown(e: React.KeyboardEvent<HTMLElement>) {
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
    // Teaching empty state (design pass, D2): name the fix, not just the fact.
    body = <Placeholder>No color texture set. Choose one under Appearance → Textures.</Placeholder>;
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
    // cell grid is ONE <canvas> that draws every frame in a single paint and
    // reflows responsively to the measured panel width (more columns when wide,
    // fewer when narrow). During the dock slide the measure is frozen at the
    // cached FINAL width and re-fit once at settle, so there's no single-column
    // transient and no settle-snap.
    //
    // #572: the previous per-cell DOM (up to 1024 divs, each painting a CSS crop
    // of a shared raster) lagged WebView2 for ~1s on open and on every scroll.
    // The canvas draws all frames once, scrolls as a static image (no redraw),
    // and hit-tests interaction by pointer math — one node instead of N.
    body = (
      <>
        {/* Pinned preview box. Its bg-bg-2 backing shows through the canvas's
            transparent (non-frame) pixels, matching the grid's uniform gray. */}
        <div className="shrink-0 p-3">
          <PreviewBox
            preview={preview}
            side={side}
            frame={previewFrame}
            rawFrame={frame}
            total={totalCells}
            pulse={pulse}
            hoverFrameRef={hoverRef}
            onRegisterHoverRedraw={registerHoverPreviewRedraw}
          />
        </div>
        {/* Scrollable canvas grid. The grid reflows responsively to this region's
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
            `stable` gutter pushes the centred canvas ~half a gutter off-centre
            (and a full gutter when no scrollbar shows), visibly misaligning the
            grid from the full-width hero above it (measured: −7px vs 0px). */}
        <div
          ref={setScrollEl}
          data-testid="atlas-scroll"
          className="atlas-grid-scroll min-h-0 flex-1 overflow-y-auto p-3"
          style={{ scrollbarGutter: "stable both-edges" }}
        >
          <AtlasFrameGrid
            gridRef={gridRef}
            okPreview={okPreview}
            totalCells={totalCells}
            side={side}
            highlight={highlight}
            rovingTarget={rovingTarget}
            deadCells={deadCells}
            layout={layout}
            onGridKeyDown={onGridKeyDown}
            onFocusCapture={() => { focusInGridRef.current = true; }}
            onBlurCapture={(e) => {
              // Clear only on a deliberate move to another real element; a blur
              // to nothing (relatedTarget null) keeps the flag so the restore
              // effect can re-home focus onto the grid after an atlas change.
              if (e.relatedTarget) focusInGridRef.current = false;
            }}
            onHover={setHoverPreview}
            onClick={stableCellClick}
          />
        </div>
      </>
    );
  }

  // ── header meta visibility ────────────────────────────────────────────────

  const showMeta = eligible && !tooLarge && !!colorTexture;

  return (
    <ToolPanel title="Atlas Frames" onClose={onClose} closing={closing} bodyScroll={false}>
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
          // Re-check at commit: the modal opened before (or the async sample landed while) it
          // was up, so the stored frame could now be known-dead. Don't assign an empty frame.
          if (deadCells.has(t.frame)) {
            setAnnouncement(`Frame ${t.frame} is empty — nothing to assign`);
            focusCell(t.frame);
            return;
          }
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

function PreviewBox({
  preview,
  side,
  frame,
  rawFrame,
  total,
  pulse,
  hoverFrameRef,
  onRegisterHoverRedraw,
}: {
  preview: PreviewState;
  side: number;
  frame: number | null;
  rawFrame: number | null;
  total: number;
  pulse: boolean;
  hoverFrameRef: React.MutableRefObject<number | null>;
  onRegisterHoverRedraw: (redraw: (() => void) | null) => void;
}) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const badgeRef = useRef<HTMLSpanElement | null>(null);
  const captionRef = useRef<HTMLSpanElement | null>(null);
  const placeholderRef = useRef<HTMLSpanElement | null>(null);
  const dataUri = preview.kind === "ok" ? preview.dataUri : null;
  const { imageRef: imgRef, imageReady: imgReady } = useDecodedImage(dataUri);
  const displayFrame = hoverFrameRef.current ?? frame;
  const placeholderText = rawFrame === null
    ? "Hover or select a frame"
    : `Frame ${rawFrame} — outside the ${side}×${side} atlas (in-game sampling is off-grid)`;

  const paintDisplay = useCallback(() => {
    const nextFrame = hoverFrameRef.current ?? frame;
    const hasFrame = nextFrame !== null;
    const canDraw = preview.kind === "ok" && hasFrame;
    const canvas = canvasRef.current;
    const badge = badgeRef.current;
    const caption = captionRef.current;
    const placeholder = placeholderRef.current;

    if (canvas) canvas.style.display = canDraw ? "" : "none";
    if (badge) {
      badge.style.display = hasFrame ? "" : "none";
      badge.textContent = hasFrame ? String(nextFrame) : "";
    }
    if (caption) {
      caption.style.display = hasFrame ? "" : "none";
      caption.textContent = hasFrame ? `Frame ${nextFrame} / ${total}` : "";
    }
    if (placeholder) {
      placeholder.style.display = hasFrame ? "none" : "";
      placeholder.textContent = hasFrame ? "" : placeholderText;
    }

    const img = imgRef.current;
    if (!canvas || !img || !imgReady || !canDraw) return;
    drawHero(canvas, img, nextFrame, side);
  }, [frame, hoverFrameRef, imgReady, placeholderText, preview.kind, side, total]);

  // The parent owns hover as a ref and schedules one redraw per frame. This
  // callback gives that scheduler an imperative, latest-value-wins paint target
  // without re-rendering the atlas grid on mouse movement.
  useEffect(() => {
    onRegisterHoverRedraw(paintDisplay);
    return () => onRegisterHoverRedraw(null);
  }, [onRegisterHoverRedraw, paintDisplay]);

  // (Re)draw when the image is ready, the keyboard/selection frame changes, or
  // the preview swaps. Hover redraws come through the registered rAF callback.
  useEffect(() => {
    paintDisplay();
  }, [paintDisplay]);

  const pulseStyle: React.CSSProperties = pulse
    ? { boxShadow: "0 0 0 2px var(--atlas-selected), 0 0 16px color-mix(in srgb, var(--atlas-selected) 60%, transparent)" }
    : {};
  const hasDisplayFrame = displayFrame !== null;

  return (
    <div
      data-testid="atlas-hero"
      className="relative flex aspect-square w-full items-center justify-center overflow-hidden rounded border border-border bg-bg-2 text-center text-xs text-text-3"
      style={{ ...pulseStyle, transition: "box-shadow var(--motion-slow-in) var(--ease-entrance)" }}
    >
      {preview.kind === "ok" && (
        <canvas
          ref={canvasRef}
          className="absolute inset-0"
          style={{ width: "100%", height: "100%", display: hasDisplayFrame ? undefined : "none" }}
        />
      )}
      <span
        ref={badgeRef}
        className="absolute left-1.5 top-1.5 rounded bg-[var(--atlas-selected)] px-1.5 text-[11px] font-semibold text-black"
        style={{ display: hasDisplayFrame ? undefined : "none" }}
      >
        {hasDisplayFrame ? displayFrame : ""}
      </span>
      <span
        ref={captionRef}
        // Fixed dark scrim + light fg over arbitrary sprite imagery —
        // intentionally theme-independent (same rationale as --overlay-scrim
        // in tokens.css); the fg routes through the scrim-fg token.
        className="absolute inset-x-0 bottom-0 bg-gradient-to-t from-black/80 to-transparent px-2 pb-1 pt-3 text-xs font-semibold text-[var(--overlay-scrim-fg)]"
        style={{ display: hasDisplayFrame ? undefined : "none" }}
      >
        {hasDisplayFrame ? `Frame ${displayFrame} / ${total}` : ""}
      </span>
      <span ref={placeholderRef} style={{ display: hasDisplayFrame ? "none" : undefined }}>
        {hasDisplayFrame ? "" : placeholderText}
      </span>
    </div>
  );
}

