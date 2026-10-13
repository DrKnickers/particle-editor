// Vitest: TimelinePanel — hidden while the session is closed; each control
// sends the timeline kind and params the host expects; the readout uses the
// §11.3 labels; a stale, empty or refused session says so inline.

import { describe, it, expect, beforeEach } from "vitest";
import { act, fireEvent, render, screen } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { TimelineStateDto } from "@particle-editor/bridge-schema";
import { TimelinePanel, timelineStatusText } from "../TimelinePanel";
import { CLOSED_TIMELINE, resetTimelineStoreForTests, useTimelineStore } from "@/lib/timeline";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";

const open = (patch: Partial<TimelineStateDto> = {}): TimelineStateDto => ({
  ...CLOSED_TIMELINE, active: true, status: "paused", frame: 32, timeSeconds: 32 / 60, seed: 1234, ...patch,
});

function renderPanel(bridge: BridgeStub) {
  return render(
    <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
      <TimelinePanel bridge={bridge} />
    </Tooltip.Provider>,
  );
}

function makeBridge() {
  return makeBridgeStub({
    responses: {
      "timeline/seek": (p) => open({ frame: p.frame, timeSeconds: p.frame / 60 }),
      "timeline/set": (p) => open({ playing: p.playing ?? false, loop: p.loop ?? false }),
      "timeline/new-variation": open({ seed: 99 }),
      "timeline/end": CLOSED_TIMELINE,
    },
  });
}

const flush = () => act(() => new Promise((r) => setTimeout(r, 0)));

function lastRequest(bridge: BridgeStub) {
  return bridge.request.mock.calls.at(-1)?.[0];
}

beforeEach(() => resetTimelineStoreForTests());

describe("TimelinePanel", () => {
  it("renders nothing while the session is closed", () => {
    const { container } = renderPanel(makeBridge());
    expect(container).toBeEmptyDOMElement();
  });

  it("shows the readout with the §11.3 labels and the seed in a disclosure", () => {
    act(() => useTimelineStore.getState().apply(open()));
    renderPanel(makeBridge());
    expect(screen.getByRole("region", { name: "Timeline" })).toBeInTheDocument();
    expect(screen.getByTestId("timeline-readout")).toHaveTextContent("Time since spawn 0.533 s · Frame 32 / 600");
    expect(screen.getByTestId("timeline-seed")).toHaveTextContent("1234");
    expect(screen.getByText("Variation: Locked")).toBeInTheDocument();
    expect(screen.getByText(/Opening the timeline clears placed instances/)).toBeInTheDocument();
    expect(screen.queryByText(/lifetime/i)).not.toBeInTheDocument();
  });

  it("sends one timeline kind per control", async () => {
    act(() => useTimelineStore.getState().apply(open()));
    const bridge = makeBridge();
    renderPanel(bridge);
    const press = async (name: string) => {
      fireEvent.click(screen.getByRole("button", { name }));
      await flush();
      return lastRequest(bridge);
    };
    expect(await press("Play")).toEqual({ kind: "timeline/set", params: { playing: true } });
    expect(await press("Pause")).toEqual({ kind: "timeline/set", params: { playing: false } });
    act(() => useTimelineStore.getState().apply(open({ frame: 32 })));
    expect(await press("Next frame")).toEqual({ kind: "timeline/seek", params: { frame: 33 } });
    expect(await press("Previous frame")).toEqual({ kind: "timeline/seek", params: { frame: 32 } });
    expect(await press("Jump to end")).toEqual({ kind: "timeline/seek", params: { frame: 600 } });
    expect(await press("Jump to start")).toEqual({ kind: "timeline/seek", params: { frame: 0 } });
    expect(await press("Loop")).toEqual({ kind: "timeline/set", params: { loop: true } });
    expect(await press("New variation")).toEqual({ kind: "timeline/new-variation", params: {} });
    expect(screen.getByTestId("timeline-seed")).toHaveTextContent("99");
    expect(await press("Close timeline")).toEqual({ kind: "timeline/end", params: {} });
    expect(screen.queryByRole("region", { name: "Timeline" })).not.toBeInTheDocument();
  });

  it("the playhead seeks to the dragged value and follows the host's frame", async () => {
    act(() => useTimelineStore.getState().apply(open({ frame: 0 })));
    const bridge = makeBridge();
    renderPanel(bridge);
    const playhead = screen.getByRole("slider", { name: "Playhead" });
    expect(playhead).toHaveAttribute("max", "600");
    fireEvent.change(playhead, { target: { value: "250" } });
    expect(playhead).toHaveValue("250");
    await flush();
    expect(lastRequest(bridge)).toEqual({ kind: "timeline/seek", params: { frame: 250 } });
    act(() => useTimelineStore.getState().apply(open({ playing: true, frame: 260 })));
    expect(playhead).toHaveValue("260");
  });

  it("says inline why a frame is missing", () => {
    expect(timelineStatusText({ status: "paused", refused: false })).toBeNull();
    expect(timelineStatusText({ status: "stale", refused: false })).toMatch(/failed/);
    expect(timelineStatusText({ status: "empty", refused: false })).toMatch(/No emitters/);
    expect(timelineStatusText({ status: "paused", refused: true })).toMatch(/overload guard/);
    act(() => useTimelineStore.getState().apply(open({ status: "empty" })));
    renderPanel(makeBridge());
    expect(screen.getByTestId("timeline-notice")).toHaveTextContent("No emitters to play.");
  });
});
