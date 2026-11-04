// Mock counterpart of src/host/BridgeDispatch_Timeline.cpp.
//
// Browser mode runs no simulation, so the mock keeps the session's state
// machine only: begin / end are idempotent, seeks clamp to the review window,
// play does not advance on its own, and a new variation steps the seed
// deterministically. The native host is the source of truth.

import type { Request, TimelineStateDto } from "@particle-editor/bridge-schema";
import type { MockDispatchHost } from "./host";

type TimelineKind = "timeline/begin" | "timeline/end" | "timeline/seek" | "timeline/set" | "timeline/new-variation";

type TimelineRequest = Extract<Request, { kind: TimelineKind }>;

// Mirrors kTimelineMaxFrames (PreviewScenario's default review window).
export const MOCK_TIMELINE_MAX_FRAMES = 600;

// Kinds the native host refuses while a session is open (RefuseWhileTimeline).
export const TIMELINE_GATED_KINDS: ReadonlySet<Request["kind"]> = new Set<Request["kind"]>([
  "engine/set/paused",
  "engine/action/step-frames",
  "engine/action/clear",
  "spawner/start",
  "spawner/trigger",
  "spawner/stop",
]);

export function closedTimelineState(seed = 1): TimelineStateDto {
  return {
    active: false,
    playing: false,
    loop: false,
    frame: 0,
    timeSeconds: 0,
    maxFrames: MOCK_TIMELINE_MAX_FRAMES,
    seed,
    status: "closed",
    refused: false,
    revision: 0,
  };
}

function withStatus(state: TimelineStateDto): TimelineStateDto {
  const status = !state.active
    ? "closed"
    : state.playing
      ? "playing"
      : state.frame >= state.maxFrames
        ? "ended"
        : "paused";
  return { ...state, timeSeconds: state.frame / 60, status };
}

export function dispatchTimeline(this: MockDispatchHost, req: TimelineRequest): TimelineStateDto {
  const t = this.timeline;
  switch (req.kind) {
    case "timeline/begin":
      if (!t.active) this.timeline = withStatus({ ...t, active: true, playing: false, frame: 0, revision: 0 });
      return this.timeline;

    case "timeline/end":
      if (t.active) {
        this.timeline = withStatus({ ...t, active: false, playing: false });
        this.emit({ kind: "timeline/state", payload: this.timeline });
      }
      return this.timeline;

    case "timeline/seek": {
      if (!t.active) throw new Error("the timeline is not open");
      const frame = req.params.frame;
      if (!Number.isInteger(frame)) throw new Error("frame must be an integer");
      this.timeline = withStatus({ ...t, frame: Math.min(t.maxFrames, Math.max(0, frame)) });
      return this.timeline;
    }

    case "timeline/set": {
      if (!t.active) throw new Error("the timeline is not open");
      const { playing, loop } = req.params;
      for (const [field, value] of [
        ["playing", playing],
        ["loop", loop],
      ] as const)
        if (value !== undefined && typeof value !== "boolean") throw new Error(`${field} must be a boolean`);
      let next = { ...t, loop: loop ?? t.loop };
      if (playing !== undefined) {
        // Play at the end of the window starts over.
        if (playing && !t.playing && t.frame >= t.maxFrames && !next.loop) next = { ...next, frame: 0 };
        next = { ...next, playing };
      }
      this.timeline = withStatus(next);
      return this.timeline;
    }

    case "timeline/new-variation": {
      if (!t.active) throw new Error("the timeline is not open");
      this.timeline = withStatus({ ...t, seed: (t.seed + 1) >>> 0, revision: t.revision + 1 });
      return this.timeline;
    }
  }
}
