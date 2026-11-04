// Vitest: while the timeline is open, free-run transport is rerouted to it
// and the controls the host refuses are disabled — the toolbar's Play / Step
// / Step 10, the F8 / F9 / F10 accelerators, Ctrl+Del and Ctrl+Space, and
// the Spawner pane, and the View menu's Pause / Step Forward and the disabled
// Clear All Particles. With the timeline closed they behave as before.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { act, fireEvent, render, screen } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { Bridge, EngineStateDto, TimelineStateDto } from "@particle-editor/bridge-schema";
import { Toolbar } from "../Toolbar";
import { MenuBar } from "../MenuBar";
import { SpawnerPane } from "@/screens/SpawnerPane";
import { useAppAccelerators } from "@/lib/use-app-accelerators";
import { CLOSED_TIMELINE, resetTimelineStoreForTests, useTimelineStore } from "@/lib/timeline";
import { __resetRightDockForTests } from "@/lib/right-dock";
import { __resetStatusFeedbackForTests, useStatusFeedback } from "@/lib/status-feedback";
import { makeDefaultEngineState } from "@/bridge/mock-state";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";

const open = (patch: Partial<TimelineStateDto> = {}): TimelineStateDto => ({
  ...CLOSED_TIMELINE,
  active: true,
  status: "paused",
  frame: 40,
  ...patch,
});

const flush = () => act(() => new Promise((r) => setTimeout(r, 0)));

function makeBridge(): BridgeStub {
  return makeBridgeStub({
    responses: {
      "engine/state/snapshot": makeDefaultEngineState() as EngineStateDto,
      "timeline/begin": open({ frame: 0 }),
      "timeline/set": (p) => open({ playing: p.playing ?? false }),
      "timeline/seek": (p) => open({ frame: p.frame }),
    },
  });
}

function kinds(bridge: BridgeStub) {
  return bridge.request.mock.calls.map(([r]) => r).filter((r) => r.kind !== "engine/state/snapshot");
}

beforeEach(() => {
  resetTimelineStoreForTests();
  __resetRightDockForTests();
  __resetStatusFeedbackForTests();
});

describe("Toolbar while the timeline is open", () => {
  const renderToolbar = (bridge: Bridge) =>
    render(
      <Tooltip.Provider delayDuration={0}>
        <Toolbar bridge={bridge} />
      </Tooltip.Provider>,
    );

  it("the timeline toggle opens the session", async () => {
    const bridge = makeBridge();
    renderToolbar(bridge);
    const toggle = screen.getByRole("button", { name: "Toggle timeline" });
    expect(toggle).toHaveAttribute("aria-pressed", "false");
    fireEvent.click(toggle);
    await flush();
    expect(kinds(bridge)).toEqual([{ kind: "timeline/begin", params: {} }]);
    expect(toggle).toHaveAttribute("aria-pressed", "true");
  });

  it("Play, Step and Step 10 drive the timeline, never free-run", async () => {
    act(() => useTimelineStore.getState().apply(open()));
    const bridge = makeBridge();
    renderToolbar(bridge);
    fireEvent.click(screen.getByRole("button", { name: "Play" }));
    await flush();
    fireEvent.click(screen.getByRole("button", { name: "Pause" }));
    await flush();
    fireEvent.click(screen.getByRole("button", { name: "Step" }));
    await flush();
    fireEvent.click(screen.getByRole("button", { name: "Step 10" }));
    await flush();
    expect(kinds(bridge)).toEqual([
      { kind: "timeline/set", params: { playing: true } },
      { kind: "timeline/set", params: { playing: false } },
      { kind: "timeline/seek", params: { frame: 41 } },
      { kind: "timeline/seek", params: { frame: 51 } },
    ]);
  });

  it("with the timeline closed, Step still steps free-run", async () => {
    const bridge = makeBridge();
    renderToolbar(bridge);
    fireEvent.click(screen.getByRole("button", { name: "Step" }));
    await flush();
    expect(kinds(bridge)).toEqual([{ kind: "engine/action/step-frames", params: { frames: 1 } }]);
  });
});

