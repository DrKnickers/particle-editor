import type { EventKind, EventOf } from "@particle-editor/bridge-schema";
import { MockBridge } from "@/bridge/mock";

/** Create a real mock bridge with one listener; the caller owns unsubscription. */
export function makeMockBridgeWithListener<K extends EventKind>(
  kind: K,
  handler: (event: EventOf<K>) => void,
) {
  const b = new MockBridge();
  const off = b.on(kind, handler);
  return { b, off };
}
