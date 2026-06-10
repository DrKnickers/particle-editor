// Vitest for the PanelLayout component (B1.4).
//
// PanelLayout owns the editor shell's main row: outer horizontal Group
// (left | centre | spawner) + nested vertical Groups inside the left
// column (tree / tabs) and centre column (viewport / curve). Persists
// per-Group ratios to localStorage via the `usePersistedLayout` hook,
// keyed by alo:layout:outer:{2col,3col} / alo:layout:left /
// alo:layout:center.
//
// These tests are the failing-skeleton end of T2 (the impl ships in T3).
// We pin three things:
//   1. Pure persistence helpers handle the corruption / missing-key /
//      ratio-drift cases (each test is small and the helpers stay
//      separately importable).
//   2. The PanelLayout DOM exposes the five quadrant-* data-testids
//      production code depends on (Modal.tsx querySelector,
//      Playwright specs, viewport-rect callbacks). The testIDs must
//      live on the same semantic inner divs they sit on today so
//      Risk 2 in tasks/todo.md is enforced by the test, not just by
//      vigilance.
//   3. Spawner visibility flips the spawner panel mount/unmount.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen } from "@testing-library/react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { BridgeContext } from "@/lib/bridge-context";
import { PanelLayout } from "../PanelLayout";
import {
  loadLayout,
  saveLayout,
  resetPanelLayoutStorage,
  collapseDockShare,
  PANEL_LAYOUT_KEYS,
  type Layout,
} from "../PanelLayout";
import { __resetRightDockForTests, setDock } from "@/lib/right-dock";

function makeStubBridge(): Bridge {
  return {
    request: () => Promise.resolve({}),
    on: () => () => {},
  } as unknown as Bridge;
}

beforeEach(() => {
  localStorage.removeItem("alo:right-dock");
  localStorage.removeItem("alo:spawner-visible");
  __resetRightDockForTests();
});

describe("PanelLayout — persistence helpers", () => {
  const DEFAULTS: Layout = { a: 25, b: 75 };

  it("loadLayout returns defaults when the key is missing", () => {
    expect(loadLayout("alo:layout:test", DEFAULTS)).toEqual(DEFAULTS);
  });

  it("loadLayout returns persisted value for a valid blob", () => {
    localStorage.setItem("alo:layout:test", JSON.stringify({ a: 40, b: 60 }));
    expect(loadLayout("alo:layout:test", DEFAULTS)).toEqual({ a: 40, b: 60 });
  });

  it("loadLayout falls back when the blob isn't JSON", () => {
    localStorage.setItem("alo:layout:test", "not-json-{{{");
    expect(loadLayout("alo:layout:test", DEFAULTS)).toEqual(DEFAULTS);
  });

  it("loadLayout falls back when a default key is missing from the blob", () => {
    // Legacy key set or partial write — keys don't match defaults shape.
    localStorage.setItem("alo:layout:test", JSON.stringify({ a: 100 }));
    expect(loadLayout("alo:layout:test", DEFAULTS)).toEqual(DEFAULTS);
  });

  it("loadLayout falls back when ratios don't sum to ~100", () => {
    localStorage.setItem("alo:layout:test", JSON.stringify({ a: 10, b: 10 }));
    expect(loadLayout("alo:layout:test", DEFAULTS)).toEqual(DEFAULTS);
  });

  it("saveLayout writes JSON to localStorage", () => {
    saveLayout("alo:layout:test", { a: 33, b: 67 });
    expect(localStorage.getItem("alo:layout:test")).toBe(
      JSON.stringify({ a: 33, b: 67 }),
    );
  });
});

