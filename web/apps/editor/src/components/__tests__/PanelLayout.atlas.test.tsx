// Vitest: atlas branch in the right-dock slot (Task 10).
//
// Verifies that setting the right-dock to "atlas" renders
// AtlasPickerPanel's ToolPanel header "Atlas Frames" inside the
// quadrant-spawner slot. Harness is a direct copy of the one in
// PanelLayout.test.tsx — same providers, same stub bridge.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { Bridge } from "@particle-editor/bridge-schema";
import { BridgeContext } from "@/lib/bridge-context";
import { PanelLayout } from "../PanelLayout";
import { __resetRightDockForTests, setDock } from "@/lib/right-dock";
import { __resetAtlasContext, publishAtlasContext } from "@/lib/atlas-context";

function makeStubBridge(): Bridge {
  return {
    request: () => Promise.resolve({}),
    on: () => () => {},
  } as unknown as Bridge;
}

const renderPanelLayout = (bridge: Bridge) =>
  render(
    <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>
    </Tooltip.Provider>,
  );

beforeEach(() => {
  localStorage.removeItem("alo:right-dock");
  localStorage.removeItem("alo:spawner-visible");
  __resetRightDockForTests();
  __resetAtlasContext();
});

describe("PanelLayout — atlas dock branch (Task 10)", () => {
  it("renders AtlasPickerPanel when dock === 'atlas'", () => {
    // Publish a minimal atlas context so the panel has something to read.
    publishAtlasContext({
      emitterId: 1,
      focusedTrack: "index",
      interpolation: "step",
      selection: { frame: null, keyTimes: [] },
    });
    setDock("atlas");

    const bridge = makeStubBridge();
    renderPanelLayout(bridge);

    // The quadrant-spawner slot is always in the DOM (always-mounted
    // collapsible) and the content is the AtlasPickerPanel ToolPanel.
    expect(screen.getByTestId("quadrant-spawner")).toBeInTheDocument();
    // ToolPanel renders its title in a dialog heading with role="dialog"
    // name matching the title prop "Atlas Frames".
    expect(
      screen.getByRole("dialog", { name: "Atlas Frames" }),
    ).toBeInTheDocument();
  });

  it("the atlas dock slot shares the same quadrant-spawner testid as the other docks", () => {
    setDock("atlas");
    const bridge = makeStubBridge();
    renderPanelLayout(bridge);
    // The slot's identity is the right-dock column, not the tool inside it.
    const slot = screen.getByTestId("quadrant-spawner");
    expect(slot).toBeInTheDocument();
    // Content is mounted (atlas panel) — slot is not empty.
    expect(slot.childElementCount).toBeGreaterThan(0);
  });
});
