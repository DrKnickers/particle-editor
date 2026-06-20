// Vitest unit tests for AtlasPickerPanel (Task 8).
// Verifies: grid cell count, selected-frame highlight, non-square header,
// and the five placeholder states (single-frame, no-texture, missing,
// too-large, off-index-channel).

import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { render, screen, waitFor, fireEvent, createEvent, act } from "@testing-library/react";
import { AtlasPickerPanel } from "../AtlasPickerPanel";
import { publishAtlasContext, __resetAtlasContext } from "@/lib/atlas-context";
import { MockBridge } from "@/bridge/mock";
import { useMockEmitterProperties } from "@/bridge/mock-state";
import { __resetPreviewCache, bumpTextureEpoch } from "@/lib/atlas-preview-cache";
import { __resetModStackForTests } from "@/lib/mod-stack";

beforeEach(() => {
  __resetAtlasContext();
  useMockEmitterProperties.getState().reset();
  __resetPreviewCache();
  __resetModStackForTests();
});

afterEach(() => { vi.restoreAllMocks(); });

// jsdom does not resolve `repeat(auto-fill, ...)`, so stub getComputedStyle to
// report a known column count for the listbox grid. We proxy the REAL computed
// style (so getPropertyValue and everything RTL/jsdom calls internally still
// works) and override only gridTemplateColumns for the listbox element.
function stubColumns(cols: number) {
  const real = window.getComputedStyle.bind(window);
  return vi.spyOn(window, "getComputedStyle").mockImplementation((el: Element) => {
    const base = real(el);
    if ((el as HTMLElement).getAttribute?.("role") !== "listbox") return base;
    return new Proxy(base, {
      get(target, prop) {
        if (prop === "gridTemplateColumns") return Array(cols).fill("46px").join(" ");
        const v = Reflect.get(target, prop);
        return typeof v === "function" ? v.bind(target) : v;
      },
    });
  });
}

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

  it("grid container is a labelled listbox with computed column tracks", async () => {
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(grid).toBeTruthy();
    // smart sizing emits explicit `repeat(<cols>, <cell>px)` tracks
    expect(grid.style.gridTemplateColumns).toMatch(/repeat\(\d+, \d+px\)/);
  });

  it("reflow keeps the crop side-driven (Risk 1: not display-column-driven)", async () => {
    setup({ textureSize: 16 }); // side = 4
    // wait for the preview to load so cropStyle is applied
    const cell = await waitFor(() => {
      const c = screen.getAllByTestId("atlas-cell").find((x) => x.getAttribute("data-frame") === "6")!;
      expect(c.style.backgroundImage).not.toBe("");
      return c;
    });
    // backgroundSize is side*100% (4 -> "400% 400%") regardless of the auto-fill
    // display column count — proves the reflow didn't change the crop.
    expect(cell.style.backgroundSize).toBe("400% 400%");
  });

  it("cells are options with preserved test hooks and aria-selected", async () => {
    setup({ textureSize: 16 }); // assigned frame = 5
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    const cells = screen.getAllByTestId("atlas-cell");
    const five = cells.find((c) => c.getAttribute("data-frame") === "5")!;
    // DOM-hook preservation (Risk 6): same element keeps data-* and gains role/aria
    expect(five.getAttribute("role")).toBe("option");
    expect(five.getAttribute("data-selected")).toBe("true");
    expect(five.getAttribute("aria-selected")).toBe("true");
    expect(five.getAttribute("aria-label")).toMatch(/frame 5/i);
    // roving tabindex: the assigned frame is the single tabbable cell
    expect(five.getAttribute("tabindex")).toBe("0");
    const other = cells.find((c) => c.getAttribute("data-frame") === "0")!;
    expect(other.getAttribute("tabindex")).toBe("-1");
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

  it("arrow keys move the roving cursor geometrically (5 columns)", async () => {
    const spy = stubColumns(5);
    setup({ textureSize: 16 }); // assigned 5 -> cursor starts at 5
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    const tabbed = () => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("tabindex") === "0")!.getAttribute("data-frame");
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 5 -> 6
    await waitFor(() => expect(tabbed()).toBe("6"));
    fireEvent.keyDown(grid, { key: "ArrowDown" });  // 6 -> 11 (+5)
    await waitFor(() => expect(tabbed()).toBe("11"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });    // 11 -> 6
    await waitFor(() => expect(tabbed()).toBe("6"));
    fireEvent.keyDown(grid, { key: "Home" });       // -> 0
    await waitFor(() => expect(tabbed()).toBe("0"));
    fireEvent.keyDown(grid, { key: "End" });        // -> 15
    await waitFor(() => expect(tabbed()).toBe("15"));
    spy.mockRestore();
  });

  it("ArrowDown is a no-op into a missing partial-row target; down-then-up is reversible", async () => {
    const spy = stubColumns(5); // 16 cells, rows: 0-4,5-9,10-14,15
    setup({ textureSize: 16 });
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    const tabbed = () => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("tabindex") === "0")!.getAttribute("data-frame");
    fireEvent.keyDown(grid, { key: "Home" }); // 0
    fireEvent.keyDown(grid, { key: "ArrowRight" }); // 1
    await waitFor(() => expect(tabbed()).toBe("1"));
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 1 -> 6
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 6 -> 11
    fireEvent.keyDown(grid, { key: "ArrowDown" }); // 11 -> 16? missing -> no-op, stays 11
    await waitFor(() => expect(tabbed()).toBe("11"));
    fireEvent.keyDown(grid, { key: "ArrowUp" });   // 11 -> 6 (reversible)
    await waitFor(() => expect(tabbed()).toBe("6"));
    spy.mockRestore();
  });

  it("Enter assigns the focused frame; Space too and prevents default", async () => {
    const spy = stubColumns(4);
    const bridge = new MockBridge();
    const req = vi.spyOn(bridge, "request");
    useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    fireEvent.keyDown(grid, { key: "Home" });   // focus 0
    fireEvent.keyDown(grid, { key: "Enter" });  // assign 0
    await waitFor(() => expect(req).toHaveBeenCalledWith(expect.objectContaining({
      kind: "emitters/set-track-key",
      params: expect.objectContaining({ newValue: 0, oldTime: 0.3 }),
    })));
    const ev = createEvent.keyDown(grid, { key: " " });
    fireEvent(grid, ev);
    expect(ev.defaultPrevented).toBe(true); // Space must not scroll the panel
    spy.mockRestore();
  });

  it("hero shows the assigned frame number and reflects keyboard focus", async () => {
    const spy = stubColumns(4);
    setup({ textureSize: 16 }); // assigned 5
    // assigned frame number is visible in the hero caption
    await waitFor(() => expect(screen.getByTestId("atlas-hero").textContent).toMatch(/frame 5/i));
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    fireEvent.keyDown(grid, { key: "Home" }); // focus 0 -> hero follows focus
    await waitFor(() => expect(screen.getByTestId("atlas-hero").textContent).toMatch(/frame 0/i));
    spy.mockRestore();
  });

  it("has an aria-live region", async () => {
    setup({ textureSize: 16 });
    await waitFor(() => expect(document.querySelector('[aria-live="polite"]')).toBeTruthy());
  });

  it("reserves a stable scrollbar gutter (anti-flicker mitigation)", async () => {
    // Presence guard only — proves the mitigation is wired; the actual loop
    // needs real layout (a Playwright check) to observe.
    setup({ textureSize: 16 });
    await screen.findByRole("listbox", { name: /atlas frames/i });
    expect(screen.getByTestId("atlas-scroll").style.scrollbarGutter).toBe("stable");
  });

  it("sizes the grid to multiple columns from the measured width (smart-sizing wiring)", async () => {
    // Guards the 1-column measurement regression: jsdom has no ResizeObserver
    // and clientWidth 0, so without this the sizing path is never exercised.
    const RealRO = globalThis.ResizeObserver;
    let roCb: (() => void) | null = null;
    globalThis.ResizeObserver = class {
      constructor(cb: () => void) { roCb = cb; }
      observe() {}
      disconnect() {}
    } as unknown as typeof ResizeObserver;
    try {
      setup({ textureSize: 16 }); // 4×4 = 16 cells
      const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
      const scroll = screen.getByTestId("atlas-scroll");
      Object.defineProperty(scroll, "clientWidth", { configurable: true, value: 250 });
      act(() => { roCb?.(); }); // fire the observer → re-measure (250 − 24 padding = 226)
      await waitFor(() => expect(grid.style.gridTemplateColumns).toMatch(/repeat\(4, \d+px\)/));
    } finally {
      globalThis.ResizeObserver = RealRO;
    }
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
    await waitFor(() => expect(previewCalls()).toBe(1)); // initial fetch (then cached)
    act(() => { bumpTextureEpoch(); }); // simulate a "reload textures"
    await waitFor(() => expect(previewCalls()).toBe(2)); // cache dropped → re-fetch fresh content
  });

  it("keeps keyboard focus in the grid when the atlas shrinks under it", async () => {
    const spy = stubColumns(8);
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
    fireEvent.keyDown(grid, { key: "End" }); // focus the last cell (63)
    await waitFor(() =>
      expect((document.activeElement as HTMLElement | null)?.getAttribute("data-frame")).toBe("63"),
    );
    // shrink to a 2×2 atlas — cell 63 unmounts
    useMockEmitterProperties.getState().patch(1, { textureSize: 4, colorTexture: "smoke.dds" });
    handlers.forEach((h) => h({ kind: "emitters/tree/changed" }));
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(4));
    // focus did NOT fall to <body> — it was re-homed into the grid
    expect((document.activeElement as HTMLElement | null)?.getAttribute("data-testid")).toBe("atlas-cell");
    spy.mockRestore();
  });
});
