// Vitest unit tests for AtlasPickerPanel.
// Verifies: grid cell count, selected-frame highlight, non-square header,
// and the five placeholder states (single-frame, no-texture, missing,
// too-large, off-index-channel).

import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { render, screen, waitFor, fireEvent, createEvent, act } from "@testing-library/react";
import { AtlasPickerPanel, __resetAtlasPropsCache } from "../AtlasPickerPanel";
import { publishAtlasContext, __resetAtlasContext } from "@/lib/atlas-context";
import { MockBridge } from "@/bridge/mock";
import type { Request } from "@particle-editor/bridge-schema";
import { useMockEmitterProperties } from "@/bridge/mock-state";
import { __resetPreviewCache, bumpTextureEpoch } from "@/lib/atlas-preview-cache";
import { __resetModStackForTests } from "@/lib/mod-stack";
import { useDockAnim } from "@/lib/dock-anim";

beforeEach(() => {
  __resetAtlasContext();
  useMockEmitterProperties.getState().reset();
  __resetPreviewCache();
  __resetModStackForTests();
  // The seeded-props cache is module-level; clear it so a textureSize/colorTexture
  // cached by one case can't leak into the next case's synchronous first render.
  __resetAtlasPropsCache();
  useDockAnim.setState({ atlasTerminalFirstPaint: false, atlasGridMounted: false });
});

afterEach(() => { vi.restoreAllMocks(); });

function setup(p?: { textureSize?: number; colorTexture?: string }) {
  useMockEmitterProperties.getState().patch(1, {
    textureSize: p?.textureSize ?? 16,
    colorTexture: p?.colorTexture ?? "fire.dds",
  });
  publishAtlasContext({
    emitterId: 1,
    focusedTrack: "index",
    interpolation: "step",
    selection: { frame: 5, keyTimes: [0.3] },
  });
  return render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
}

