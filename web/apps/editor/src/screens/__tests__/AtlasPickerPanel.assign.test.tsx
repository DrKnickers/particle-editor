// Vitest unit tests for AtlasPickerPanel click-to-assign (Task 9).
// Verifies: single-key direct assign, multi-same-frame direct assign,
// multi-diff confirm dialog, and no-key no-op.

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

describe("AtlasPickerPanel assignment", () => {
  it("single key: clicking a cell sets that key's value (no confirm)", async () => {
    const bridge = mk();
    const spy = vi.spyOn(bridge, "request");
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
    const cell = await waitFor(() => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("data-frame") === "9")!);
    fireEvent.click(cell);
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
    const cell = await waitFor(() => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("data-frame") === "3")!);
    fireEvent.click(cell);
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
    const cell = await waitFor(() => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("data-frame") === "7")!);
    fireEvent.click(cell);
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
    const cell = await waitFor(() => screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("data-frame") === "2")!);
    fireEvent.click(cell);
    await new Promise((r) => setTimeout(r, 20));
    expect(spy.mock.calls.filter(([r]) => (r as { kind: string }).kind === "emitters/set-track-key")).toHaveLength(0);
  });
});
