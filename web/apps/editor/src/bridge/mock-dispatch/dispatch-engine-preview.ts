// Browser-only engine preview fixtures. Owns request timing and cancellation;
// no file, pipe or renderer work occurs in this mock.
import {
  ENGINE_LIVE_STATE_CHANGED_EVENT,
  ENGINE_PREVIEW_STILL_RENDERED_EVENT,
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type EngineLiveStateName,
  type EngineLiveStatsDto,
  type EngineStateDto,
  type EnginePreviewPendingDto,
  type EnginePreviewStatusDto,
  type Request,
} from "@particle-editor/bridge-schema";
import type { MockDispatchHost } from "./host";
import { useMockEngineState } from "../mock-state";

export const MOCK_PREVIEW_PNG =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=";
const MOCK_ROUND_TRIP_SECONDS = 2.6;
const MOCK_PREVIEW_DELAY_MS = 20;
// Mirrors kMaxTimeSeconds in src/host/EnginePreviewPipe.h.
const MAX_TIME_SECONDS = 600;
// Mirrors EngineHostLaunchPolicy.h; browser mode simulates a configured host becoming ready.
const PATH_NOT_SET_MESSAGE = "Set the map editor executable in Preferences > Engine preview.";
type PreviewRequest = Extract<
  Request,
  { kind: "enginePreview/status" | "enginePreview/still" | "enginePreview/cancel" }
>;

export interface MockEnginePreviewState {
  state: EnginePreviewStatusDto["state"];
  nextId: number;
  pending: ReturnType<typeof setTimeout> | null;
  error: string | null;
  operation: "status" | "still" | null;
  live: EngineLiveStateName;
}

type PreviewHost = MockDispatchHost & { enginePreview: MockEnginePreviewState };

export function dispatchEnginePreview(
  this: PreviewHost,
  req: PreviewRequest,
  exePath: string | null,
): EnginePreviewPendingDto | Record<string, never> {
  const preview: MockEnginePreviewState = this.enginePreview;
  if (req.kind === "enginePreview/cancel") {
    if (preview.pending !== null) {
      clearTimeout(preview.pending);
      preview.pending = null;
      if (preview.operation === "status") {
        this.patchAndBroadcast({ enginePreviewAvailable: exePath !== null });
        this.emit({
          kind: ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
          payload: { state: "absent", requestId: preview.nextId - 1, error: "Engine preview cancelled." },
        });
      } else {
        this.emit({
          kind: ENGINE_PREVIEW_STILL_RENDERED_EVENT,
          payload: { ok: false, requestId: preview.nextId - 1, error: "Engine preview cancelled." },
        });
      }
      preview.operation = null;
    }
    return {};
  }
  if (preview.pending !== null) throw new Error("An engine preview request is already running.");
  const requestId: number = preview.nextId++;
  if (req.kind === "enginePreview/status") {
    preview.operation = "status";
    preview.pending = setTimeout((): void => {
      preview.pending = null;
      preview.operation = null;
      this.patchAndBroadcast({ enginePreviewAvailable: preview.state === "ready" || exePath !== null });
      this.emit({
        kind: ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
        payload: {
          state: preview.state,
          requestId,
          ...(preview.state !== "absent" ? { hostVersion: "1.0.0" } : {}),
          ...(preview.state === "ready" ? { internalName: "preview_effect" } : {}),
          ...(preview.error ? { error: preview.error } : {}),
        },
      });
    }, MOCK_PREVIEW_DELAY_MS);
    return { status: "pending", requestId };
  }
  const timeSeconds: number = req.params.timeSeconds;
  if (!Number.isFinite(timeSeconds) || timeSeconds < 0 || timeSeconds > MAX_TIME_SECONDS)
    throw new Error("Preview time must be between 0 and 600 seconds.");
  preview.operation = "still";
  preview.pending = setTimeout((): void => {
    preview.pending = null;
    preview.operation = null;
    if (preview.state === "absent" && exePath !== null && preview.error === null) {
      preview.state = "ready";
      this.patchAndBroadcast({ enginePreviewAvailable: true });
    }
    const error: string | null = preview.error ?? (preview.state !== "ready" ? PATH_NOT_SET_MESSAGE : null);
    if (error) {
      this.emit({ kind: ENGINE_PREVIEW_STILL_RENDERED_EVENT, payload: { ok: false, requestId, error } });
      return;
    }
    this.emit({
      kind: ENGINE_PREVIEW_STILL_RENDERED_EVENT,
      payload: {
        ok: true,
        requestId,
        dataUri: MOCK_PREVIEW_PNG,
        seconds: MOCK_ROUND_TRIP_SECONDS,
        width: 1,
        height: 1,
      },
    });
  }, MOCK_PREVIEW_DELAY_MS);
  return { status: "pending", requestId };
}

type LiveRequest = Extract<Request, { kind: "engineLive/start" | "engineLive/stop" }>;

// The mock's pretend stream rate, shown by the viewport badge while live.
const MOCK_LIVE_FPS = 60;

// The mock has no engine host to stream from: it reports the live view as started, and the
// viewport keeps showing the mock's own frame.
export function dispatchEngineLive(
  this: PreviewHost,
  req: LiveRequest,
  exePath: string | null,
): { state: EngineLiveStateName } {
  const preview: MockEnginePreviewState = this.enginePreview;
  if (req.kind === "engineLive/stop") {
    preview.live = "off";
    this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "off" } });
    return { state: "off" };
  }
  if (preview.state === "absent" && exePath !== null && preview.error === null) {
    preview.state = "ready";
    this.patchAndBroadcast({ enginePreviewAvailable: true });
  }
  if (preview.state !== "ready") throw new Error(preview.error ?? PATH_NOT_SET_MESSAGE);
  preview.live = "starting";
  this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "starting" } });
  setTimeout((): void => {
    if (preview.live !== "starting") return; // stopped before the pretend start finished
    preview.live = "live";
    this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "live" } });
  }, MOCK_PREVIEW_DELAY_MS);
  return { state: "starting" };
}

export function readMockEngineLiveStats(this: PreviewHost): EngineLiveStatsDto {
  const live: EngineLiveStateName = this.enginePreview.live;
  const { ground, groundZ }: EngineStateDto = useMockEngineState.getState();
  const keepsGround: boolean = live === "live" && (!ground || groundZ !== 0);
  return {
    state: live,
    fps: live === "live" ? MOCK_LIVE_FPS : 0,
    cameraLatencyMs: null,
    groundNote: keepsGround ? "Engine view keeps its ground visible at height 0." : "",
  };
}
