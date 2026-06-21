// PanelLayout — B1.4 resizable splitters for the editor shell's
// main row.
//
// Replaces the Tailwind flex layout that used to live in App.tsx's
// "Main row" block (the panel + viewport + spawner three-column grid)
// with a tree of nested `react-resizable-panels` Groups. Four
// splitters:
//
//   1. left ↔ centre   (outer Group, horizontal)
//   2. centre ↔ spawner (outer Group, horizontal; only when spawnerVisible)
//   3. viewport ↔ curve (centre Group, vertical)
//   4. tree ↔ tabs      (left Group, vertical)
//
// Persistence is DIY in 4.x (autoSaveId is gone): on mount we read
// {alo:layout:outer:{2col|3col}, alo:layout:left, alo:layout:center}
// from localStorage and pass them as the Group's `defaultLayout`; on
// pointer-release the library calls `onLayoutChanged` with the new
// percentage map, which we persist back. Each Group's stored blob is
// validated on read (JSON.parse, key set matches defaults, ratios sum
// to ~100); any failure falls back to in-code defaults rather than
// crashing the layout.
//
// The five quadrant-* data-testids live on the inner divs *inside*
// each Panel's children, not on the Panel itself. Two reasons:
//   - Risk 2 in the plan: Modal.tsx's
//     `document.querySelector('[data-testid="quadrant-viewport"]')`
//     and the subsequent getBoundingClientRect rely on the rect
//     matching the viewport pixels exactly. Placing the testID on
//     our own `.relative h-full` div keeps the geometry identical to
//     today's App.tsx:234 site.
//   - The library's Panel renders a wrapper div (className lands on
//     a nested DOM node); the testID on our inner div keeps the
//     rect semantics unambiguous.
//
// Spawner mount/unmount: 4.x doesn't hash the panel-id set into the
// persistence key, so the 3-col state (left+centre+spawner) and the
// 2-col state (left+centre) get distinct localStorage keys
// (`:3col` vs `:2col`). The outer Group is `key`'d on
// `spawnerVisible` so React fully remounts it when the user flips
// the toolbar's Spawner button — the new mount reads from the
// appropriate key with the appropriate defaults.

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Group, Panel, Separator, usePanelRef, type Layout } from "react-resizable-panels";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useRightDock, setDock } from "@/lib/right-dock";
import { computeSceneRect, dockSlideTarget } from "@/lib/scene-rect";
import { useDockAnim } from "@/lib/dock-anim";
import { ViewportSlot } from "./ViewportSlot";
import { OverloadBanner } from "./OverloadBanner";
import { CurveEditorPanel } from "./CurveEditorPanel";
import { EmitterPropertyTabs } from "@/screens/EmitterPropertyTabs";
import { EmitterTree } from "@/screens/EmitterTree";
import { LightingPanel } from "@/screens/LightingPanel";
import { SpawnerPanel } from "@/screens/SpawnerPanel";
import { AtlasPickerPanel } from "@/screens/AtlasPickerPanel";

export type { Layout };

// Splitter hit/cursor zone. The visible `.ce-splitter` band is 8px; we pin
// the library's mouse (`fine`) resize-target minimum to the same 8px so it
// stops inflating a thin handle to its 10px default — which spilled the
// resize cursor ~3px onto the viewport where no splitter was visible.
// `coarse` (touch) stays generous; this is a mouse-driven desktop app.
const RESIZE_HIT_MIN = { coarse: 20, fine: 8 } as const;

// In-code defaults. Each map's keys must match the id= attributes on
// the corresponding Panel components below.
const OUTER_3COL_DEFAULTS: Layout = { left: 20, center: 60, spawner: 20 };
const LEFT_DEFAULTS: Layout = { tree: 25, tabs: 75 };
const CENTER_DEFAULTS: Layout = { viewport: 75, curve: 25 };

// The right-dock slot's pixel floor. Pinned to the Panel's `minSize` below so
// they stay in lockstep, and reused by the Item-3 dock-slide anim as the
// fallback open-target width on a first-ever open (before any close has
// recorded the dock's remembered width).
const DOCK_MIN_PX = 260;

