// Vitest: the timeline store (lib/timeline.ts) — responses and events land in
// one state, seeks keep one request in flight, steps build on the latest
// frame asked for, and the session is never persisted.

import { describe, it, expect, beforeEach, afterEach } from "vitest";
import { act, renderHook } from "@testing-library/react";
import type { Request, TimelineStateDto } from "@particle-editor/bridge-schema";
import { makeBridgeStub } from "@/test/bridge-stub";
import {
  CLOSED_TIMELINE, closeTimeline, getTimelineState, openTimeline, resetTimeline, resetTimelineStoreForTests,
  seekTimeline, setTimelineLoop, setTimelinePlaying, stepTimeline, toggleTimeline, useTimelineEvents,
  useTimelineStore,
} from "../timeline";
import { __resetStatusFeedbackForTests, useStatusFeedback } from "../status-feedback";

const open = (patch: Partial<TimelineStateDto> = {}): TimelineStateDto => ({
  ...CLOSED_TIMELINE, active: true, status: "paused", seed: 7, ...patch,
});

const flush = () => act(() => new Promise((r) => setTimeout(r, 0)));

function sent(bridge: ReturnType<typeof makeBridgeStub>, kind: Request["kind"]) {
  return bridge.request.mock.calls.map(([r]) => r).filter((r) => r.kind === kind);
}

beforeEach(() => {
  resetTimelineStoreForTests();
  __resetStatusFeedbackForTests();
  localStorage.clear();
});
afterEach(() => resetTimelineStoreForTests());

describe("timeline store", () => {
  it("starts closed and writes nothing to storage", async () => {
    expect(getTimelineState()).toEqual(CLOSED_TIMELINE);
    const bridge = makeBridgeStub({ responses: { "timeline/begin": open() } });
    await openTimeline(bridge);
    expect(getTimelineState().active).toBe(true);
    expect(localStorage.length).toBe(0);
  });

  it("applies each response and each timeline/state event", async () => {
    const bridge = makeBridgeStub({
      responses: {
        "timeline/begin": open(),
        "timeline/set": (p) => open({ playing: p.playing ?? false, loop: p.loop ?? false }),
        "timeline/end": CLOSED_TIMELINE,
      },
    });
    renderHook(() => useTimelineEvents(bridge));
    await openTimeline(bridge);
    await setTimelinePlaying(bridge, true);
    expect(getTimelineState().playing).toBe(true);
    bridge.emit("timeline/state", open({ playing: true, frame: 42, timeSeconds: 0.7 }));
    expect(getTimelineState().frame).toBe(42);
    await setTimelineLoop(bridge, true);
    expect(sent(bridge, "timeline/set").at(-1)).toEqual({ kind: "timeline/set", params: { loop: true } });
    await closeTimeline(bridge);
    expect(getTimelineState().active).toBe(false);
  });

  it("toggle opens a closed session and closes an open one", async () => {
    const bridge = makeBridgeStub({ responses: { "timeline/begin": open(), "timeline/end": CLOSED_TIMELINE } });
    await toggleTimeline(bridge);
    await toggleTimeline(bridge);
    expect(bridge.request.mock.calls.map(([r]) => r.kind)).toEqual(["timeline/begin", "timeline/end"]);
  });

  it("a refused request leaves the state alone and reports the failure", async () => {
    const bridge = makeBridgeStub({
      responses: { "timeline/begin": () => Promise.reject(new Error("the timeline is not available in this run")) },
    });
    await openTimeline(bridge);
    expect(getTimelineState()).toEqual(CLOSED_TIMELINE);
    expect(useStatusFeedback.getState().message).toBe("Open timeline failed: the timeline is not available in this run");
  });
});

describe("seeks", () => {
  // A seek bridge whose answers are released by hand.
  function deferredSeekBridge() {
    const release: Array<() => void> = [];
    const bridge = makeBridgeStub({
      responses: {
        "timeline/seek": (p) => new Promise((resolve) => release.push(() => resolve(open({ frame: p.frame })))),
      },
    });
    return { bridge, release };
  }

  it("keep one in flight and send only the latest value when it answers", async () => {
    const { bridge, release } = deferredSeekBridge();
    seekTimeline(bridge, 10);
    seekTimeline(bridge, 20);
    seekTimeline(bridge, 30);
    expect(sent(bridge, "timeline/seek").map((r) => (r as { params: { frame: number } }).params.frame)).toEqual([10]);
    release.shift()!();
    await flush();
    expect(sent(bridge, "timeline/seek").map((r) => (r as { params: { frame: number } }).params.frame)).toEqual([10, 30]);
    release.shift()!();
    await flush();
    expect(getTimelineState().frame).toBe(30);
    expect(sent(bridge, "timeline/seek")).toHaveLength(2);
  });

  it("steps build on the seek in flight, then on the pending one", async () => {
    const { bridge, release } = deferredSeekBridge();
    useTimelineStore.getState().apply(open({ frame: 100 }));
    void seekTimeline(bridge, 300);   // in flight; the store still says 100
    stepTimeline(bridge, 1);          // 301, pending
    stepTimeline(bridge, 10);         // 311, pending
    release.shift()!();
    await flush();
    expect(sent(bridge, "timeline/seek").map((r) => (r as { params: { frame: number } }).params.frame)).toEqual([300, 311]);
    stepTimeline(bridge, 1);          // 312, while 311 is in flight
    release.shift()!();
    await flush();
    release.shift()!();
    await flush();
    expect(sent(bridge, "timeline/seek").map((r) => (r as { params: { frame: number } }).params.frame)).toEqual([300, 311, 312]);
    expect(getTimelineState().frame).toBe(312);
  });

  it("steps clamp to the window", async () => {
    const { bridge, release } = deferredSeekBridge();
    useTimelineStore.getState().apply(open({ frame: 595 }));
    stepTimeline(bridge, 10);
    release.shift()!();
    await flush();
    stepTimeline(bridge, -1000);
    expect(sent(bridge, "timeline/seek").map((r) => (r as { params: { frame: number } }).params.frame)).toEqual([600, 0]);
  });

  it("closing drops a pending seek, and the drain settles even when the host refuses", async () => {
    const release: Array<() => void> = [];
    const bridge = makeBridgeStub({
      responses: {
        "timeline/seek": () => new Promise((_, reject) => release.push(() => reject(new Error("busy")))),
        "timeline/end": CLOSED_TIMELINE,
      },
    });
    useTimelineStore.getState().apply(open({ frame: 10 }));
    const drained = seekTimeline(bridge, 20);
    void seekTimeline(bridge, 30);
    await closeTimeline(bridge);
    release.shift()!();
    await act(() => drained);
    expect(sent(bridge, "timeline/seek")).toHaveLength(1);
    expect(getTimelineState().active).toBe(false);
  });

  it("reset pauses a playing session, then seeks to frame 0", async () => {
    const bridge = makeBridgeStub({
      responses: { "timeline/set": open({ playing: false, frame: 90 }), "timeline/seek": open({ frame: 0 }) },
    });
    useTimelineStore.getState().apply(open({ playing: true, frame: 90 }));
    resetTimeline(bridge);
    await flush();
    expect(bridge.request.mock.calls.map(([r]) => r)).toEqual([
      { kind: "timeline/set", params: { playing: false } },
      { kind: "timeline/seek", params: { frame: 0 } },
    ]);
  });
});