describe("PanelLayout — collapseDockShare (dock-mount fix)", () => {
  // Regression (2026-06-10): a CLOSED dock at mount + a persisted
  // alo:layout:outer:3col with a nonzero spawner share rendered the dock
  // slot open-and-empty (~its old share) and it never collapsed. The
  // Group's defaultLayout (stored blob, spawner 20) disagreed with the
  // dock Panel's "0%" defaultSize, and the mount effect's isCollapsed()
  // check raced the library's deferred initial-layout apply. The fix
  // makes both seeds agree by folding the dock share into centre.

  it("folds the spawner share into center and zeroes spawner", () => {
    expect(collapseDockShare({ left: 20, center: 60, spawner: 20 })).toEqual({
      left: 20,
      center: 80,
      spawner: 0,
    });
  });

  it("preserves the ~100 sum loadLayout validates", () => {
    const out = collapseDockShare({ left: 33, center: 33, spawner: 34 });
    const sum = Object.values(out).reduce((a, v) => a + v, 0);
    expect(Math.abs(sum - 100)).toBeLessThanOrEqual(0.5);
  });

  it("is a no-op when spawner is already 0 or absent", () => {
    const zero = { left: 30, center: 70, spawner: 0 };
    expect(collapseDockShare(zero)).toEqual(zero);
    const absent = { left: 30, center: 70 };
    expect(collapseDockShare(absent)).toEqual(absent);
  });

  it("seeds the dock slot at zero share when closed at mount with a stale stored layout", () => {
    // The full repro: dock closed + persisted open-width blob.
    localStorage.setItem("alo:right-dock", "none");
    localStorage.setItem(
      "alo:layout:outer:3col",
      JSON.stringify({ left: 20, center: 60, spawner: 20 }),
    );
    __resetRightDockForTests();

    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );

    const slotPanel = screen
      .getByTestId("quadrant-spawner")
      .closest("[data-panel]") as HTMLElement;
    expect(slotPanel).not.toBeNull();
    // The library maps each panel's layout share onto flex-grow. With the
    // fix, the Group's defaultLayout carries spawner=0 (the stale 20 is
    // folded into center), so the slot renders with zero share instead of
    // its remembered open width.
    expect(slotPanel.style.flexGrow).toBe("0");
  });
});

describe("PanelLayout — Reset panel layout (T6)", () => {
  it("resetPanelLayoutStorage clears every key listed in PANEL_LAYOUT_KEYS", () => {
    // Seed every persisted-layout key with arbitrary content; the
    // helper should wipe all four without touching unrelated keys.
    for (const k of PANEL_LAYOUT_KEYS) {
      localStorage.setItem(k, JSON.stringify({ stub: 1 }));
    }
    localStorage.setItem("alo:unrelated", "keep-me");

    resetPanelLayoutStorage();

    for (const k of PANEL_LAYOUT_KEYS) {
      expect(localStorage.getItem(k)).toBeNull();
    }
    expect(localStorage.getItem("alo:unrelated")).toBe("keep-me");
  });

  it("loadLayout returns defaults for every key after resetPanelLayoutStorage", () => {
    // End-to-end: a stored layout is honoured first, then the reset
    // gesture restores defaults on the next read.
    const DEFAULTS: Layout = { x: 30, y: 70 };
    for (const k of PANEL_LAYOUT_KEYS) {
      localStorage.setItem(k, JSON.stringify({ x: 60, y: 40 }));
      expect(loadLayout(k, DEFAULTS)).toEqual({ x: 60, y: 40 });
    }
    resetPanelLayoutStorage();
    for (const k of PANEL_LAYOUT_KEYS) {
      expect(loadLayout(k, DEFAULTS)).toEqual(DEFAULTS);
    }
  });

  it("PANEL_LAYOUT_KEYS covers every key PanelLayout writes (outer:2col/3col + left + center)", () => {
    // Guards against drift: if a future change adds a new persisted
    // Group, this test must be updated alongside PANEL_LAYOUT_KEYS or
    // the menu item silently leaks state across resets.
    expect([...PANEL_LAYOUT_KEYS].sort()).toEqual([
      "alo:layout:center",
      "alo:layout:left",
      "alo:layout:outer:2col",
      "alo:layout:outer:3col",
    ]);
  });
});

