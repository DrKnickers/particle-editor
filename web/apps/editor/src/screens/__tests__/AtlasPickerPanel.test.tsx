// Vitest unit tests for AtlasPickerPanel.
//
// [#572] The frame grid is ONE <canvas> (role="listbox") — there are no
// per-frame DOM cells anymore. jsdom has no canvas, so these tests can't inspect
// pixels; they verify the a11y layer instead: the listbox + its single active
// option (referenced by aria-activedescendant), aria-setsize/-selected/-disabled,
// the grid-box dimensions, keyboard nav (arrows/Home/End move the active
// descendant), and click/hover via pointer math against the canvas
// (getBoundingClientRect is all-zeros in jsdom, so clientX/clientY are grid-local
// offsets). Cell geometry in jsdom: gridW holds COLD_START_GRIDW=215, so a 16-cell
// atlas lays out as 4 cols × 50px cells (step = cell 50 + GRID_GAP 4 = 54).

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

// ── canvas-grid helpers ──────────────────────────────────────────────────────
const listbox = () => screen.getByRole("listbox", { name: /atlas frames/i });
const activeDesc = () => listbox().getAttribute("aria-activedescendant");
const activeOption = () => screen.getByTestId("atlas-active-option");
const gridBox = () => screen.getByTestId("atlas-grid-box");

// The grid center of frame k in jsdom geometry (4 cols × 50px cells, gap 4).
function frameCenter(k: number, cols = 4, cell = 50): { clientX: number; clientY: number } {
  const step = cell + 4; // GRID_GAP
  return { clientX: (k % cols) * step + cell / 2, clientY: Math.floor(k / cols) * step + cell / 2 };
}
const moveToFrame = (k: number, cols = 4, cell = 50) => fireEvent.mouseMove(listbox(), frameCenter(k, cols, cell));

