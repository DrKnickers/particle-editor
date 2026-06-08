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
import { ViewportSlot } from "./ViewportSlot";
import { CurveEditorPanel } from "./CurveEditorPanel";
import { EmitterPropertyTabs } from "@/screens/EmitterPropertyTabs";
import { EmitterTree } from "@/screens/EmitterTree";
import { LightingPanel } from "@/screens/LightingPanel";
import { SpawnerPanel } from "@/screens/SpawnerPanel";

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
  const outerDefaultLayout = useMemo<Layout>(
    () => loadLayout("alo:layout:outer:3col", OUTER_3COL_DEFAULTS),
    [],
  );
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
  const [dockAnimating, setDockAnimating] = useState(false);
  useEffect(() => {
    const p = dockPanelRef.current;
    if (!p) return;
    const need = dockVisible ? p.isCollapsed() : !p.isCollapsed();
    if (!need) return;
    setDockAnimating(true);
    const raf = requestAnimationFrame(() => {
      if (dockVisible) p.expand();
      else p.collapse();
    });
    const t = setTimeout(() => setDockAnimating(false), 260);
    return () => {
      cancelAnimationFrame(raf);
      clearTimeout(t);
    };
  }, [dockVisible, dockPanelRef]);

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
          slot's identity is the right-dock, not the tool inside it. */}
      <Separator
        className={
          "ce-splitter ce-splitter-v" +
          (dockVisible ? "" : " invisible pointer-events-none")
        }
      />
      <Panel
        id="spawner"
        panelRef={dockPanelRef}
        collapsible
        collapsedSize={0}
        defaultSize={dockVisibleAtMount ? `${outerDefaultLayout.spawner ?? 20}%` : "0%"}
        minSize={260}
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
          ) : null}
        </aside>
      </Panel>
    </Group>
  );
}
