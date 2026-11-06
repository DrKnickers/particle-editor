// EngineLiveBadge: a small pill at the viewport's top-left that says when the viewport shows
// the engine host's renderer instead of ours, and why it fell back. It owns the single
// subscription to `engineLive/state-changed`; the state itself lives in lib/engine-live.
// Mounted in PanelLayout's viewport quadrant beside OverloadBanner. Renders nothing while
// off with no message, so it never appears in runs without an engine host.
import { useEffect } from "react";
import { ENGINE_LIVE_STATE_CHANGED_EVENT, type Bridge, type EventOf } from "@particle-editor/bridge-schema";
import { applyEngineLiveState, useEngineLive, type EngineLiveStore } from "@/lib/engine-live";

// How long a fallback or start failure stays on screen.
const MESSAGE_VISIBLE_MS = 6_000;

export function EngineLiveBadge({ bridge }: { bridge: Bridge }): React.JSX.Element | null {
  const state = useEngineLive((store: EngineLiveStore) => store.state);
  const message = useEngineLive((store: EngineLiveStore) => store.message);

  // Mirrors the host's live-view state into the store.
  useEffect(
    (): (() => void) =>
      bridge.on(ENGINE_LIVE_STATE_CHANGED_EVENT, (event: EventOf<typeof ENGINE_LIVE_STATE_CHANGED_EVENT>): void =>
        applyEngineLiveState(event.payload),
      ),
    [bridge],
  );
  // Clears a fallback message after it has been readable for a while.
  useEffect((): (() => void) | undefined => {
    if (message === null) return undefined;
    const timer: ReturnType<typeof setTimeout> = setTimeout(
      (): void => useEngineLive.setState({ message: null }),
      MESSAGE_VISIBLE_MS,
    );
    return (): void => clearTimeout(timer);
  }, [message]);

  if (state === "off" && message === null) return null;
  let label: string = message ?? "";
  if (state === "starting") label = "Starting engine view…";
  if (state === "live") label = "Engine view";
  return (
    <div
      role="status"
      className="pointer-events-none absolute left-3 top-3 z-10 max-w-[60%] rounded-full bg-panel/90 px-3 py-1 text-xs text-text shadow"
    >
      {label}
    </div>
  );
}