// B1.4 T6: every localStorage key PanelLayout owns. Exported so the
// View → Reset panel layout menu item can clear them all in one shot
// before AppShell bumps the key= that remounts PanelLayout with the
// in-code defaults above.
export const PANEL_LAYOUT_KEYS = [
  "alo:layout:outer:2col",
  "alo:layout:outer:3col",
  "alo:layout:left",
  "alo:layout:center",
] as const;

/** Clear every persisted panel-layout entry. Used by the View →
 *  Reset panel layout menu item. Combined with a `key=` bump on the
 *  PanelLayout mount, this forces every Group to re-read defaults on
 *  the next mount. */
export function resetPanelLayoutStorage(): void {
  if (typeof localStorage === "undefined") return;
  for (const key of PANEL_LAYOUT_KEYS) {
    try {
      localStorage.removeItem(key);
    } catch {
      /* localStorage disabled — drop silently */
    }
  }
}

/** Persisted-layout reader. Pure for unit-testability. */
export function loadLayout(key: string, defaults: Layout): Layout {
  try {
    const raw =
      (typeof localStorage !== "undefined") ? localStorage.getItem(key) : null;
    if (!raw) return defaults;
    const parsed = JSON.parse(raw) as unknown;
    if (parsed === null || typeof parsed !== "object") return defaults;
    const m = parsed as Record<string, unknown>;
    const allKeysMatch = Object.keys(defaults).every(
      (k) => typeof m[k] === "number" && Number.isFinite(m[k]),
    );
    if (!allKeysMatch) return defaults;
    const sum = Object.keys(defaults).reduce((a, k) => a + (m[k] as number), 0);
    if (Math.abs(sum - 100) > 0.5) return defaults;
    // Strip any extra keys not in defaults — the Group ignores them, but
    // keeping persistence shape clean avoids accidental key drift.
    const trimmed: Layout = {};
    for (const k of Object.keys(defaults)) trimmed[k] = m[k] as number;
    return trimmed;
  } catch {
    return defaults;
  }
}

/** Persisted-layout writer. Drops silently if localStorage is full / disabled. */
export function saveLayout(key: string, layout: Layout): void {
  try {
    localStorage.setItem(key, JSON.stringify(layout));
  } catch {
    /* localStorage full / disabled — drop silently */
  }
}

/** [dock-mount fix, 2026-06-10] Fold the right-dock's share into the centre
 *  column. Used to seed the outer Group's `defaultLayout` when the dock is
 *  CLOSED at mount: the stored 3col blob remembers the dock's OPEN width
 *  (persistence is gated on dockVisible), so feeding it verbatim to the
 *  Group seeds the dock slot open-and-empty (~its old share) on boot. The
 *  dock Panel's own `defaultSize` is already "0%" in that case, but the lib
 *  has TWO init paths — immediate (Group `defaultLayout` wins) and deferred
 *  (Panel `defaultSize` wins, see the 4.x quirk note below) — and the mount
 *  effect's isCollapsed() check races the deferred apply. Making both seeds
 *  agree on spawner=0 removes the race instead of correcting it after the
 *  fact. Pure + exported for the regression test. Sum is preserved (the
 *  loadLayout ~100 validation stays satisfied). */
export function collapseDockShare(layout: Layout): Layout {
  const spawner = layout.spawner ?? 0;
  if (!(spawner > 0)) return layout;
  return { ...layout, center: (layout.center ?? 0) + spawner, spawner: 0 };
}

function usePersistedLayout(key: string, defaults: Layout) {
  // useMemo with [key] so a visibility flip (key change) re-reads.
  const defaultLayout = useMemo(() => loadLayout(key, defaults), [key, defaults]);
  const onLayoutChanged = useCallback(
    (layout: Layout) => {
      saveLayout(key, layout);
    },
    [key],
  );
  return { defaultLayout, onLayoutChanged };
}

type Props = { bridge: Bridge };

