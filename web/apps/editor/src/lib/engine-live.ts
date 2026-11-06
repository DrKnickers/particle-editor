// Owns the live engine view's client state: whether the viewport shows the engine host's
// renderer, and why it last stopped. The host owns the stream and the fallback decision;
// EngineLiveBadge registers the one subscription that keeps this store current.
import { create } from "zustand";
import { type Bridge, type EngineLiveStateDto, type EngineLiveStateName } from "@particle-editor/bridge-schema";
import { checkEnginePreviewStatus, useEnginePreview } from "@/lib/engine-preview";

export interface EngineLiveStore {
  state: EngineLiveStateName;
  // Why the view stopped or could not start; cleared when a new start is requested.
  message: string | null;
}

export const useEngineLive = create<EngineLiveStore>((): EngineLiveStore => ({ state: "off", message: null }));

export function applyEngineLiveState(update: EngineLiveStateDto): void {
  useEngineLive.setState({ state: update.state, ...(update.message ? { message: update.message } : {}) });
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// Checks the engine host first when no check has passed yet, so one click starts the view.
export async function startEngineLive(bridge: Bridge): Promise<void> {
  useEngineLive.setState({ message: null });
  if (useEnginePreview.getState().status?.state !== "ready") await checkEnginePreviewStatus(bridge);
  const preview = useEnginePreview.getState();
  if (preview.status?.state !== "ready") {
    useEngineLive.setState({ message: preview.error ?? "The engine host is not ready." });
    return;
  }
  try {
    const reply: { state: EngineLiveStateName } = await bridge.request({ kind: "engineLive/start", params: {} });
    useEngineLive.setState({ state: reply.state });
  } catch (error) {
    useEngineLive.setState({ state: "off", message: errorMessage(error) });
  }
}

export async function stopEngineLive(bridge: Bridge): Promise<void> {
  try {
    await bridge.request({ kind: "engineLive/stop", params: {} });
    useEngineLive.setState({ state: "off", message: null });
  } catch (error) {
    useEngineLive.setState({ message: errorMessage(error) });
  }
}
