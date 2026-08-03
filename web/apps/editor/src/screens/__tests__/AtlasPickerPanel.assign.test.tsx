// Vitest unit tests for AtlasPickerPanel click-to-assign (Task 9).
// Verifies: single-key direct assign, multi-same-frame direct assign,
// multi-diff confirm dialog, and no-key no-op.
//
// [#572] The grid is ONE <canvas> — clicks are hit-tested by pointer math, not
// per-cell DOM. jsdom's getBoundingClientRect is all-zeros, so a synthetic click
// at a cell's grid-local center (frameCenter) resolves to that frame. jsdom
// geometry: 16-cell atlas → 4 cols × 50px cells, gap 4 (step 54).

import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { AtlasPickerPanel } from "../AtlasPickerPanel";
import { publishAtlasContext, __resetAtlasContext } from "@/lib/atlas-context";
import { MockBridge } from "@/bridge/mock";
import { useMockEmitterProperties } from "@/bridge/mock-state";
import { __resetPreviewCache } from "@/lib/atlas-preview-cache";
import { __resetModStackForTests } from "@/lib/mod-stack";

beforeEach(() => {
  __resetAtlasContext();
  useMockEmitterProperties.getState().reset();
  __resetPreviewCache();
  __resetModStackForTests();
});

function mk() {
  useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds" });
  const bridge = new MockBridge();
  return bridge;
}

const listbox = () => screen.getByRole("listbox", { name: /atlas frames/i });
function frameCenter(k: number, cols = 4, cell = 50) {
  const step = cell + 4; // GRID_GAP
  return { clientX: (k % cols) * step + cell / 2, clientY: Math.floor(k / cols) * step + cell / 2 };
}
const clickFrame = (k: number) => fireEvent.click(listbox(), frameCenter(k));

describe("AtlasPickerPanel assignment", () => {
  it("single key: clicking a cell sets that key's value (no confirm)", async () => {
    const bridge = mk();
    const spy = vi.spyOn(bridge, "request");
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(9);
    await waitFor(() => expect(spy).toHaveBeenCalledWith(expect.objectContaining({
      kind: "emitters/set-track-key",
      params: expect.objectContaining({ id: 1, track: "index", oldTime: 0.3, newTime: 0.3, newValue: 9 }),
    })));
  });

  it("multi-same: clicking sets ALL selected keys, no confirm", async () => {
    const bridge = mk();
    const spy = vi.spyOn(bridge, "request");
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.2, 0.6] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(3);
    await waitFor(() => {
      const calls = spy.mock.calls.filter(([r]) => (r as { kind: string }).kind === "emitters/set-track-key");
      expect(calls.map(([r]) => (r as { params: { oldTime: number } }).params.oldTime).sort()).toEqual([0.2, 0.6]);
      expect(calls.every(([r]) => (r as { params: { newValue: number } }).params.newValue === 3)).toBe(true);
    });
  });

  it("multi-diff: clicking shows a confirm; assigns all only on OK", async () => {
    const bridge = mk();
    const spy = vi.spyOn(bridge, "request");
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [0.2, 0.6] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(7);
    expect(spy.mock.calls.filter(([r]) => (r as { kind: string }).kind === "emitters/set-track-key")).toHaveLength(0);
    fireEvent.click(await screen.findByRole("button", { name: /set all/i }));
    await waitFor(() => {
      const calls = spy.mock.calls.filter(([r]) => (r as { kind: string }).kind === "emitters/set-track-key");
      expect(calls).toHaveLength(2);
      expect(calls.every(([r]) => (r as { params: { newValue: number } }).params.newValue === 7)).toBe(true);
    });
  });

  it("no-key: clicking is a no-op", async () => {
    const bridge = mk();
    const spy = vi.spyOn(bridge, "request");
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(2);
    await new Promise((r) => setTimeout(r, 20));
    expect(spy.mock.calls.filter(([r]) => (r as { kind: string }).kind === "emitters/set-track-key")).toHaveLength(0);
  });

  it("announces + commits a real assign, and not on the zero-key no-op", async () => {
    const bridge = mk();
    const req = vi.spyOn(bridge, "request");
    const live = () => document.querySelector('[aria-live="polite"]')!;
    // keyTimes match a real fixture index key (time 33) so the assign actually
    // mutates the overlay — not just a fulfilled no-op.
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [33] } });
    const view = render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(9);
    // the correct set-track-key command was dispatched...
    await waitFor(() => expect(req).toHaveBeenCalledWith(expect.objectContaining({
      kind: "emitters/set-track-key",
      params: expect.objectContaining({ id: 1, track: "index", oldTime: 33, newValue: 9 }),
    })));
    // ...the overlay actually changed (read back through get-tracks)...
    const tracks = await bridge.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const idx = (tracks as { tracks: Array<{ name: string; keys: Array<{ time: number; value: number }> }> })
      .tracks.find((t) => t.name === "index")!;
    expect(idx.keys.find((k) => k.time === 33)!.value).toBe(9);
    // ...and we announced it
    await waitFor(() => expect(live().textContent).toMatch(/assigned frame 9/i));
    // zero-key no-op: announcement must be UNCHANGED (snapshot-compare, not one-sided)
    const before = live().textContent;
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } });
    view.rerender(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    clickFrame(2);
    await new Promise((r) => setTimeout(r, 20));
    expect(live().textContent).toBe(before);
  });

  it("restores focus to the grid after the confirm modal closes", async () => {
    const bridge = mk();
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [0.2, 0.6] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const canvas = await screen.findByRole("listbox", { name: /atlas frames/i });
    // Focus re-homes onto the single canvas listbox (the whole grid is one
    // element now); spy on its focus() rather than a per-cell element.
    const focusSpy = vi.spyOn(canvas, "focus");
    clickFrame(7); // opens confirm
    fireEvent.click(await screen.findByRole("button", { name: /set all/i }));
    await waitFor(() => expect(focusSpy).toHaveBeenCalled());
  });
});