describe("AtlasPickerPanel", () => {
  it("advertises side*side frames via aria-setsize on the active option", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("16"));
  });

  it("highlights the selected frame (active option aria-selected + aria-activedescendant)", async () => {
    setup({ textureSize: 16 }); // assigned frame 5
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
    const opt = activeOption();
    expect(opt.getAttribute("aria-selected")).toBe("true");
    expect(opt.getAttribute("aria-label")).toMatch(/frame 5/i);
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
      // frame 99 is off the 4×4 grid → nothing selected; the roving target
      // falls back to frame 0, whose option is NOT selected.
      expect(activeOption().getAttribute("aria-selected")).toBe("false");
      expect(
        screen.getByText(/frame 99 — outside the 4×4 atlas \(in-game sampling is off-grid\)/i),
      ).toBeTruthy();
    });
  });

  it("grid is a single canvas listbox sizing a fixed-px box", async () => {
    setup({ textureSize: 16 }); // 16 cells (side = 4)
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(grid.tagName).toBe("CANVAS");
    // In jsdom `clientWidth` is 0, so the ResizeObserver never updates `gridW`
    // and it stays at COLD_START_GRIDW (215) → fitGridLayout(16, 215, 4, 44, 160)
    // = 4 columns × 50px cells. Box width = 4*50 + 3*gap(4) = 212px; rows = 4 →
    // height = 4*50 + 3*4 = 212px. The <canvas> fills the box.
    expect(gridBox().style.width).toBe("212px");
    expect(gridBox().style.height).toBe("212px");
    // The canvas draws directly from the full-res preview — no shared CSS raster.
    expect(grid.style.getPropertyValue("--atlas-url")).toBe("");
    expect(grid.style.backgroundImage).toBe("");
  });

  it("exposes a single active a11y option (aria hooks preserved on the roving cell)", async () => {
    setup({ textureSize: 16 }); // assigned frame = 5 → roving target 5
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
    const opt = activeOption();
    expect(opt.getAttribute("role")).toBe("option");
    expect(opt.getAttribute("id")).toBe("atlas-opt-5");
    expect(opt.getAttribute("aria-selected")).toBe("true");
    expect(opt.getAttribute("aria-label")).toMatch(/frame 5/i);
    expect(opt.getAttribute("aria-posinset")).toBe("6"); // frame 5 → position 6
    expect(opt.getAttribute("aria-setsize")).toBe("16");
    // the listbox owns + points at that option
    expect(listbox().getAttribute("aria-owns")).toBe("atlas-opt-5");
  });

  it("names the roving frame index in the active option's label (updates as you navigate)", async () => {
    // Replaces the old per-cell always-visible index badge (now drawn on the
    // canvas): the active option announces "Frame N" for whichever cell is roving.
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
    expect(activeOption().getAttribute("aria-label")).toMatch(/^frame 5$/i);
    fireEvent.keyDown(grid, { key: "Home" }); // → frame 0
    await waitFor(() => expect(activeOption().getAttribute("aria-label")).toMatch(/^frame 0$/i));
  });

  it("centres the grid box horizontally (mx-auto)", async () => {
    setup({ textureSize: 16 });
    await screen.findByRole("listbox", { name: /atlas frames/i });
    // The fixed-width box is centred in the scroll panel via margin auto.
    expect(gridBox().className).toContain("mx-auto");
  });

  it("resets the roving cursor when switching to a smaller atlas (stale-index)", async () => {
    // start on an 8x8 (64-cell) atlas, assigned frame 50
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step",
      selection: { frame: 50, keyTimes: [0.3] } });
    const view = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("64"));
    // switch to a 4x4 (16-cell) atlas, assigned frame 3
    useMockEmitterProperties.getState().patch(2, { textureSize: 16, colorTexture: "smoke.dds" });
    publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step",
      selection: { frame: 3, keyTimes: [0.3] } });
    view.rerender(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    // roving target resets to the new in-range assigned frame (3), never a stale 50
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-3"));
    const opt = activeOption();
    expect(opt.getAttribute("aria-setsize")).toBe("16");
    expect(opt.getAttribute("aria-selected")).toBe("true");
  });

  it("arrow keys move the roving cursor geometrically (layout columns)", async () => {
    setup({ textureSize: 16 }); // 4 columns, assigned 5 -> cursor starts at 5
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Wait for the cursor to settle at the assigned frame before firing keys —
    // else the reset effect can clobber an early keypress.
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 5 -> 6
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-6"));
    fireEvent.keyDown(grid, { key: "ArrowDown" });  // 6 -> 10 (+4)
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-10"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });    // 10 -> 6
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-6"));
    fireEvent.keyDown(grid, { key: "Home" });       // -> 0
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-0"));
    fireEvent.keyDown(grid, { key: "End" });        // -> 15
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-15"));
  });

  it("ArrowDown is a no-op into a missing target past the last row; down-then-up reversible", async () => {
    setup({ textureSize: 16 }); // 4 cols, 16 cells, rows: 0-3,4-7,8-11,12-15
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
    fireEvent.keyDown(grid, { key: "Home" }); // 0
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 1
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-1"));
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 1 -> 5
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 5 -> 9
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 9 -> 13
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-13"));
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 13 -> 17? past last cell -> no-op, stays 13
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-13"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });   // 13 -> 9 (reversible)
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-9"));
  });

  it("Enter assigns the focused frame; Space too and prevents default", async () => {
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Wait for the cursor to settle at the assigned frame before firing keys.
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5"));
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
    setup({ textureSize: 16 });
    await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(screen.getByTestId("atlas-scroll").style.scrollbarGutter).toBe("stable both-edges");
  });

  it("column count tracks the atlas via fitGridLayout at the (slide-frozen) width", async () => {
    // The grid reflows from the measured width via fitGridLayout. In jsdom
    // `clientWidth` is 0, so gridW holds COLD_START_GRIDW (215). The grid-box
    // width encodes the column count.
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
    await screen.findByRole("listbox", { name: /atlas frames/i });
    // 16 cells @ w=215 → 4 cols × 50px → box width 4*50 + 3*gap(4) = 212px.
    expect(gridBox().style.width).toBe("212px");
    // Swap to a 4-cell (2×2) atlas in place — the column count drops to the √n
    // ideal (2) and the cell grows to fill (2 cols × 105px). Box width = 214px.
    useMockEmitterProperties.getState().patch(1, { textureSize: 4, colorTexture: "smoke.dds" });
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(gridBox().style.width).toBe("214px"));
  });

  it("refreshes the atlas when the emitter's texture changes (tree/changed)", async () => {
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
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("16")); // 4×4
    // swap the emitter's texture to a larger atlas, then signal the change
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "smoke.dds" });
    expect(handlers.length).toBeGreaterThan(0); // panel subscribed
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("64")); // 8×8, re-fetched
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
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("16"));

    v.rerender(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);

    expect(activeOption().getAttribute("aria-setsize")).toBe("16");
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
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("16"));
    expect(previewCount()).toBe(2); // unchanged — no redundant fetch
  });

  it("renders the full grid unconditionally (no dock-slide deferral)", async () => {
    setup({ textureSize: 16 });
    await screen.findByTestId("atlas-meta"); // header is up
    await screen.findByRole("listbox", { name: /atlas frames/i }); // canvas grid mounted
    expect(screen.queryByTestId("atlas-scroll")).not.toBeNull();   // scroll region present
    expect(screen.queryByTestId("atlas-hero")).not.toBeNull();     // hero preview present
  });

  it("backing is contained: scroll container never carries bg-black", async () => {
    setup({ textureSize: 16 });
    const scroll = await screen.findByTestId("atlas-scroll");
    expect(scroll.className).not.toContain("bg-black");
  });

  it("the canvas backing is the uniform gray (bg-bg-2); no shared CSS raster", async () => {
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // The backing is the SAME gray as the frames (bg-bg-2), applied via class —
    // transparent (non-frame) canvas pixels reveal it. No inline background, no
    // shared --atlas-url data-URI (the canvas draws the atlas directly).
    expect(grid.className).toContain("bg-bg-2");
    expect(grid.style.background).toBe("");
    expect(grid.style.backgroundImage).toBe("");
    expect(grid.style.getPropertyValue("--atlas-url")).toBe("");
    // the hero is a <canvas> crop (no CSS background raster) — it mounts once the
    // preview resolves to "ok"; assert the canvas is present rather than a token.
    await waitFor(() =>
      expect(screen.getByTestId("atlas-hero").querySelector("canvas")).not.toBeNull(),
    );
  });

  it("gives a hover-highlight overlay that tracks the pointer (and hides on leave)", async () => {
    // The old per-cell CSS hover-lift is gone (no per-cell DOM); the hover cue is
    // now an imperatively-positioned overlay over the hovered cell.
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Visibility is opacity-driven (design pass: the overlay fades via
    // .atlas-hover-fade instead of a display pop; it stays mounted).
    const overlay = screen.getByTestId("atlas-hover-overlay");
    expect(overlay.style.opacity).toBe("0");
    moveToFrame(9); // 4 cols → frame 9 at (row 2, col 1)
    expect(overlay.style.opacity).toBe("1");
    expect(overlay.style.width).toBe("50px");
    expect(overlay.style.left).toBe("54px"); // col 1 → 1*(cell 50 + gap 4)
    expect(overlay.style.top).toBe("108px"); // row 2 → 2*(cell 50 + gap 4)
    fireEvent.mouseLeave(grid);
    expect(overlay.style.opacity).toBe("0");
  });

  it("hover moves do not re-render the atlas grid", async () => {
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // settle

    const before = Number(grid.getAttribute("data-render-count"));
    expect(before).toBeGreaterThan(0);

    for (const k of [1, 2, 3, 4, 5, 6]) {
      moveToFrame(k);
    }
    fireEvent.mouseLeave(grid);

    // pointer math + imperative overlay/hero updates use refs, never setState.
    expect(Number(grid.getAttribute("data-render-count"))).toBe(before);
  });

  it("alpha-gated mode: the canvas stays the uniform gray (no shared raster); listbox unchanged", async () => {
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated: true });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(grid.className).toContain("bg-bg-2");
    expect(grid.style.getPropertyValue("--atlas-url")).toBe("");
    expect(grid.style.backgroundImage).toBe("");
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
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // settled
    fireEvent.keyDown(grid, { key: "End" }); // roving → last cell (63); focus lands on the canvas
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-63"));
    expect(document.activeElement).toBe(grid); // the listbox canvas holds DOM focus
    // shrink to a 2×2 atlas — roving 63 is now out of range
    useMockEmitterProperties.getState().patch(1, { textureSize: 4, colorTexture: "smoke.dds" });
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("4"));
    // focus did NOT fall to <body> — it was re-homed onto the grid canvas.
    await waitFor(() =>
      expect(document.activeElement).toBe(screen.getByRole("listbox", { name: /atlas frames/i })),
    );
  });

  // Reject every request whose kind/params match — delegate the rest to the real
  // mock. The matcher is typed against the real `Request` union (not a loose
  // `{kind:string}`) so a schema rename/widening fails to COMPILE here instead of
  // silently never matching.
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
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled
    fireEvent.keyDown(grid, { key: "Home" });  // focus frame 0
    fireEvent.keyDown(grid, { key: "Enter" }); // assign → set-track-key rejects → commit fails
    await waitFor(() =>
      expect(document.querySelector('[aria-live="polite"]')!.textContent).toMatch(/could not assign frame 0/i),
    );
  });

  it("re-open seeds the grid synchronously from the cache for the SAME emitter", async () => {
    // First mount populates the module-level lastEmitterProps for emitter 1 (8×8).
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    const v1 = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("64"));
    v1.unmount();

    // Re-open the SAME emitter with get-properties BLOCKED: the grid must still seed
    // 64 frames from lastEmitterProps (preview served from the still-warm cache).
    const blocked = new MockBridge();
    const real = blocked.request.bind(blocked);
    const reqSpy = vi.spyOn(blocked, "request").mockImplementation(((req: Request) =>
      req.kind === "emitters/get-properties"
        ? new Promise<never>(() => {}) // never resolves
        : real(req as never)) as typeof blocked.request);
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={blocked} onClose={() => {}} />);
    // Seeded from lastEmitterProps (id matches) → grid paints without waiting…
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("64"));
    // …and the confirming get-properties fetch STILL fires (the seed is a head-start).
    expect(reqSpy).toHaveBeenCalledWith(expect.objectContaining({ kind: "emitters/get-properties" }));
  });

  it("re-open does NOT show stale frames for a DIFFERENT emitter (id-match guard)", async () => {
    // Warm the cache for emitter 1 (8×8).
    useMockEmitterProperties.getState().patch(1, { textureSize: 64, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    const v1 = render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("64"));
    v1.unmount();

    // Re-open a DIFFERENT emitter (id 2) with get-properties BLOCKED. The cache is
    // keyed to id 1, so the seed must NOT apply → defaults (no colorTexture) → the
    // "No color texture set" placeholder, never emitter 1's grid.
    const blocked = new MockBridge();
    const real = blocked.request.bind(blocked);
    vi.spyOn(blocked, "request").mockImplementation(((req: Request) =>
      req.kind === "emitters/get-properties"
        ? new Promise<never>(() => {})
        : real(req as never)) as typeof blocked.request);
    publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step", selection: { frame: 0, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={blocked} onClose={() => {}} />);
    await waitFor(() => expect(screen.getByText(/no color texture set/i)).toBeTruthy());
    expect(screen.queryByRole("listbox", { name: /atlas frames/i })).toBeNull(); // no stale grid
  });

  it("surfaces an INACTIVE-mode preview failure when the blend mode switches into it (not silent)", async () => {
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
    await waitFor(() => expect(activeOption().getAttribute("aria-setsize")).toBe("16"));
    // …the emitter becomes alpha-gated → the failed alpha mode becomes active → broken.
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: true });
    act(() => { handlers.forEach((h) => h({ kind: "emitters/tree/changed" })); });
    await waitFor(() => expect(screen.getByText(/could not be read/i)).toBeTruthy());
  });

  // ── readiness split (perf-audit P1b) ──────────────────────────────────────
  it("a placeholder (no-texture) releases the dock gate via atlasTerminalFirstPaint without mounting the grid", async () => {
    setup({ textureSize: 16, colorTexture: "" });
    await waitFor(() => expect(useDockAnim.getState().atlasTerminalFirstPaint).toBe(true));
    expect(useDockAnim.getState().atlasGridMounted).toBe(false);
  });

  it("a full grid sets BOTH atlasGridMounted and atlasTerminalFirstPaint", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => expect(useDockAnim.getState().atlasGridMounted).toBe(true));
    expect(useDockAnim.getState().atlasTerminalFirstPaint).toBe(true);
  });

  it("defers the INACTIVE alpha-mode preview to idle; the active mode fetches synchronously", async () => {
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
