// Vitest unit tests for AtlasPickerPanel (Task 8).
// Verifies: grid cell count, selected-frame highlight, non-square header,
// and the five placeholder states (single-frame, no-texture, missing,
// too-large, off-index-channel).

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
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
});
