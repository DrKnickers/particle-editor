// Vitest unit tests for BackgroundPicker.
//
// Covers:
//   1. Game-dome section: Names enumerate from skydome-list per context,
//      and choosing one dispatches skydome-environment.
//   2. Load-status surfacing: a dome whose .alo loads shows the "Game dome"
//      active-mode indicator; a dome that fails to load shows a per-slot
//      "couldn't load" note + the solid-colour fallback indicator (instead of
//      silently reading as applied).
//   3. The former custom skydome-texture slots are gone (no "Browse..."
//      tiles) — the picker is Game dome + Solid colour only.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, within } from "@testing-library/react";
import { BackgroundPicker } from "../BackgroundPicker";
import { MockBridge } from "@/bridge/mock";
import { useMockEngineState, makeDefaultEngineState } from "@/bridge/mock-state";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";

// The MockBridge mutates a global zustand store; reset it so each MockBridge-
// driven test starts from a clean snapshot (the stub-bridge tests below don't
// touch the store).
beforeEach(() => {
  useMockEngineState.setState(makeDefaultEngineState());
});

// Display-logic stub: inject just the skydome fields the picker reads so we can
// assert the surfacing without a real engine. skydome-list returns empty (the
// injected current Name is added by the picker's withCurrent()).
function makeSnapshotBridge(partial: Partial<EngineStateDto>): Bridge {
  const snapshot = {
    background: 0x00112233,
    skydomeSlot: 0,
    skydomeContext: "space",
    skydomePrimaryName: "",
    skydomeSecondaryName: "",
    skydomePrimaryStatus: "none",
    skydomeSecondaryStatus: "none",
    ...partial,
  };
  const request = vi.fn().mockImplementation((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
    if (req.kind === "engine/query/skydome-list")
      return Promise.resolve({ primary: [], secondary: [] });
    return Promise.resolve({});
  });
  return {
    request,
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge;
}

// The game-dome section: Names enumerate from skydome-list per context,
// and choosing one dispatches skydome-environment.
describe("BackgroundPicker — game dome", () => {
  it("populates the primary selector from skydome-list and dispatches on change", async () => {
    const bridge = new MockBridge();
    render(<BackgroundPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    // Default context is Space → the mock returns Stars_* primaries.
    const primary = await screen.findByRole("combobox", { name: "Primary dome" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "Stars_Low" })).toBeInTheDocument();
    });

    fireEvent.change(primary, { target: { value: "Stars_Low" } });
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.skydomePrimaryName).toBe("Stars_Low");
      expect(snap.skydomeContext).toBe("space");
    });
  });

  it("switching context to Land re-enumerates the lists", async () => {
    const bridge = new MockBridge();
    render(<BackgroundPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("combobox", { name: "Primary dome" });

    fireEvent.click(screen.getByRole("button", { name: "land" }));
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "Day_Blue_Sky" })).toBeInTheDocument();
    });
  });
});

