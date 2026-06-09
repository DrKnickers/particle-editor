// file-op.ts — wraps file/save|open|save-as so non-cancel failures surface
// in a single App-level error modal instead of being silently discarded.
// `bridge` is passed in (App.tsx:35 owns the only instance; it is a prop,
// not a module singleton). Touches only the error store, so it is callable
// from non-component code (use-app-accelerators.ts).
import { create } from "zustand";
import type { Bridge, Request, ResponseFor } from "@particle-editor/bridge-schema";

type FileOpErrorStore = {
  message: string | null;
  show: (message: string) => void;
  clear: () => void;
};
export const useFileOpErrorStore = create<FileOpErrorStore>((set) => ({
  message: null,
  show: (message) => set({ message }),
  clear: () => set({ message: null }),
}));

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
  const r = await bridge.request(req);
  if (!r.ok && r.error !== "user-cancelled") {
    useFileOpErrorStore.getState().show(messageFor(req.kind, r.error));
  }
  return r;
}