describe("AtlasPickerPanel", () => {
  it("renders side*side cells", async () => {
    setup({ textureSize: 16 });
    await waitFor(() =>
      expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16),
    );
  });

  it("highlights the selected frame", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => {
      const cells = screen.getAllByTestId("atlas-cell");
      const five = cells.find((c) => c.getAttribute("data-frame") === "5")!;
      expect(five.getAttribute("data-selected")).toBe("true");
    });
  });

  it("non-square header 'M of N'", async () => {
    setup({ textureSize: 20 });
    await waitFor(() =>
      expect(screen.getByTestId("atlas-meta").textContent).toContain("16 of 20"),
    );
  });

  it("single-frame placeholder", async () => {
    setup({ textureSize: 1 });
    await waitFor(() => expect(screen.getByText(/single frame/i)).toBeTruthy());
  });

  it("no-texture placeholder", async () => {
    setup({ textureSize: 16, colorTexture: "" });
    await waitFor(() =>
      expect(screen.getByText(/no color texture/i)).toBeTruthy(),
    );
  });

  it("missing placeholder", async () => {
    setup({ textureSize: 16, colorTexture: "__missing__.dds" });
    await waitFor(() => expect(screen.getByText(/not found/i)).toBeTruthy());
  });

  it("too-large placeholder", async () => {
    setup({ textureSize: 1_000_000 });
    await waitFor(() => expect(screen.getByText(/too large/i)).toBeTruthy());
  });

  it("inert off the index channel", async () => {
    setup({ textureSize: 16 });
    publishAtlasContext({
      emitterId: 1,
      focusedTrack: "scale",
      interpolation: "step",
      selection: { frame: null, keyTimes: [] },
    });
    await waitFor(() =>
      expect(
        screen.getByText(/select keys on the index channel/i),
      ).toBeTruthy(),
    );
  });

  it("broken placeholder", async () => {
    setup({ textureSize: 16, colorTexture: "__broken__.dds" });
    await waitFor(() => expect(screen.getByText(/could not be read/i)).toBeTruthy());
  });

  it("out-of-range frame: no highlight + off-grid note", async () => {
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({
      emitterId: 1,
      focusedTrack: "index",
      interpolation: "step",
      selection: { frame: 99, keyTimes: [0.3] },
    });
    render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => {
      const cells = screen.getAllByTestId("atlas-cell");
      expect(cells.some((c) => c.getAttribute("data-selected") === "true")).toBe(false);
      expect(
        screen.getByText(/frame 99 — outside the 4×4 atlas \(in-game sampling is off-grid\)/i),
      ).toBeTruthy();
    });
  });

  it("grid container is a labelled listbox with fixed-px column tracks", async () => {
    setup({ textureSize: 16 }); // 16 cells (side = 4)
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(grid).toBeTruthy();
    // The grid reflows responsively to the measured width via fitGridLayout. In
    // jsdom `clientWidth` is 0, so the ResizeObserver never updates `gridW` and it
    // stays at the init default COLD_START_GRIDW (215 — the deterministic dock-min
    // content width) → fitGridLayout(16, 215, 4, 44, 160) = 4 columns × 50px cells.
    // Tracks are FIXED-px (not 1fr) so the grid is a static block the dock's
    // overflow:hidden clips as it widens (no live reflow).
    expect(grid.style.gridTemplateColumns).toMatch(/^repeat\(\d+, \d+px\)$/);
    expect(grid.style.gridTemplateColumns).toBe("repeat(4, 50px)");
  });

  it("crop is side-driven and references the shared --atlas-url", async () => {
    setup({ textureSize: 16 }); // side = 4
    // wait for the preview to load so the crop style is applied
    const cell = await waitFor(() => {
      const c = screen.getAllByTestId("atlas-cell").find((x) => x.getAttribute("data-frame") === "6")!;
      expect(c.style.backgroundImage).not.toBe("");
      return c;
    });
    // The cell paints the SHARED atlas image (a CSS var on the container), not an
    // embedded dataUri — so an alpha toggle swaps the var, not the cell.
    expect(cell.style.backgroundImage).toBe("var(--atlas-url)");
    // backgroundSize is side*100% (4 -> "400% 400%") — the crop is side-driven.
    expect(cell.style.backgroundSize).toBe("400% 400%");
  });

  it("cells are options with preserved test hooks and aria-selected", async () => {
    setup({ textureSize: 16 }); // assigned frame = 5
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    const cells = screen.getAllByTestId("atlas-cell");
    const five = cells.find((c) => c.getAttribute("data-frame") === "5")!;
    // DOM-hook preservation: same element keeps data-* and gains role/aria
    expect(five.getAttribute("role")).toBe("option");
    expect(five.getAttribute("data-selected")).toBe("true");
    expect(five.getAttribute("aria-selected")).toBe("true");
    expect(five.getAttribute("aria-label")).toMatch(/frame 5/i);
    // roving tabindex: the assigned frame is the single tabbable cell
    expect(five.getAttribute("tabindex")).toBe("0");
    const other = cells.find((c) => c.getAttribute("data-frame") === "0")!;
    expect(other.getAttribute("tabindex")).toBe("-1");
  });

  it("shows an always-visible frame-index badge on every cell", async () => {
    setup({ textureSize: 16 }); // 16 cells, assigned frame 5
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    for (const c of screen.getAllByTestId("atlas-cell")) {
      const k = c.getAttribute("data-frame")!;
      const badge = c.querySelector('[data-testid="atlas-cell-badge"]')!;
      expect(badge.textContent).toBe(k); // labels its atlas index…
      expect(badge.className).not.toContain("opacity-0"); // …and is NOT hover-gated
    }
  });

  it("centres the grid tracks horizontally (justify-center)", async () => {
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Fixed-px tracks would left-pack in the full-width grid box; justify-center
    // centres them so the grid sits centred in the panel.
    expect(grid.className).toContain("justify-center");
  });

  it("resets the roving cursor when switching to a smaller atlas (stale-index)", async () => {
    // start on an 8x8 (64-cell) atlas, assigned frame 50
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step",
      selection: { frame: 50, keyTimes: [0.3] } });
    const view = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64));
    // switch to a 4x4 (16-cell) atlas, assigned frame 3
    useMockEmitterProperties.getState().patch(2, { textureSize: 16, colorTexture: "smoke.dds" });
    publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step",
      selection: { frame: 3, keyTimes: [0.3] } });
    view.rerender(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    const cells = screen.getAllByTestId("atlas-cell");
    // exactly one tabbable cell, and it is in range (the new assigned frame 3)
    const tabbable = cells.filter((c) => c.getAttribute("tabindex") === "0");
    expect(tabbable).toHaveLength(1);
    expect(tabbable[0].getAttribute("data-frame")).toBe("3");
  });

  it("arrow keys move the roving cursor geometrically (side columns)", async () => {
    setup({ textureSize: 16 }); // side = 4 columns, assigned 5 -> cursor starts at 5
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    const tabbed = () => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("tabindex") === "0")!.getAttribute("data-frame");
    // The listbox renders while the preview is still loading (no cells yet); wait for
    // the cells so the assigned-frame cursor has settled before firing keys — else the
    // reset effect can clobber an early keypress (full-suite flake: '6' vs '1').
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 5 -> 6
    await waitFor(() => expect(tabbed()).toBe("6"));
    fireEvent.keyDown(grid, { key: "ArrowDown" });  // 6 -> 10 (+4)
    await waitFor(() => expect(tabbed()).toBe("10"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });    // 10 -> 6
    await waitFor(() => expect(tabbed()).toBe("6"));
    fireEvent.keyDown(grid, { key: "Home" });       // -> 0
    await waitFor(() => expect(tabbed()).toBe("0"));
    fireEvent.keyDown(grid, { key: "End" });        // -> 15
    await waitFor(() => expect(tabbed()).toBe("15"));
  });

  it("ArrowDown is a no-op into a missing target past the last row; down-then-up reversible", async () => {
    setup({ textureSize: 16 }); // side = 4, 16 cells, rows: 0-3,4-7,8-11,12-15
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    const tabbed = () => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("tabindex") === "0")!.getAttribute("data-frame");
    // Wait for the cells (preview resolved → reset effect settled) before firing keys,
    // else the reset effect can clobber an early keypress (full-suite flake: '6' vs '1').
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    fireEvent.keyDown(grid, { key: "Home" }); // 0
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 1
    await waitFor(() => expect(tabbed()).toBe("1"));
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 1 -> 5
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 5 -> 9
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 9 -> 13
    await waitFor(() => expect(tabbed()).toBe("13"));
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 13 -> 17? past last cell -> no-op, stays 13
    await waitFor(() => expect(tabbed()).toBe("13"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });   // 13 -> 9 (reversible)
    await waitFor(() => expect(tabbed()).toBe("9"));
  });

  it("Enter assigns the focused frame; Space too and prevents default", async () => {
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Wait for the cells (preview resolved → reset effect settled) before firing keys,
    // else the reset effect can clobber Home and Enter assigns the wrong frame.
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    fireEvent.keyDown(grid, { key: "Home" });   // focus 0
    fireEvent.keyDown(grid, { key: "Enter" });  // assign 0
    await waitFor(() => expect(req).toHaveBeenCalledWith(expect.objectContaining({
      kind: "emitters/set-track-key",
      params: expect.objectContaining({ newValue: 0, oldTime: 0.3 }),
    })));
    const ev = createEvent.keyDown(grid, { key: " " });
    fireEvent(grid, ev);
    expect(ev.defaultPrevented).toBe(true); // Space must not scroll the panel
  });

  it("hero shows the assigned frame number and reflects keyboard focus", async () => {
    setup({ textureSize: 16 }); // assigned 5
    // assigned frame number is visible in the hero caption
    await waitFor(() => expect(screen.getByTestId("atlas-hero").textContent).toMatch(/frame 5/i));
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    fireEvent.keyDown(grid, { key: "Home" }); // focus 0 -> hero follows focus
    await waitFor(() => expect(screen.getByTestId("atlas-hero").textContent).toMatch(/frame 0/i));
  });

  it("has an aria-live region", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => expect(document.querySelector('[aria-live="polite"]')).toBeTruthy());
  });

  it("reserves a symmetric scrollbar gutter (anti-flicker + centring)", async () => {
    // Presence guard only — proves the mitigation is wired; the actual loop
    // and the centring need real layout (a Playwright check) to observe.
    // `both-edges` keeps the width stable AND centres the grid in the panel (a
    // one-sided gutter pushes the centred tracks off-centre vs the hero).
    setup({ textureSize: 16 });
    await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(screen.getByTestId("atlas-scroll").style.scrollbarGutter).toBe("stable both-edges");
  });

  it("column count tracks the atlas via fitGridLayout at the (slide-frozen) width", async () => {
    // The grid reflows from the measured width via fitGridLayout. In jsdom
    // `clientWidth` is 0, so the ResizeObserver never updates `gridW` and it holds
    // the init default COLD_START_GRIDW (215). At that narrow cold-start width the
    // 44px min-cell floor caps columns at 4 (maxColsForMin = floor((215+4)/48) = 4),
    // so the column count is driven by the atlas's √n ideal under that cap: a
    // 16-cell atlas → min(16,4,4)=4 cols, a 4-cell atlas → min(4,2,4)=2 cols. The
    // count follows the layout, never a 1-column mid-slide transient (the slide
    // freezes the measure → no snap). (Wider docks showing MORE columns is the
    // responsive width path, exercised by the real-layout Playwright check.)
    const bridge = new MockBridge();
    const handlers: Array<(e: unknown) => void> = [];
    const realOn = bridge.on.bind(bridge);
    vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
      if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
      return realOn(kind, cb as never);
    });
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // 16 cells @ w=215 → 4 cols × 50px (fitGridLayout(16,215,4,44,160)); fixed-px.
    expect(grid.style.gridTemplateColumns).toBe("repeat(4, 50px)");
    // Swap to a 4-cell (2×2) atlas in place — the column count drops to the √n
    // ideal (2) and the cell grows to fill (fitGridLayout(4,215,4,44,160) → 2 cols
    // × 105px), never a single-column snap.
    useMockEmitterProperties.getState().patch(1, { textureSize: 4, colorTexture: "smoke.dds" });
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() =>
      expect(screen.getByRole("listbox", { name: /atlas frames/i }).style.gridTemplateColumns).toBe("repeat(2, 105px)"),
    );
  });

  it("refreshes the atlas when the emitter's texture changes (tree/changed)", async () => {
    const bridge = new MockBridge();
    // capture the panel's emitters/tree/changed handler so we can fire it
    const handlers: Array<(e: unknown) => void> = [];
    const realOn = bridge.on.bind(bridge);
    vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
      if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
      return realOn(kind, cb as never);
    });
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16)); // 4×4
    // swap the emitter's texture to a larger atlas, then signal the change
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "smoke.dds" });
    expect(handlers.length).toBeGreaterThan(0); // panel subscribed
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64)); // 8×8, re-fetched
  });

  it("re-fetches the preview when the texture epoch bumps (reload textures)", async () => {
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    const previewCalls = () => req.mock.calls.filter(([r]) => (r as { kind: string }).kind === "textures/get-preview").length;
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [33] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await waitFor(() => expect(previewCalls()).toBe(2)); // both alpha modes prefetched (then cached)
    act(() => { bumpTextureEpoch(); }); // simulate a "reload textures"
    await waitFor(() => expect(previewCalls()).toBe(4)); // cache dropped → both modes re-fetch fresh content
  });

  it("keeps the loaded grid mounted across same-input parent rerenders", async () => {
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [33] } });
    const firstBridge = new MockBridge();
    const v = render(<AtlasPickerPanel bridge={firstBridge} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));

    v.rerender(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);

    expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16);
    expect(screen.getByTestId("atlas-hero").textContent).toMatch(/frame 5/i);
  });


  // The manual Alpha toggle + its localStorage persistence were removed — the
  // emitter's blend mode (blendAlphaGated) now drives the preview. These tests
  // assert BOTH modes still prefetch and that blendAlphaGated selects the active
  // (first-fetched, displayed) mode. (The mock returns the same PNG for both
  // flatten modes, so the active/first fetch is the observable proxy for "which
  // preview is shown".)
  const previewFlattenParams = (calls: ReadonlyArray<readonly unknown[]>) =>
    calls
      .map((c) => c[0] as { kind: string; params: { flattenAlpha?: boolean } })
      .filter((r) => r.kind === "textures/get-preview")
      .map((r) => r.params.flattenAlpha);

  it("prefetches BOTH alpha modes upfront; blendAlphaGated=false → flat is the active fetch; no toggle", async () => {
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: false });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    // Both modes fetch upfront (color + real-alpha), order-independent → sort.
    await waitFor(() => expect([...previewFlattenParams(req.mock.calls)].sort()).toEqual([false, true]));
    // Not-alpha-gated → the FLAT (color) preview is active → fetched first.
    expect(previewFlattenParams(req.mock.calls)[0]).toBe(true);
    // The manual Alpha toggle is gone.
    expect(screen.queryByTestId("atlas-alpha-toggle")).toBeNull();
  });

  it("blendAlphaGated=true → the real-alpha preview is the active (first) fetch", async () => {
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: true });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    // Alpha-gated → the real-alpha preview (flattenAlpha=false) is active → first.
    await waitFor(() => expect(previewFlattenParams(req.mock.calls)[0]).toBe(false));
  });

  it("a gating-only blend-mode flip issues NO new preview fetch (both modes already cached)", async () => {
    // Guards the perf invariant the old toggle test protected: a same-texture
    // blend-mode change refetches emitter-properties but must NOT re-fetch previews
    // (both alpha modes are already in hand → the swap is synchronous).
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    const handlers: Array<(e: unknown) => void> = [];
    const realOn = bridge.on.bind(bridge);
    vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
      if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
      return realOn(kind, cb as never);
    });
    const previewCount = () =>
      req.mock.calls.filter(([r]) => (r as { kind: string }).kind === "textures/get-preview").length;
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: false });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await waitFor(() => expect(previewCount()).toBe(2)); // both modes prefetched
    // Flip ONLY the blend classification (same texture) via a tree/changed refetch.
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: true });
    act(() => { handlers.forEach((h) => h({ kind: "emitters/tree/changed" })); });
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    expect(previewCount()).toBe(2); // unchanged — no redundant fetch
  });

  it("renders the full grid unconditionally (no dock-slide deferral)", async () => {
    // The grid mounts immediately rather than deferring behind the slide — it
    // seeds `gridW` from the cached/default width and reflows once measured, so
    // the full grid + hero are present from the first frame (no gate). The hero
    // shows too.
    setup({ textureSize: 16 });
    await screen.findByTestId("atlas-meta"); // header is up
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16)); // full grid mounted
    expect(screen.queryByTestId("atlas-scroll")).not.toBeNull();   // scroll region present
    expect(screen.queryByTestId("atlas-hero")).not.toBeNull();     // hero preview present
  });

  it("backing is contained: scroll container never carries bg-black", async () => {
    // The dark backing now lives on each cell/hero (cropStyle), not on the
    // panel containers — so the scroll region must not slab a black surface in
    // either alpha mode.
    setup({ textureSize: 16 });
    const scroll = await screen.findByTestId("atlas-scroll");
    expect(scroll.className).not.toContain("bg-black");
    // The backing is class-based and mode-independent — the dedicated
    // "alpha-gated mode" test below covers the gated render.
  });

  it("color mode (default): container backing is the uniform gray (bg-bg-2); cells are cheap", async () => {
    setup({ textureSize: 16 });
    // wait for the preview to load so the crop style + container var are applied
    const cell = await waitFor(() => {
      const c = screen.getAllByTestId("atlas-cell").find((x) => x.getAttribute("data-frame") === "6")!;
      expect(c.style.backgroundImage).not.toBe("");
      return c;
    });
    // The backing is now the SAME gray as the cells (bg-bg-2), applied via class —
    // no inline per-mode background, no checkerboard.
    const grid = screen.getByRole("listbox", { name: /atlas frames/i });
    expect(grid.className).toContain("bg-bg-2");
    expect(grid.style.background).toBe("");
    expect(grid.style.backgroundImage).toBe("");
    // The shared atlas image is exposed on the container as --atlas-url.
    expect(grid.style.getPropertyValue("--atlas-url")).toMatch(/^url\(/);
    // Cells are cheap + mode-independent: they reference the shared var, carry NO
    // inline background-color and NO per-cell checker layers (the gray is bg-bg-2).
    expect(cell.style.backgroundImage).toBe("var(--atlas-url)");
    expect(cell.style.backgroundColor).toBe("");
    expect(cell.className).toContain("bg-bg-2");
    expect(cell.style.backgroundImage).not.toContain("var(--atlas-checker-");
    // the hero is now a <canvas> crop (no CSS background raster) — assert the
    // canvas is present rather than a background token. The crop is painted into
    // the canvas; transparent pixels reveal the hero element's bg-bg-2 (a no-op
    // under jsdom's contextless canvas).
    const hero = screen.getByTestId("atlas-hero");
    expect(hero.querySelector("canvas")).not.toBeNull();
  });

  it("gives cells a hover lift affordance (with a reduced-motion opt-out)", async () => {
    setup({ textureSize: 16 });
    const cell = await waitFor(() => {
      const c = screen.getAllByTestId("atlas-cell").find((x) => x.getAttribute("data-frame") === "6")!;
      expect(c.style.backgroundImage).not.toBe("");
      return c;
    });
    expect(cell.className).toContain("hover:scale-[1.06]");
    expect(cell.className).toContain("motion-reduce:hover:scale-100");
  });

  it("hover moves do not re-render the atlas grid", async () => {
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));

    const renderAttr = grid.getAttribute("data-render-count");
    expect(renderAttr).not.toBeNull();
    const before = Number(renderAttr);
    expect(before).toBeGreaterThan(0);

    const cells = screen.getAllByTestId("atlas-cell");
    for (const k of [1, 2, 3, 4, 5, 6]) {
      fireEvent.mouseEnter(cells[k]);
      fireEvent.mouseLeave(cells[k]);
    }

    expect(Number(grid.getAttribute("data-render-count"))).toBe(before);
  });

  it("alpha-gated mode: the CONTAINER stays the uniform gray (no checkerboard); cells unchanged", async () => {
    // Render an alpha-gated emitter directly (the mode that used to require the
    // Alpha toggle ON). The container must stay uniform bg-bg-2 gray — no checkerboard.
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: true });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    const cell = await waitFor(() => {
      const c = screen.getAllByTestId("atlas-cell").find((x) => x.getAttribute("data-frame") === "6")!;
      expect(c.style.backgroundImage).not.toBe("");
      return c;
    });
    const grid = screen.getByRole("listbox", { name: /atlas frames/i });
    expect(grid.className).toContain("bg-bg-2");
    expect(grid.style.backgroundImage).not.toContain("var(--atlas-checker-");
    expect(grid.style.backgroundColor).not.toBe("var(--atlas-checker-l)");
    // The cell crop is mode-independent: just the shared var crop, no checker.
    expect(cell.style.backgroundImage).toBe("var(--atlas-url)");
    expect(cell.style.backgroundImage).not.toContain("var(--atlas-checker-");
  });

  it("keeps keyboard focus in the grid when the atlas shrinks under it", async () => {
    const bridge = new MockBridge();
    const handlers: Array<(e: unknown) => void> = [];
    const realOn = bridge.on.bind(bridge);
    vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
      if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
      return realOn(kind, cb as never);
    });
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [33] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // The cells render only after the preview prefetch resolves; press End only
    // once they exist, else focusCell(63) finds no element and focus never lands
    // (a pre-existing race — the keydown fires once and isn't retried by waitFor).
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64));
    fireEvent.keyDown(grid, { key: "End" }); // focus the last cell (63)
    await waitFor(() =>
      expect((document.activeElement as HTMLElement | null)?.getAttribute("data-frame")).toBe("63"),
    );
    // shrink to a 2×2 atlas — cell 63 unmounts
    useMockEmitterProperties.getState().patch(1, { textureSize: 4, colorTexture: "smoke.dds" });
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(4));
    // focus did NOT fall to <body> — it was re-homed into the grid. The re-home
    // runs in a post-shrink effect that lands a tick AFTER the 4-cell render, so
    // poll for it via waitFor rather than sampling activeElement once (CI flake:
    // null vs 'atlas-cell' when the effect hasn't settled).
    await waitFor(() =>
      expect((document.activeElement as HTMLElement | null)?.getAttribute("data-testid")).toBe("atlas-cell"),
    );
  });

  // Reject every request whose kind/params match — delegate the rest to the real
  // mock. The matcher is typed against the real `Request` union (not a loose
  // `{kind:string}`) so a schema rename/widening fails to COMPILE here instead of
  // silently never matching. The `as never` on the delegate + the outer cast only
  // bridge the impl to the generic `request<R>` spy shape (passing the union to the
  // generic directly trips TS2321 "excessive stack depth" under `tsc -b`); neither
  // weakens the matcher's kind-checking.
  function rejectMatching(bridge: MockBridge, match: (r: Request) => boolean) {
    const real = bridge.request.bind(bridge);
    vi.spyOn(bridge, "request").mockImplementation(((req: Request) =>
      match(req) ? Promise.reject(new Error("test-induced failure")) : real(req as never)) as typeof bridge.request);
  }

  it("announces a FAILED assign to the live region (no silent failure)", async () => {
    const bridge = new MockBridge();
    rejectMatching(bridge, (r) => r.kind === "emitters/set-track-key"); // host rejects the write
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Wait for the cells before firing keys: the listbox renders during
    // preview-load with no cells yet, and Home would race the cursor-init
    // effect (which seeds focus at the context frame, 5) — leaving focus at 5
    // and Enter assigning frame 5 instead of 0.
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    fireEvent.keyDown(grid, { key: "Home" });  // focus frame 0
    fireEvent.keyDown(grid, { key: "Enter" }); // assign → set-track-key rejects → commit fails
    // The aria-live region announces the failure (parity with the success path),
    // rather than going silent for a screen-reader / keyboard user.
    await waitFor(() =>
      expect(document.querySelector('[aria-live="polite"]')!.textContent).toMatch(/could not assign frame 0/i),
    );
  });

  it("re-open seeds the grid synchronously from the cache for the SAME emitter", async () => {
    // First mount populates the module-level lastEmitterProps for emitter 1 (8×8).
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    const v1 = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64));
    v1.unmount();

    // Re-open the SAME emitter with get-properties BLOCKED: the grid must still seed
    // 64 cells from lastEmitterProps (preview is served from the still-warm cache),
    // not wait on the round-trip or fall back to the single-frame default.
    const blocked = new MockBridge();
    const real = blocked.request.bind(blocked);
    const reqSpy = vi.spyOn(blocked, "request").mockImplementation(((req: Request) =>
      req.kind === "emitters/get-properties"
        ? new Promise<never>(() => {}) // never resolves
        : real(req as never)) as typeof blocked.request);
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={blocked} onClose={() => {}} />);
    // Seeded from lastEmitterProps (id matches) → grid paints without waiting on the
    // round-trip…
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64));
    // …and the confirming get-properties fetch STILL fires (the seed is a head-start,
    // not a replacement — guards a regression that seeds and never re-validates).
    expect(reqSpy).toHaveBeenCalledWith(expect.objectContaining({ kind: "emitters/get-properties" }));
  });

  it("re-open does NOT show stale frames for a DIFFERENT emitter (id-match guard)", async () => {
    // Warm the cache for emitter 1 (8×8).
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    const v1 = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(64));
    v1.unmount();

    // Re-open a DIFFERENT emitter (id 2) with get-properties BLOCKED. The cache is
    // keyed to id 1, so the seed must NOT apply → defaults (no colorTexture) → the
    // "No color texture set" placeholder, never emitter 1's 64 stale cells.
    const blocked = new MockBridge();
    const real = blocked.request.bind(blocked);
    vi.spyOn(blocked, "request").mockImplementation(((req: Request) =>
      req.kind === "emitters/get-properties"
        ? new Promise<never>(() => {})
        : real(req as never)) as typeof blocked.request);
    publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={blocked} onClose={() => {}} />);
    await waitFor(() => expect(screen.getByText(/no color texture set/i)).toBeTruthy());
    expect(screen.queryAllByTestId("atlas-cell")).toHaveLength(0); // no stale emitter-1 grid
  });

  it("surfaces an INACTIVE-mode preview failure when the blend mode switches into it (not silent)", async () => {
    // Not-alpha-gated → active mode is color (flattenAlpha=true). Fail the INACTIVE
    // alpha mode (flattenAlpha=false) — its failure is invisible until the blend
    // mode makes it active (via an emitters/tree/changed refetch).
    const bridge = new MockBridge();
    const handlers: Array<(e: unknown) => void> = [];
    const realOn = bridge.on.bind(bridge);
    vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
      if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
      return realOn(kind, cb as never);
    });
    rejectMatching(bridge, (r) => r.kind === "textures/get-preview" && r.params.flattenAlpha === false);
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: false });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    // active (color) mode renders fine…
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    // …the emitter becomes alpha-gated → the failed alpha mode becomes active → broken.
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: true });
    act(() => { handlers.forEach((h) => h({ kind: "emitters/tree/changed" })); });
    await waitFor(() => expect(screen.getByText(/could not be read/i)).toBeTruthy());
  });

  // ── readiness split (perf-audit P1b) ──────────────────────────────────────
  it("a placeholder (no-texture) releases the dock gate via atlasTerminalFirstPaint without mounting the grid", async () => {
    setup({ textureSize: 16, colorTexture: "" });
    // terminal-first-paint fires on the placeholder so the dock can slide promptly…
    await waitFor(() => expect(useDockAnim.getState().atlasTerminalFirstPaint).toBe(true));
    // …but the grid is NOT mounted (no "ok" preview), so the two signals are distinct.
    expect(useDockAnim.getState().atlasGridMounted).toBe(false);
  });

  it("a full grid sets BOTH atlasGridMounted and atlasTerminalFirstPaint", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => expect(useDockAnim.getState().atlasGridMounted).toBe(true));
    expect(useDockAnim.getState().atlasTerminalFirstPaint).toBe(true);
  });

  it("defers the INACTIVE alpha-mode preview to idle; the active mode fetches synchronously", async () => {
    // Override the global synchronous rIC stub with a controllable queue so we can
    // observe the inactive mode's deferral (perf-audit P1b).
    const idleQ: Array<() => void> = [];
    const origR = globalThis.requestIdleCallback;
    const origC = globalThis.cancelIdleCallback;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (globalThis as any).requestIdleCallback = (cb: () => void) => { idleQ.push(cb); return idleQ.length; };
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (globalThis as any).cancelIdleCallback = (id: number) => { idleQ[id - 1] = () => {}; };
    try {
      useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
      publishAtlasContext({
        emitterId: 1, focusedTrack: "index", interpolation: "step",
        selection: { frame: 5, keyTimes: [0.3] },
      });
      const bridge = new MockBridge();
      const spy = vi.spyOn(bridge, "request");
      const previewFetches = () =>
        spy.mock.calls.filter((c) => (c[0] as Request).kind === "textures/get-preview").length;
      render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
      // Active mode fetched synchronously; the inactive mode is queued, not yet fetched.
      await waitFor(() => expect(previewFetches()).toBe(1));
      // Flush idle → the inactive mode now fetches.
      await act(async () => { idleQ.splice(0).forEach((fn) => fn()); });
      await waitFor(() => expect(previewFetches()).toBe(2));
    } finally {
      globalThis.requestIdleCallback = origR;
      globalThis.cancelIdleCallback = origC;
    }
  });
});
