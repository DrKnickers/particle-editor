// Mock counterpart of src/host/BridgeDispatch_Shell.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import type { MockDispatchHost } from "./host";
import { EXTERNAL_LINKS } from "@/lib/external-links";

type ShellKind =
  | "stats/set-frozen"
  | "register-accelerators"
  | "app/open-external"
  | "app/quit"
  | "layout/viewport-rect"
  | "layout/scene-rect"
  | "animate-scene-rect"
  | "host/backing-color"
  | "viewport/capture-snapshot"
  | "viewport/input";

type ShellRequest = Extract<Request, { kind: ShellKind }>;

export function dispatchShell(this: MockDispatchHost, req: ShellRequest): unknown {
  switch (req.kind) {

    // ---------------- stats freeze (test-only knob) ----------------
    // Mock parity for native stats/set-frozen. Browser
    // mode emits no stats/tick, so freezing is largely no-op, but
    // emit the frozen-changed event for any consumer that listens.
    case "stats/set-frozen":
      this.emit({
        kind: "stats/frozen-changed",
        payload: { frozen: req.params.frozen },
      });
      return {};

    // ---------------- host plumbing: accepted no-ops ----------------
    case "register-accelerators":
      // Mock: nothing to register. Accelerator handling lives in the
      // native host; in browser mode the design iteration doesn't need
      // a real hotkey system, so swallow the call.
      return {};

    case "app/open-external": {
      const target = req.params.target;
      if (target !== "guide" && target !== "repository") {
        throw new Error("app/open-external: unknown target");
      }
      window.open(EXTERNAL_LINKS[target], "_blank", "noopener");
      return { opened: true };
    }

    case "app/quit":
      // Mock: no host window to close. In browser mode the design
      // iteration doesn't need a real "quit" — the dev server keeps
      // running. Accept the request silently.
      return {};

    case "layout/viewport-rect":
      // Mock: no native HWND to reposition.
      return {};

    case "layout/scene-rect":
      // Mock: no native AlphaCompositor to mask.
      return {};

    case "animate-scene-rect":
      // Mock: no native viewport-anim system (the host interpolates the
      // dock-slide rect under this architecture). Accept silently.
      return {};

    case "host/backing-color":
      // Mock: no native DComp backing visual to recolour.
      return {};

    case "viewport/capture-snapshot":
      // Mock: no engine to snapshot. Empty image + zero dims so the
      // React Modal's render guard (`snapshot && snapshot.imageBase64`)
      // short-circuits the <img> portal in unit tests.
      return { imageBase64: "", w: 0, h: 0 };

    case "viewport/input":
      // Mock: no native HWND to PostMessage to.
      // Tests assert on `dispatch` call args (kind + payload shape);
      // the return shape is the standard empty-object ack. Browser
      // mode never has an engine to drive, so this is a pure no-op.
      return {};

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