describe("accelerators while the timeline is open", () => {
  function Harness({ bridge }: { bridge: Bridge }) {
    useAppAccelerators(bridge);
    return null;
  }

  it("F8 / F9 / F10 drive the timeline; Ctrl+Del and Ctrl+Space announce instead of sending", async () => {
    act(() => useTimelineStore.getState().apply(open()));
    const bridge = makeBridge();
    render(<Harness bridge={bridge} />);
    const press = async (combo: string) => {
      bridge.emit("accelerator/pressed", { combo });
      await flush();
    };
    await press("F8");
    await press("F9");
    await press("F10");
    await press("Ctrl+Del");
    expect(useStatusFeedback.getState().message).toBe("Clear particles is unavailable while the timeline is open");
    await press("Ctrl+Space");
    expect(useStatusFeedback.getState().message).toBe("Spawn is unavailable while the timeline is open");
    expect(kinds(bridge).filter((r) => r.kind !== "register-accelerators")).toEqual([
      { kind: "timeline/set", params: { playing: true } },
      { kind: "timeline/seek", params: { frame: 41 } },
      { kind: "timeline/seek", params: { frame: 51 } },
    ]);
  });
});

describe("Spawner pane while the timeline is open", () => {
  it("disables every control and says why", async () => {
    const bridge = makeBridge();
    const { container } = render(<SpawnerPane bridge={bridge} />);
    await flush();
    expect(container.querySelector("fieldset")).not.toBeDisabled();
    expect(screen.queryByTestId("spawner-timeline-note")).not.toBeInTheDocument();
    act(() => useTimelineStore.getState().apply(open()));
    expect(container.querySelector("fieldset")).toBeDisabled();
    expect(screen.getByTestId("spawner-timeline-note")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Stop spawner" })).toBeDisabled();
    const before = kinds(bridge).length;
    fireEvent.click(screen.getByRole("radio", { name: /auto/i }));
    await flush();
    expect(kinds(bridge)).toHaveLength(before);
    vi.restoreAllMocks();
  });

  it("a Spinner's wheel, which the fieldset does not reach, sends nothing", async () => {
    const bridge = makeBridge();
    const { container } = render(<SpawnerPane bridge={bridge} />);
    await flush();
    const spinner = container.querySelector("fieldset input[type=text]")!;
    const starts = () => kinds(bridge).filter((r) => r.kind === "spawner/start").length;
    // The control: with the timeline closed the wheel commits a new config.
    fireEvent.wheel(spinner, { deltaY: -100 });
    await flush();
    expect(starts()).toBe(1);
    act(() => useTimelineStore.getState().apply(open()));
    const shown = (spinner as HTMLInputElement).value;
    fireEvent.wheel(spinner, { deltaY: -100 });
    await flush();
    expect(starts()).toBe(1);
    expect((spinner as HTMLInputElement).value).toBe(shown);
  });
});

describe("the View menu while the timeline is open", () => {
  it("Pause and Step Forward drive the timeline, and Clear All Particles is disabled", async () => {
    act(() => useTimelineStore.getState().apply(open()));
    const bridge = makeBridge();
    const noop = () => {};
    render(
      <Tooltip.Provider delayDuration={0}>
        <MenuBar
          bridge={bridge}
          onOpenImportEmittersDialog={noop}
          onOpenAboutDialog={noop}
          onOpenRescaleDialog={noop}
          onResetPanelLayout={noop}
        />
      </Tooltip.Provider>,
    );
    await flush();
    const openMenu = (name: string) => {
      const trigger = screen.getByRole("menuitem", { name });
      fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
      fireEvent.click(trigger);
    };
    // Step Forward first: once Pause starts play, Step Forward is disabled.
    openMenu("View");
    fireEvent.click(await screen.findByRole("menuitem", { name: /^Step Forward/ }));
    await flush();
    openMenu("View");
    fireEvent.click(await screen.findByRole("menuitem", { name: /^Pause/ }));
    await flush();
    openMenu("View");
    expect(await screen.findByRole("menuitem", { name: /^Step Forward/ })).toHaveAttribute("data-disabled");
    const transport = kinds(bridge).filter((r) => r.kind.startsWith("timeline/") || r.kind.startsWith("engine/"));
    expect(transport.filter((r) => r.kind !== "engine/query/msaa-levels")).toEqual([
      { kind: "timeline/seek", params: { frame: 41 } },
      { kind: "timeline/set", params: { playing: true } },
    ]);
    fireEvent.keyDown(document.activeElement ?? document.body, { key: "Escape" });
    openMenu("Edit");
    expect(await screen.findByRole("menuitem", { name: /Clear All Particles/ })).toHaveAttribute("data-disabled");
  });
});
