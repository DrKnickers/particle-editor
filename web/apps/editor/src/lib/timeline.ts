// timeline.ts — the web side of the interactive timeline (design section 11).
//
// The host owns the session (src/host/BridgeDispatch_Timeline.cpp): while it
// is open, the frame on screen is the deterministic replay of the document to
// the session's frame, and free-run transport, Clear and the spawner are
// refused. This store mirrors the host's TimelineStateDto. It is seeded from
// each timeline/* response and follows the `timeline/state` event, so the
// panel, the toolbar, the View menu and the accelerators all read one value.
//
// It is never persisted: the session starts closed on every load (a page
// reload ends the host's session too, via app/ready), so nothing auto-opens
// over the autosave-recovery modal or the a11y goldens.
//
// Seeks keep one request in flight. A playhead drag sends the first value at
// once and, while that seek replays, keeps only the latest value; it is sent
// when the first answers. A slow replay therefore never queues behind itself.

import { useEffect } from "react";
import { create } from "zustand";
import type { Bridge, TimelineStateDto } from "@particle-editor/bridge-schema";
import { fireAndReport } from "@/lib/status-feedback";

// Mirrors kTimelineMaxFrames (PreviewScenario's default review window).
export const TIMELINE_MAX_FRAMES = 600;
export const TIMELINE_FRAMES_PER_SECOND = 60;

export const CLOSED_TIMELINE: TimelineStateDto = Object.freeze({
  active: false,
  playing: false,
  loop: false,
  frame: 0,
  timeSeconds: 0,
  maxFrames: TIMELINE_MAX_FRAMES,
  seed: 0,
  status: "closed",
  refused: false,
  revision: 0,
});

type TimelineStore = {
  state: TimelineStateDto;
  apply: (state: TimelineStateDto) => void;
};

export const useTimelineStore = create<TimelineStore>((set) => ({
  state: CLOSED_TIMELINE,
  apply: (state) => set({ state }),
}));

/** Select one value from the session state. */
export function useTimeline<T>(selector: (s: TimelineStateDto) => T): T {
  return useTimelineStore((store) => selector(store.state));
}

export function getTimelineState(): TimelineStateDto {
  return useTimelineStore.getState().state;
}

/** Follow the host's `timeline/state` event. Mounted once, by the app shell. */
export function useTimelineEvents(bridge: Bridge): void {
  useEffect(() => bridge.on("timeline/state", (e) => useTimelineStore.getState().apply(e.payload)), [bridge]);
}

function applyResponse(response: TimelineStateDto | undefined): void {
  if (response) useTimelineStore.getState().apply(response);
}

export function openTimeline(bridge: Bridge): Promise<void> {
  return fireAndReport(bridge, { kind: "timeline/begin", params: {} }, "Open timeline").then(applyResponse);
}

export function closeTimeline(bridge: Bridge): Promise<void> {
  pendingFrame = null; // a seek queued now would land after the end
  return fireAndReport(bridge, { kind: "timeline/end", params: {} }, "Close timeline").then(applyResponse);
}

export function toggleTimeline(bridge: Bridge): Promise<void> {
  return getTimelineState().active ? closeTimeline(bridge) : openTimeline(bridge);
}

export function setTimelinePlaying(bridge: Bridge, playing: boolean): Promise<void> {
  return fireAndReport(bridge, { kind: "timeline/set", params: { playing } }, playing ? "Play" : "Pause").then(
    applyResponse,
  );
}

export function setTimelineLoop(bridge: Bridge, loop: boolean): Promise<void> {
  return fireAndReport(bridge, { kind: "timeline/set", params: { loop } }, "Loop").then(applyResponse);
}

export function newTimelineVariation(bridge: Bridge): Promise<void> {
  return fireAndReport(bridge, { kind: "timeline/new-variation", params: {} }, "New variation").then(applyResponse);
}

// The frame of the one seek in flight, and the latest frame asked for while
// it runs.
let inFlightFrame: number | null = null;
let pendingFrame: number | null = null;
// Settles once no seek is in flight or pending (fireAndReport never rejects).
let drained: Promise<void> = Promise.resolve();

/** Seek to `frame`, keeping one seek in flight (see the header). The promise
 *  settles when the queue drains, whether the host took the seek or not. */
export function seekTimeline(bridge: Bridge, frame: number): Promise<void> {
  if (inFlightFrame !== null) {
    pendingFrame = frame;
    return drained;
  }
  inFlightFrame = frame;
  drained = fireAndReport(bridge, { kind: "timeline/seek", params: { frame } }, "Seek")
    .then(applyResponse)
    .then(() => {
      inFlightFrame = null;
      const next = pendingFrame;
      pendingFrame = null;
      if (next !== null && getTimelineState().active) return seekTimeline(bridge, next);
    });
  return drained;
}

/** Step from the frame last asked for: the pending seek's, else the one in
 *  flight, else the session's. */
export function stepTimeline(bridge: Bridge, frames: number): void {
  const { frame, maxFrames } = getTimelineState();
  const base = pendingFrame ?? inFlightFrame ?? frame;
  void seekTimeline(bridge, Math.min(maxFrames, Math.max(0, base + frames)));
}

/** Reset: stop playing and return to frame 0. */
export function resetTimeline(bridge: Bridge): void {
  void (getTimelineState().playing ? setTimelinePlaying(bridge, false) : Promise.resolve()).then(() =>
    seekTimeline(bridge, 0),
  );
}

/** Test seam: forget the seek queue and close the store. */
export function resetTimelineStoreForTests(): void {
  inFlightFrame = null;
  pendingFrame = null;
  drained = Promise.resolve();
  useTimelineStore.setState({ state: CLOSED_TIMELINE });
}