describe("PanelLayout — DOM structure", () => {
  it("renders all five quadrant testIDs when Spawner is visible", () => {
    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );
    // Default right-dock is `"spawner"` (lib/right-dock.ts).
    expect(screen.getByTestId("quadrant-emitter-tree")).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-property-tabs")).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-viewport")).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-curve-editor")).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-spawner")).toBeInTheDocument();
  });

  it("keeps the right-dock slot mounted but empty when the dock is closed", () => {
    // Seed the localStorage-backed dock store with `none`, then reset the
    // in-memory store so it re-reads from localStorage.
    localStorage.setItem("alo:right-dock", "none");
    __resetRightDockForTests();

    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );
    // The dock is now an ALWAYS-mounted collapsible slot — so the Group
    // never remounts on open/close (no left-pane flicker) and the slot can
    // animate. When closed it stays in the DOM but renders no content.
    const slot = screen.getByTestId("quadrant-spawner");
    expect(slot).toBeInTheDocument();
    expect(slot.childElementCount).toBe(0);
    // The other four stay mounted.
    expect(screen.getByTestId("quadrant-viewport")).toBeInTheDocument();
  });

  it("renders the Lighting pane in the shared right-dock slot when dock=lighting", () => {
    // The slot keeps the `quadrant-spawner` testid (its identity is the
    // right-dock, not the tool inside); the content is the Lighting pane,
    // and the centre column (curve editor) stays mounted alongside it.
    setDock("lighting");
    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );
    expect(screen.getByTestId("quadrant-spawner")).toBeInTheDocument();
    expect(
      screen.getByRole("dialog", { name: "Lighting" }),
    ).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-curve-editor")).toBeInTheDocument();
  });

  it("disables the dock separator AND panel while the dock is closed (drag-open guard)", () => {
    // Regression (2026-06-10): with the dock closed, the user could DRAG
    // the invisible dock splitter and open an empty slot. CSS
    // `pointer-events-none` on the Separator does NOT stop it — the
    // library hit-tests separators by document-level pointer
    // coordinates against their rects, not via DOM events on the
    // element. The supported switch is the `disabled` prop, on both the
    // Separator ("cannot be used to resize its neighboring panels") and
    // the Panel (else it can still be resized indirectly). The lib
    // surfaces these as aria-disabled / data-disabled.
    localStorage.setItem("alo:right-dock", "none");
    __resetRightDockForTests();

    const bridge = makeStubBridge();
    const { container } = render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );

    const separators = Array.from(container.querySelectorAll("[data-separator]"));
    expect(separators.length).toBeGreaterThan(0);
    const disabled = separators.filter(
      (s) => s.getAttribute("aria-disabled") === "true",
    );
    // Exactly one disabled separator: the dock's (the hidden vertical one).
    expect(disabled).toHaveLength(1);
    expect(disabled[0]!.className).toContain("invisible");

    const slotPanel = screen
      .getByTestId("quadrant-spawner")
      .closest("[data-panel]");
    expect(slotPanel).not.toBeNull();
    expect(slotPanel!.hasAttribute("data-disabled")).toBe(true);
  });

  it("keeps the dock separator and panel resizable while the dock is open", () => {
    // Default right-dock is "spawner" (lib/right-dock.ts) — open.
    const bridge = makeStubBridge();
    const { container } = render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );

    const separators = Array.from(container.querySelectorAll("[data-separator]"));
    expect(separators.length).toBeGreaterThan(0);
    expect(
      separators.filter((s) => s.getAttribute("aria-disabled") === "true"),
    ).toHaveLength(0);

    const slotPanel = screen
      .getByTestId("quadrant-spawner")
      .closest("[data-panel]");
    expect(slotPanel).not.toBeNull();
    expect(slotPanel!.hasAttribute("data-disabled")).toBe(false);
  });

  it("quadrant-viewport rect is the innermost wrapper (Modal portal target preservation)", () => {
    const bridge = makeStubBridge();
    render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );
    // Risk 2 from the plan: Modal.tsx does
    //   document.querySelector('[data-testid="quadrant-viewport"]')
    // and reads getBoundingClientRect on the result. The testID must
    // land on a div with `position: relative` (same semantics as today's
    // App.tsx:234) so positioned children (ViewportPill, tool panels,
    // Modal portal img) lay out correctly. We assert the testID node
    // is the `.relative h-full` wrapper, not a library-injected outer.
    const node = screen.getByTestId("quadrant-viewport");
    expect(node.className).toContain("relative");
  });
});