// Load-status surfacing + active-mode indicator.
describe("BackgroundPicker — skydome load status", () => {
  it("a loaded dome shows the Game dome indicator and no failure note", async () => {
    const bridge = makeSnapshotBridge({
      skydomePrimaryName: "Stars_Low",
      skydomePrimaryStatus: "ok",
    });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);

    const indicator = await screen.findByRole("status", { name: "Active background" });
    // The indicator renders at mount as "Solid colour" and flips to "Game dome" only
    // after the async snapshot resolves -> AWAIT the text (a sync getByText here races
    // the snapshot resolution and flakes under CI load).
    await within(indicator).findByText("Game dome");
    expect(screen.queryByText("Couldn't load this dome's model.")).toBeNull();
    expect(screen.queryByText(/selected dome didn't load/)).toBeNull();
  });

  it("a failed dome shows a per-slot note + the solid-colour fallback indicator", async () => {
    const bridge = makeSnapshotBridge({
      skydomePrimaryName: "Broken_Sky",
      skydomePrimaryStatus: "load-failed",
    });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);

    // The chosen Name still shows in the dropdown (controlled by the name)...
    const primary = await screen.findByRole("combobox", { name: "Primary dome" });
    // ...but the load failure is surfaced rather than read as applied. Await a
    // snapshot-derived element first so the sync assertions below don't race the fetch.
    await screen.findByText("Couldn't load this dome's model.");
    expect((primary as HTMLSelectElement).value).toBe("Broken_Sky");
    const indicator = screen.getByRole("status", { name: "Active background" });
    expect(within(indicator).getByText("Solid colour")).toBeInTheDocument();
    expect(within(indicator).getByText(/selected dome didn't load/)).toBeInTheDocument();
  });

  it("surfaces a failed SECONDARY dome independently of the primary slot", async () => {
    const bridge = makeSnapshotBridge({
      skydomeSecondaryName: "Broken_Sky",
      skydomeSecondaryStatus: "load-failed",
    });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);

    const secondary = await screen.findByRole("combobox", { name: "Secondary dome" });
    await screen.findByText("Couldn't load this dome's model.");
    expect((secondary as HTMLSelectElement).value).toBe("Broken_Sky");
    const indicator = screen.getByRole("status", { name: "Active background" });
    expect(within(indicator).getByText("Solid colour")).toBeInTheDocument();
  });

  it("a loaded SECONDARY-only dome reads as Game dome", async () => {
    const bridge = makeSnapshotBridge({
      skydomeSecondaryName: "Nebula_Field_Blue",
      skydomeSecondaryStatus: "ok",
    });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);

    const indicator = await screen.findByRole("status", { name: "Active background" });
    await within(indicator).findByText("Game dome");
    expect(screen.queryByText("Couldn't load this dome's model.")).toBeNull();
  });

  it("a persisted legacy texture slot (no game dome) reads as Custom texture, not Solid colour", async () => {
    // Regression guard: dropping the skydomeSlot read would make the indicator
    // claim "Solid colour" while the engine still renders the legacy texture.
    const bridge = makeSnapshotBridge({ skydomeSlot: 9 });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);

    const indicator = await screen.findByRole("status", { name: "Active background" });
    await within(indicator).findByText("Custom texture");
    expect(within(indicator).queryByText("Solid colour")).toBeNull();
    // The solid swatch must NOT read as the active/selected background.
    const solid = screen.getByRole("button", { name: "Solid colour" });
    expect(solid).toHaveAttribute("aria-pressed", "false");
  });

  it("end-to-end: a missing dome dispatched through the mock surfaces load-failed", async () => {
    const bridge = new MockBridge();
    render(<BackgroundPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("combobox", { name: "Primary dome" });

    // Drive the dispatch directly (awaiting it drains the initial-snapshot
    // request first, so the live engine/state/changed update isn't clobbered).
    // The mock derives "load-failed" for a MOCK_MISSING_DOMES Name.
    await bridge.request({
      kind: "engine/set/skydome-environment",
      params: { context: "space", primaryName: "Broken_Sky", secondaryName: "" },
    });

    expect(await screen.findByText("Couldn't load this dome's model.")).toBeInTheDocument();
    const indicator = screen.getByRole("status", { name: "Active background" });
    expect(within(indicator).getByText("Solid colour")).toBeInTheDocument();
  });
});

// The custom skydome-texture slots were removed.
describe("BackgroundPicker — no custom texture slots", () => {
  it("renders no Browse... custom-slot tiles", async () => {
    const bridge = new MockBridge();
    render(<BackgroundPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    // Wait for the body to mount.
    await screen.findByRole("combobox", { name: "Primary dome" });
    expect(screen.queryByRole("button", { name: /Custom slot/ })).toBeNull();
    expect(screen.queryByText("Browse...")).toBeNull();
  });
});
