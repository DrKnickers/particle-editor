// Owns the host-session warning and its bridge subscription, not preview recovery.
import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

// This is a host-session condition, so it has no dismiss button or expiry.
// The host replays after page reload; bridges retain it for layout remounts.
export function ViewportUnavailableNotice({ bridge }: { bridge: Bridge }) {
  const [reason, setReason] = useState<string | null>(null);
  const [previousBridge, setPreviousBridge] = useState<Bridge>(bridge);
  if (previousBridge !== bridge) {
    setPreviousBridge(bridge);
    setReason(null);
  }
  // The host replays session failures when this bridge subscription is attached.
  useEffect(() => {
    return bridge.on("viewport/unavailable", (e) => setReason(e.payload.reason));
  }, [bridge]);

  if (reason === null) return null;
  return (
    <div
      role="alert"
      data-testid="viewport-unavailable-notice"
      className="banner-animate absolute inset-x-3 top-3 z-30 rounded-md border border-danger-fg bg-panel px-3 py-2 text-xs text-text"
    >
      <p className="font-semibold">3D preview unavailable</p>
      <p className="break-words">{reason}</p>
      <p className="text-text-2">
        You can still edit and save. Try updating your graphics driver. Details are in the log.
      </p>
    </div>
  );
}