export function PanelLayout({ bridge }: Props) {
  // The right-dock slot holds the Spawner OR the Lighting pane (exclusive),
  // or nothing. The outer layout only cares whether the column EXISTS —
  // swapping spawner↔lighting keeps it open and reflows nothing; only the
  // present↔absent transition carves / absorbs the column's width.
  const dock = useRightDock();
  const dockVisible = dock !== null;

  // The outer 3-col layout is ALWAYS mounted: the right-dock is a
  // collapsible slot, not a conditionally-rendered panel. So the Group
  // never remounts on open/close — the left pane no longer flickers — and
  // the slot can animate open/closed. One persisted layout; the library
  // remembers the dock's expanded size across a collapse, so the old
  // 2col/3col dual-key carry-over machinery is gone.
  //
  // Capture the dock's open/closed state AT MOUNT so the dock Panel's
  // initial defaultSize matches (mount collapsed when closed); toggles
  // after mount are driven imperatively via dockPanelRef.
  const dockVisibleAtMount = useRef(dockVisible).current;
  // [dock-mount fix] Closed at mount → fold the stored dock share into the
  // centre column so the Group's defaultLayout agrees with the dock Panel's
  // "0%" defaultSize (see collapseDockShare). The STORED blob is untouched —
  // persistence is dockVisible-gated — so the remembered open width still
  // seeds the next open-at-mount session.
  // storedDockSharePct keeps the blob's ORIGINAL spawner share even when the
  // seed below zeroes it — the first open after a closed-at-mount session
  // restores the dock to this width (see expandDock in the toggle effect).
  const { outerDefaultLayout, storedDockSharePct } = useMemo(() => {
    const stored = loadLayout("alo:layout:outer:3col", OUTER_3COL_DEFAULTS);
    return {
      outerDefaultLayout: dockVisibleAtMount ? stored : collapseDockShare(stored),
      storedDockSharePct: stored.spawner ?? null,
    };
  }, [dockVisibleAtMount]);
  // Persist only while the dock is OPEN, so a closed (collapsed) layout
  // never overwrites the remembered open widths.
  const onOuterLayoutChanged = useCallback(
    (l: Layout) => {
      if (dockVisible) saveLayout("alo:layout:outer:3col", l);
    },
    [dockVisible],
  );

  // Collapse the dock slot when closed, expand when open. Spawner↔lighting
  // keeps dockVisible true → no collapse/expand, just a content swap.
  // The toggle animates by enabling a `flex` transition (`.dock-animating`)
  // for the duration of THIS open/close only — a permanent transition would
  // lag splitter drags + window resizes. The size change is deferred one
  // frame so the class is committed before flex changes (else the first
  // frame jumps with nothing to tween).
  const dockPanelRef = usePanelRef();
  // Drives the `.dock-animating` CSS class (the flex-grow tween). Distinct from
  // the dock-anim STORE signal used below, which suppresses ViewportSlot's RO
  // sends during the host-interpolated slide.
  const [dockAnimating, setDockAnimating] = useState(false);
  // Shadow of react-resizable-panels' internal `expandToSize`: the dock's
  // SETTLED pixel width, captured at the open-SETTLE (not at the toggle), so
  // both the close (how much the centre grows) and the next open (how much it
  // shrinks) use a stable value. Capturing at the toggle would read a
  // mid-CSS-transition `offsetWidth` during a rapid re-toggle and poison the
  // next slide's target (the host then snaps at the settle).
  const lastDockWidthCssRef = useRef<number | null>(null);

  useEffect(() => {
    const p = dockPanelRef.current;
    if (!p) return;
    const need = dockVisible ? p.isCollapsed() : !p.isCollapsed();
    if (!need) return;

    setDockAnimating(true);

    // [Item 3] Drive a host-side time-interpolated viewport rect synced to the
    // CSS flex-grow tween, so the engine viewport edge glides with the panel
    // instead of juddering against the clumpy per-frame scene-rect stream.
    const vpEl = document.querySelector('[data-testid="quadrant-viewport"]') as HTMLElement | null;

    // Dock width (CSS px) that transfers to/from the centre column. Prefer the
    // remembered SETTLED width (captured at the last open-settle). OPEN with no
    // remembered width yet → the panel min (first-ever open); CLOSE with none →
    // read the live width, which is accurate because a first close happens from
    // a settled-open dock (the only residual is a first-open immediately
    // re-closed mid-transition, which the authoritative settle send corrects).
    const dockWidthCss = dockVisible
      ? lastDockWidthCssRef.current ?? DOCK_MIN_PX
      : lastDockWidthCssRef.current ?? p.getSize().inPixels;

    // [dock-mount fix] expand() restores the library's remembered
    // pre-collapse size (`expandToSize`), falling back to minSize when
    // nothing was recorded — the closed-AT-MOUNT case, where no
    // in-session collapse ever ran. That minSize fallback is the
    // DESIRED first-open width: it equals DOCK_MIN_PX, which is also
    // what the dock-slide anim assumes for a first-ever open
    // (dockWidthCss above), so panel and host anim agree. The resize
    // chain below is purely a safety net for expand() landing at ~zero
    // share (never observed with the current lib, but cheap to keep):
    // in-session settled width, else the stored blob's share
    // (storedDockSharePct — the Group seed zeroed it via
    // collapseDockShare), else the px floor.
    const expandDock = () => {
      p.expand();
      // Guard on asPercentage, NOT inPixels: the layout share updates
      // synchronously on expand(), but inPixels reads offsetWidth, which
      // only reflects the new flex style after the next render — it
      // reads a stale 0 here even on a successful expand, and acting on
      // it would clobber the remembered reopen width every time.
      if (p.getSize().asPercentage < 0.5) {
        if (lastDockWidthCssRef.current != null) {
          p.resize(lastDockWidthCssRef.current);
        } else if (storedDockSharePct != null && storedDockSharePct > 0) {
          p.resize(`${storedDockSharePct}%`);
        } else {
          p.resize(DOCK_MIN_PX);
        }
      }
    };

    const reducedMotion =
      typeof window.matchMedia === "function" &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    const animate = vpEl !== null && dockWidthCss > 0 && !reducedMotion;
    if (animate) useDockAnim.getState().setAnimating(true);

    // Pre-compute the scene rects (only when animating) BEFORE scheduling the
    // rAF, so the slide-start callback is pure scheduling.
    const dpr = window.devicePixelRatio || 1;
    let from: ReturnType<typeof computeSceneRect> | null = null;
    let to: ReturnType<typeof computeSceneRect> | null = null;
    if (animate && vpEl) {
      from = computeSceneRect(vpEl);
      const deltaDev = Math.round(dockWidthCss * dpr);
      // `dockVisible` true here means we are OPENING (the dock becomes visible);
      // the viewport shrinks by the dock width. CLOSE grows it. (The narrow-
      // window edge where the LEFT pane also moves is corrected by the
      // authoritative settle send below, which reads the real settled rect.)
      to = dockSlideTarget(from, deltaDev, dockVisible);
    }

    // The settle window (260ms > the 200ms tween) ends the CSS class + the
    // suppression signal, records the SETTLED dock width for the next slide, and
    // sends ONE authoritative layout/scene-rect at the real rest rect. CRITICAL:
    // it is armed when the slide ACTUALLY STARTS (inside startSlide), NOT at
    // effect-run — the atlas cold-start gate can delay startSlide by up to ~280ms,
    // and a settle anchored to effect-run would fire mid-tween, flipping
    // `animating` false while the panel is still moving. That un-freezes the
    // atlas grid (→ the live column reflow) and un-holds the curve editor.
    let settleTimer = 0;
    const scheduleSettle = () => {
      settleTimer = window.setTimeout(() => {
        setDockAnimating(false);
        if (animate) useDockAnim.getState().setAnimating(false);
        const settledDockW = p.getSize().inPixels;
        if (settledDockW > 0) lastDockWidthCssRef.current = settledDockW;
        if (vpEl) {
          void bridge
            .request({ kind: "layout/scene-rect", params: computeSceneRect(vpEl) })
            .catch(() => {});
        }
      }, 260);
    };

    // Flip the flex-grow (expand/collapse) and, if animating, send the host the
    // synced viewport-rect tween, then arm the settle relative to THIS moment.
    // Reduced-motion / unknown width → snap only (the CSS transition is `none`).
    const startSlide = (rafTs: number) => {
      if (dockVisible) expandDock();
      else p.collapse();
      if (animate && from && to) {
        // Stamp ms since the flex actually changed (this rAF) so the host can
        // back-date its QPC clock to the CSS origin across the IPC hop;
        // performance.now() and rafTs share the same clock.
        const msElapsedAtSend = Math.max(0, performance.now() - rafTs);
        void bridge
          .request({
            kind: "animate-scene-rect",
            params: { from, to, durationMs: 200, easing: "ease", msElapsedAtSend },
          })
          .catch(() => {});
      }
      scheduleSettle();
    };

    let raf = 0;
    let raf2 = 0;
    // Cold-start gate for the ATLAS open only: wait until its cell grid is in the
    // DOM before the tween (see below). Tracked here so cleanup can tear them down.
    let readySub: (() => void) | null = null;
    let readyTimeout = 0;
    if (dockVisible && dock === "atlas") {
      // OPEN (atlas): gate the slide START on the grid being mounted (atlasReady).
      // A RE-OPEN renders the grid synchronously from the props/preview caches, so
      // atlasReady is already true here → start immediately (one rAF for a fresh
      // rafTs). A COLD first open renders the grid only after async round-trips, so
      // we start the slide on the false→true edge — eliminating the centre-column
      // overlap from the grid mount landing mid-tween. A max-timeout fallback
      // starts the slide regardless, so a grid that never mounts can't hang it.
      const fire = () => {
        if (raf) return; // already started (edge + timeout race, or re-entry)
        if (readyTimeout) { clearTimeout(readyTimeout); readyTimeout = 0; }
        raf = requestAnimationFrame(startSlide);
      };
      // READ readiness BEFORE clearing it. On a RE-OPEN the AtlasPickerPanel
      // (a child) mounts its grid synchronously from the caches and its
      // gridMounted effect runs BEFORE this parent effect (React runs child
      // effects first), so atlasReady is already true here → start immediately.
      // Otherwise (cold open) clear any stale readiness and wait for THIS grid
      // mount's false→true edge.
      const readyAtRun = useDockAnim.getState().atlasReady;
      if (readyAtRun) {
        fire(); // re-open: grid already mounted via the caches → start now
      } else {
        useDockAnim.getState().setAtlasReady(false); // defensive: clear stale
        readySub = useDockAnim.subscribe((s) => {
          if (s.atlasReady) { readySub?.(); readySub = null; fire(); }
        });
        // Fallback: never let the slide HANG if the grid never mounts. This is a
        // pathological-case net (a real grid mounts in well under this), so its
        // exact value isn't tween-critical — it only guarantees the dock still
        // opens and the flags still clear. The normal cold-open path is the
        // false→true edge above, which fires the moment the grid renders.
        readyTimeout = window.setTimeout(() => {
          readySub?.(); readySub = null; fire();
        }, 280);
      }
    } else if (dockVisible) {
      // OPEN (lighting/spawner): DOUBLE rAF. The panel mounts NOW; frame 1 lets
      // that content render + paint, then frame 2 starts the slide tween
      // uncontended — eliminating the centre-column lag/snap that came from the
      // mount landing in the middle of the flex tween.
      raf = requestAnimationFrame(() => {
        raf2 = requestAnimationFrame(startSlide);
      });
    } else {
      // CLOSE: single rAF — nothing is mounting, so no extra paint frame needed.
      raf = requestAnimationFrame(startSlide);
    }

    return () => {
      cancelAnimationFrame(raf);
      cancelAnimationFrame(raf2);
      clearTimeout(settleTimer);
      // Tear down the atlas cold-start readiness gate (no-ops on the other
      // branches): cancel the subscription and the fallback timeout so a
      // superseding toggle / unmount can't start a stale slide or leak a timer.
      readySub?.();
      if (readyTimeout) clearTimeout(readyTimeout);
      // A superseding toggle (or unmount) cancels the settle timer above — the
      // ONLY happy-path clear. Drop the in-flight slide's flags here too, else
      // a re-toggle that early-returns (or takes the non-animate branch) leaves
      // the suppression signal stuck true and silences ViewportSlot's RO
      // indefinitely. The re-run re-raises if the new toggle animates. Also
      // covers unmount (this cleanup runs then), so no separate unmount net is
      // needed. (Clearing dockAnimating here additionally fixes the pre-existing
      // CSS-class stick on the same re-toggle path.)
      useDockAnim.getState().setAnimating(false);
      setDockAnimating(false);
    };
  }, [dockVisible, dockPanelRef, bridge, storedDockSharePct]);

  // The content shown in the dock slot LAGS `dock` on close: keep the last
  // pane mounted while the slot animates shut so it slides out instead of
  // popping, then clear it. Open/swap shows the new pane immediately.
  const [displayDock, setDisplayDock] = useState(dock);
  useEffect(() => {
    if (dock !== null) {
      setDisplayDock(dock);
      return;
    }
    const t = setTimeout(() => setDisplayDock(null), 260);
    return () => clearTimeout(t);
  }, [dock]);

  // The dock is CLOSING when the logical state says closed (dock === null)
  // but the content is still mounted for the slide-out (displayDock !== null).
  // During this window the panel must NOT be an interactive, openable dialog:
  // it's sliding away and about to unmount. `inert` removes it from the
  // focus/hit-test/a11y tree (correctness — you can't click a panel that's
  // leaving); ToolPanel additionally stamps data-state="closing" so it no
  // longer matches an "open ToolPanel" selector. Without this, a click that
  // lands in the ~260ms slide-out window (Playwright's strict actionability,
  // or a fast user) targets a shrinking-then-detaching Close button and
  // never lands.
  const dockClosing = dock === null && displayDock !== null;

  const left = usePersistedLayout("alo:layout:left", LEFT_DEFAULTS);
  const center = usePersistedLayout("alo:layout:center", CENTER_DEFAULTS);

  // B1.4 T4c: React no longer dispatches a popup-rect to the
  // host. The host self-sizes the engine popup HWND to its own main
  // client area on WM_CREATE / WM_SIZE / WM_WINDOWPOSCHANGED
  // (HostWindow.cpp). The popup is therefore stable across splitter
  // drags — only the SCENE rect changes, dispatched by ViewportSlot
  // as layout/scene-rect. Zero React→host postMessages for layout
  // rect (other than the per-frame scene-rect updates).

  // 4.x quirk: when a Group mounts with `groupSize === 0` (typical
  // for nested flex containers on first paint), the library flips
  // `defaultLayoutDeferred = true` and, on the first ResizeObserver
  // tick once the group gains real size, calls `We(panels)` —
  // which reads each Panel's `defaultSize` prop and IGNORES the
  // Group's `defaultLayout`. The Group prop is effectively an
  // SSR-hydration hint only. So per-Panel `defaultSize` is the
  // canonical knob — pull it from the loaded layout.
  return (
    <Group
      orientation="horizontal"
      className={dockAnimating ? "dock-animating" : undefined}
      defaultLayout={outerDefaultLayout}
      onLayoutChanged={onOuterLayoutChanged}
      resizeTargetMinimumSize={RESIZE_HIT_MIN}
      style={{ flex: 1, minHeight: 0, overflow: "hidden" }}
    >
      {/* Pixel (not %) minSize: inspector `.form-row` labels live in a
          flexible column with text-overflow:ellipsis, so they truncate
          when this pane is dragged narrow. A % floor is window-relative
          (fine when wide, truncates on small windows); a px floor is
          absolute. 330px fits the longest spinner-row label
          ("Distance from camera:") next to its 58px/40px input+unit
          columns. (Long *checkbox* labels are handled separately by the
          `.form-row-check` grid, which frees the column the checkbox
          otherwise wasted.) maxSize stays a % so a wide window caps it. */}
      <Panel
        id="left"
        defaultSize={`${outerDefaultLayout.left}%`}
        minSize={330}
        maxSize="40%"
      >
        <div className="panel panel-flush-right h-full w-full">
          <div className="panel-header">
            <span>Particle System</span>
          </div>
          <div className="panel-body flex min-h-0 flex-col overflow-hidden">
            <Group
              orientation="vertical"
              defaultLayout={left.defaultLayout}
              onLayoutChanged={left.onLayoutChanged}
              resizeTargetMinimumSize={RESIZE_HIT_MIN}
              style={{ flex: 1, minHeight: 0 }}
            >
              <Panel
                id="tree"
                defaultSize={`${left.defaultLayout.tree}%`}
                minSize="10%"
              >
                <aside
                  data-testid="quadrant-emitter-tree"
                  className="h-full w-full min-h-0 overflow-hidden flex flex-col p-3 text-sm"
                >
                  <EmitterTree bridge={bridge} />
                </aside>
              </Panel>
              <Separator className="ce-splitter ce-splitter-h" />
              <Panel
                id="tabs"
                defaultSize={`${left.defaultLayout.tabs}%`}
                minSize="20%"
              >
                <div
                  data-testid="quadrant-property-tabs"
                  className="h-full w-full min-h-0"
                >
                  <EmitterPropertyTabs bridge={bridge} />
                </div>
              </Panel>
            </Group>
          </div>
        </div>
      </Panel>

      <Separator className="ce-splitter ce-splitter-v" />

      <Panel
        id="center"
        defaultSize={`${outerDefaultLayout.center}%`}
        minSize="30%"
      >
        <Group
          orientation="vertical"
          defaultLayout={center.defaultLayout}
          onLayoutChanged={center.onLayoutChanged}
          resizeTargetMinimumSize={RESIZE_HIT_MIN}
          style={{ flex: 1, minHeight: 0, width: "100%" }}
        >
          <Panel
            id="viewport"
            defaultSize={`${center.defaultLayout.viewport}%`}
            minSize="30%"
          >
            <div
              data-testid="quadrant-viewport"
              className="relative h-full w-full min-h-0"
            >
              <ViewportSlot bridge={bridge} />
              {/* Preview spawn-overload banner (overload guard §3) — a
                  sibling of ViewportSlot inside this `relative`
                  viewport-rect container so `absolute top-center`
                  positioning needs no extra geometry. Registers its
                  own viewport occlusion (risk 4); see the component
                  header for the full rationale. */}
              <OverloadBanner bridge={bridge} />
            </div>
          </Panel>
          <Separator className="ce-splitter ce-splitter-h" />
          <Panel
            id="curve"
            defaultSize={`${center.defaultLayout.curve}%`}
            minSize="10%"
          >
            <div
              data-testid="quadrant-curve-editor"
              className="h-full w-full min-h-0 border-t border-border"
            >
              <CurveEditorPanel bridge={bridge} />
            </div>
          </Panel>
        </Group>
      </Panel>

      {/* Right dock — ALWAYS mounted as a collapsible slot (collapsedSize 0),
          so opening/closing never remounts the Group (no left-pane flicker)
          and the slot can animate. The separator hides while collapsed so no
          handle floats at the edge. Pixel minSize (260) keeps the docked
          labels readable; the Panel id stays "spawner" regardless of content
          so the persistence key (alo:layout:outer:3col) is stable — the
          slot's identity is the right-dock, not the tool inside it.

          While CLOSED, both the separator and the panel are `disabled` —
          the CSS classes alone do NOT stop a drag, because the library
          hit-tests separators by document-level pointer COORDINATES
          against their rects (an invisible, pointer-events-none element
          still has a rect), so the user could drag an empty slot open
          (bug, 2026-06-10). Separator.disabled removes it from that
          hit-test; Panel.disabled stops indirect resizes (the lib's own
          docs: "to prevent a panel from being resized at all, it needs
          to also be disabled"). The dock toggle's imperative
          p.expand()/p.collapse() still works — trigger="imperative-api"
          overrides disabled-panel constraints in the lib. */}
      <Separator
        disabled={!dockVisible}
        className={
          "ce-splitter ce-splitter-v" +
          (dockVisible ? "" : " invisible pointer-events-none")
        }
      />
      <Panel
        id="spawner"
        panelRef={dockPanelRef}
        disabled={!dockVisible}
        collapsible
        collapsedSize={0}
        defaultSize={dockVisibleAtMount ? `${outerDefaultLayout.spawner ?? 20}%` : "0%"}
        minSize={DOCK_MIN_PX}
        maxSize="40%"
      >
        <aside
          data-testid="quadrant-spawner"
          className="h-full w-full overflow-hidden"
          inert={dockClosing}
        >
          {displayDock === "spawner" ? (
            <SpawnerPanel bridge={bridge} />
          ) : displayDock === "lighting" ? (
            <LightingPanel bridge={bridge} onClose={() => setDock(null)} closing={dockClosing} />
          ) : displayDock === "atlas" ? (
            <AtlasPickerPanel bridge={bridge} onClose={() => setDock(null)} closing={dockClosing} />
          ) : null}
        </aside>
      </Panel>
    </Group>
  );
}
