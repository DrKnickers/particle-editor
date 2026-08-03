// file-op.ts — wraps file/save|open|save-as so non-cancel failures surface
// in a single App-level error modal instead of being silently discarded.
// `bridge` is passed in (App.tsx:35 owns the only instance; it is a prop,
// not a module singleton). Touches only the error store, so it is callable
// from non-component code (use-app-accelerators.ts).
import { create } from "zustand";
import type { Bridge, Request, ResponseFor } from "@particle-editor/bridge-schema";

type FileOpErrorStore = {
  message: string | null;
  // Optional title override. Errors use the modal's default ("Couldn't complete
  // that"); a non-error NOTICE (e.g. opened a model .alo) passes its own title.
  title: string | null;
  show: (message: string, title?: string) => void;
  clear: () => void;
};
export const useFileOpErrorStore = create<FileOpErrorStore>((set) => ({
  message: null,
  title: null,
  show: (message, title) => set({ message, title: title ?? null }),
  clear: () => set({ message: null, title: null }),
}));

// Shown after opening an .alo that holds no particle emitters — a model file
// loads into an empty editor, so without this the user gets no signal that the
// file simply has nothing to edit.
export const NO_EMITTERS_NOTICE =
  "This .alo has no particle emitters — it looks like a model, not a particle effect.";

export type FileOpReq = Extract<
  Request,
  { kind: "file/open" | "file/save" | "file/save-as" }
>;

const PREFIX: Record<FileOpReq["kind"], string> = {
  "file/open": "Couldn't open the file.",
  "file/save": "Couldn't save the file.",
  "file/save-as": "Couldn't save the file.",
};

// Generic host errors ("save failed" / "load failed") add no information
// beyond the prefix; anything else (a real path/permission message) is shown.
export function messageFor(kind: FileOpReq["kind"], error: string): string {
  const generic = error === "save failed" || error === "load failed" || error === "";
  return generic ? PREFIX[kind] : `${PREFIX[kind]}\n\n${error}`;
}

export async function runFileOp(
  bridge: Bridge,
  req: FileOpReq,
): Promise<ResponseFor<FileOpReq>> {
  let r: ResponseFor<FileOpReq>;
  try {
    r = await bridge.request(req);
  } catch (err) {
    // A REJECTED request (bridge not ready, transport error) bypasses the
    // {ok:false} path below — surface it in the same error store so the failure
    // is visible, then re-throw so the caller can keep its prompt open and not
    // run a destructive pending action (release-audit #11).
    useFileOpErrorStore.getState().show(messageFor(req.kind, String(err)));
    throw err;
  }
  if (!r.ok && r.error !== "user-cancelled") {
    useFileOpErrorStore.getState().show(messageFor(req.kind, r.error));
    return r;
  }
  // After a successful Open, surface a notice if the .alo holds no particle
  // emitters (a model file). The host has already loaded + bound the system by
  // the time file/open resolves, so emitters/list reflects the opened file.
  // Best-effort: a list failure must never block or fail the open.
  if (req.kind === "file/open" && r.ok) {
    try {
      const dto = await bridge.request({ kind: "emitters/list", params: {} });
      if (dto.root.children.length === 0) {
        useFileOpErrorStore
          .getState()
          .show(NO_EMITTERS_NOTICE, "No particle emitters");
      }
    } catch {
      /* non-fatal — skip the notice if the list can't be read */
    }
  }
  return r;
}
