// Typed access to `window.bridge` for the end-to-end specs.
//
// The app installs `window.bridge` in src/bridge/expose.ts. Under --test-host
// it is a TestHostBridge (src/bridge/test-host.ts), which forwards every
// request verbatim to the C++ dispatcher — so the native specs can also reach
// the handful of test-host-only kinds (and test-only params on production
// kinds) that the host answers but the app schema deliberately leaves out (no
// production caller). Those are typed here from their C++ handlers (src/host/);
// everything else comes from @particle-editor/bridge-schema.
import type { Page } from "@playwright/test";
import type { Bridge, Request, ResponseFor } from "@particle-editor/bridge-schema";

// BridgeDispatch_Engine.cpp — device-recovery work seam (d3d9ex.spec.ts).
export type DeviceRecoveryWorkParams =
  | { action?: "query" | "arm" | "release" | "clear-query-result" }
  | { action: "inject-query-result"; result: "s-ok" | "s-false" | "device-lost"; repeatCount?: number };

export type DeviceRecoveryWorkState = {
  pending: boolean;
  reloadCount: number;
  authoredApplyCount: number;
  deviceProbeCount: number;
  composedFramePrepareCount: number;
  endFrameQueryCreateCount: number;
  endFrameQueryFailureCount: number;
  endFrameQueryTimeoutCount: number;
  endFrameQueryOverrideConsumedCount: number;
  endFrameQueryOverrideRemaining: number;
  frameReady: boolean;
};

// BridgeDispatch_File.cpp — undo byte-budget seam (undo-navigation.spec.ts).
export type UndoBudgetState = {
  maxTotalBytes: number;
  totalBytes: number;
  depth: number;
  cursor: number;
};

export type TestHostRequest =
  | { kind: "debug/device-recovery-work";     params: DeviceRecoveryWorkParams }
  | { kind: "undo/test/budget";               params: { maxTotalBytes?: number } }
  // BridgeDispatch_File.cpp — document-replacement seams (selection-identity.spec.ts).
  | { kind: "debug/seed-autosave-recovery";   params: { path: string; originalFilename?: string } }
  | { kind: "debug/emit-document-replaced";   params: Record<string, never> }
  // BridgeDispatch_File.cpp — test-host opt-ins on the production autosave kinds.
  | { kind: "autosave/check-recovery";        params: { __testAllowRecovery: boolean } }
  | {
      kind: "autosave/recover";
      params: {
        choice: "recent" | "stable" | "discard";
        __testAllowRecovery?: boolean;
        __testCorruptHandoffCandidate?: boolean;
      };
    }
  // BridgeDispatch_Spawner.cpp — the --record cursor-preview seam.
  | { kind: "preview/attach";                 params: { x: number; y: number } }
  | { kind: "preview/place";                  params: Record<string, never> }
  | { kind: "preview/kill";                   params: Record<string, never> };

type TestHostResponseFor<R extends TestHostRequest> =
  R extends { kind: "debug/device-recovery-work" } ? DeviceRecoveryWorkState :
  R extends { kind: "undo/test/budget" }           ? UndoBudgetState :
  // Production kinds with test-only params answer with the production shape.
  R extends { kind: infer K extends Request["kind"] } ? ResponseFor<Extract<Request, { kind: K }>> :
  Record<string, never>;

export type E2ERequest = Request | TestHostRequest;

export type E2EResponseFor<R extends E2ERequest> =
  R extends Request ? ResponseFor<R> :
  R extends TestHostRequest ? TestHostResponseFor<R> :
  never;

export type E2EBridge = Omit<Bridge, "request"> & {
  request<R extends E2ERequest>(req: R): Promise<E2EResponseFor<R>>;
};

declare global {
  interface Window {
    bridge?: E2EBridge;
  }
}

/** Issue one bridge request inside the page and return its typed response. */
export function bridgeRequest<R extends E2ERequest>(page: Page, req: R): Promise<E2EResponseFor<R>>;
export function bridgeRequest(page: Page, req: E2ERequest): Promise<unknown> {
  return page.evaluate((r) => {
    if (!window.bridge) throw new Error("window.bridge is not installed");
    return window.bridge.request(r);
  }, req);
}
