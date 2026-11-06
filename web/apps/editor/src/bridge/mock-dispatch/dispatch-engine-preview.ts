// Browser-only engine preview fixtures. Owns request timing and cancellation;
// no file, pipe or renderer work occurs in this mock.
import {
  ENGINE_LIVE_STATE_CHANGED_EVENT,
  ENGINE_PREVIEW_STILL_RENDERED_EVENT,
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type EngineLiveStateName,
  type EnginePreviewPendingDto,
  type EnginePreviewStatusDto,
  type Request,
} from "@particle-editor/bridge-schema";
import type { MockDispatchHost } from "./host";

export const MOCK_PREVIEW_PNG =
  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=";
const MOCK_ROUND_TRIP_SECONDS = 2.6;
const MOCK_PREVIEW_DELAY_MS = 20;
// Mirrors kMaxTimeSeconds in src/host/EnginePreviewPipe.h.
const MAX_TIME_SECONDS = 600;
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
}

type PreviewHost = MockDispatchHost & { enginePreview: MockEnginePreviewState };

export function dispatchEnginePreview(
  this: PreviewHost,
  req: PreviewRequest,
): EnginePreviewPendingDto | Record<string, never> {
  const preview: MockEnginePreviewState = this.enginePreview;
  if (req.kind === "enginePreview/cancel") {
    if (preview.pending !== null) {
      clearTimeout(preview.pending);
      preview.pending = null;
      if (preview.operation === "status") {
        this.patchAndBroadcast({ enginePreviewAvailable: false });
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
      this.patchAndBroadcast({ enginePreviewAvailable: preview.state === "ready" });
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
  if (preview.state !== "ready") throw new Error("Check engine preview status before requesting a still.");
  preview.operation = "still";
  preview.pending = setTimeout((): void => {
    preview.pending = null;
    preview.operation = null;
    if (preview.error) {
      this.emit({ kind: ENGINE_PREVIEW_STILL_RENDERED_EVENT, payload: { ok: false, requestId, error: preview.error } });
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

// The mock has no engine host to stream from: it reports the live view as started, and the
// viewport keeps showing the mock's own frame.
export function dispatchEngineLive(this: PreviewHost, req: LiveRequest): { state: EngineLiveStateName } {
  if (req.kind === "engineLive/stop") {
    this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "off" } });
    return { state: "off" };
  }
  if (this.enginePreview.state !== "ready")
    throw new Error("Check engine preview status before starting the live engine view.");
  this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "starting" } });
  setTimeout((): void => {
    this.emit({ kind: ENGINE_LIVE_STATE_CHANGED_EVENT, payload: { state: "live" } });
  }, MOCK_PREVIEW_DELAY_MS);
  return { state: "starting" };
}
