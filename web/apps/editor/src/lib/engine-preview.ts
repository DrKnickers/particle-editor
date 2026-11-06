// Owns the transient preview panel state and correlated bridge waits. The host
// owns availability, saving and rendering; this module never changes the document.
import { create } from "zustand";
import {
  ENGINE_PREVIEW_STILL_RENDERED_EVENT,
  ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
  type Bridge,
  type EventOf,
  type EnginePreviewPendingDto,
  type EnginePreviewResultDto,
  type EnginePreviewStatusDto,
  type Request,
} from "@particle-editor/bridge-schema";

// Mirrors src/host/EnginePreviewPipe.h: 3 commands * (kCommandBudgetSeconds + kTransportGraceSeconds), plus 5 seconds margin.
const STATUS_TIMEOUT_MS = 65_000;
// Mirrors src/host/EnginePreviewPipe.h: 1 command * (kStillBudgetSeconds + kTransportGraceSeconds), plus 5 seconds margin.
const STILL_TIMEOUT_MS = 190_000;
type PreviewRequest = Extract<Request, { kind: "enginePreview/status" | "enginePreview/still" }>;
const MILLISECONDS_PER_SECOND = 1_000;

type PreviewReply = EnginePreviewStatusDto | EnginePreviewResultDto;
type PreviewEventKind = typeof ENGINE_PREVIEW_STATUS_CHECKED_EVENT | typeof ENGINE_PREVIEW_STILL_RENDERED_EVENT;

export interface EnginePreviewStore {
  isOpen: boolean;
  isChecking: boolean;
  isBusy: boolean;
  status: EnginePreviewStatusDto | null;
  result: Extract<EnginePreviewResultDto, { ok: true }> | null;
  capturedAtMs: number | null;
  // Click to image, including the copy save, pipe and PNG transfer; result.seconds is the engine's share.
  roundTripSeconds: number | null;
  error: string | null;
}

export const useEnginePreview = create<EnginePreviewStore>((): EnginePreviewStore => ({
  isOpen: false,
  isChecking: false,
  isBusy: false,
  status: null,
  result: null,
  capturedAtMs: null,
  roundTripSeconds: null,
  error: null,
}));

export function setEnginePreviewOpen(isOpen: boolean): void {
  useEnginePreview.setState({ isOpen });
}

function requestPreviewEvent(
  bridge: Bridge,
  request: PreviewRequest,
  kind: PreviewEventKind,
  timeoutMs: number,
): Promise<PreviewReply> {
  return new Promise<PreviewReply>((resolve, reject): void => {
    let requestId: number | null = null;
    let earlyReply: PreviewReply | null = null;
    const finish = (reply: PreviewReply): void => {
      if (requestId === null) {
        earlyReply = reply;
        return;
      }
      if (reply.requestId !== requestId) return;
      clearTimeout(timer);
      unsubscribe();
      resolve(reply);
    };
    // Subscribe before requesting: a fast worker may finish before request() resolves.
    const unsubscribe: () => void = bridge.on(kind, (event: EventOf<PreviewEventKind>): void => finish(event.payload));
    const timer: ReturnType<typeof setTimeout> = setTimeout((): void => {
      unsubscribe();
      reject(new Error("Engine preview timed out. Retry the status check."));
    }, timeoutMs);
    const send = async (): Promise<void> => {
      try {
        const pending: EnginePreviewPendingDto = await bridge.request(request);
        const hasPendingRequest: boolean =
          pending.status === "pending" && Number.isSafeInteger(pending.requestId) && pending.requestId > 0;
        if (!hasPendingRequest) throw new Error("Invalid engine preview pending reply.");
        requestId = pending.requestId;
        if (earlyReply !== null) finish(earlyReply);
      } catch (error) {
        clearTimeout(timer);
        unsubscribe();
        reject(error);
      }
    };
    void send();
  });
}

export async function checkEnginePreviewStatus(bridge: Bridge, pid?: number): Promise<void> {
  const state: EnginePreviewStore = useEnginePreview.getState();
  if (state.isChecking || state.isBusy) return;
  useEnginePreview.setState({ isChecking: true, error: null });
  try {
    const status: PreviewReply = await requestPreviewEvent(
      bridge,
      { kind: "enginePreview/status", params: pid === undefined ? {} : { pid } },
      ENGINE_PREVIEW_STATUS_CHECKED_EVENT,
      STATUS_TIMEOUT_MS,
    );
    if (!("state" in status)) throw new Error("Invalid engine preview status reply.");
    useEnginePreview.setState({ status, error: status.error ?? null });
  } catch (error) {
    useEnginePreview.setState({ error: error instanceof Error ? error.message : String(error) });
  } finally {
    useEnginePreview.setState({ isChecking: false });
  }
}

export async function captureEnginePreview(bridge: Bridge, timeSeconds: number, matchCamera: boolean): Promise<void> {
  const state: EnginePreviewStore = useEnginePreview.getState();
  if (state.isChecking || state.isBusy) return;
  useEnginePreview.setState({ isBusy: true, error: null, result: null, capturedAtMs: null, roundTripSeconds: null });
  const requestedAtMs: number = Date.now();
  try {
    const result: PreviewReply = await requestPreviewEvent(
      bridge,
      { kind: "enginePreview/still", params: { timeSeconds, matchCamera } },
      ENGINE_PREVIEW_STILL_RENDERED_EVENT,
      STILL_TIMEOUT_MS,
    );
    if (!("ok" in result)) throw new Error("Invalid engine preview still reply.");
    if (!result.ok) {
      useEnginePreview.setState({ error: result.error });
      return;
    }
    const capturedAtMs: number = Date.now();
    useEnginePreview.setState({
      result,
      capturedAtMs,
      roundTripSeconds: (capturedAtMs - requestedAtMs) / MILLISECONDS_PER_SECOND,
    });
  } catch (error) {
    useEnginePreview.setState({ error: error instanceof Error ? error.message : String(error) });
  } finally {
    useEnginePreview.setState({ isBusy: false });
  }
}

export async function cancelEnginePreview(bridge: Bridge): Promise<void> {
  try {
    await bridge.request({ kind: "enginePreview/cancel", params: {} });
  } catch (error) {
    useEnginePreview.setState({ error: error instanceof Error ? error.message : String(error) });
  }
}
