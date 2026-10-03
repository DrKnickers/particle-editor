// Mock counterpart of src/host/BridgeDispatch_File.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import {
  makeDefaultEngineState,
  useMockEngineState,
  useMockRecentFiles,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type FileKind =
  | "autosave/check-recovery"
  | "autosave/recover"
  | "file/new"
  | "file/pick-open"
  | "file/open"
  | "file/save"
  | "file/save-as"
  | "file/recent/list";

type FileRequest = Extract<Request, { kind: FileKind }>;

export function dispatchFile(this: MockDispatchHost, req: FileRequest): unknown {
  switch (req.kind) {

    // ---------------- autosave crash-recovery ----------------
    case "autosave/check-recovery":
      // Mock: no %TEMP% scan in browser mode — always "no orphan", so the
      // recovery dialog never appears under `pnpm dev` / web vitest.
      // Component tests that exercise the dialog inject an orphan directly
      // (they render AutosaveRecoveryDialog, not the whole shell).
      return { orphan: null };

    case "autosave/recover":
      // Mock: no document to swap. Report success per the chosen action.
      return { status: req.params?.choice === "discard" ? "discarded" : "recovered" };

    // ---------------- file ops ----------
    //
    // The mock implementations are deliberately UI-free: there's no
    // real picker, no on-disk read/write. They simulate the host's
    // observable side-effects (currentFilePath, dirty, recentFiles)
    // so React handlers + Playwright specs can exercise the round
    // trip in browser mode. The schema-level contract (return shapes,
    // event ordering) matches the native host.
    //
    // Two historical callers also depend on this:
    //   - BackgroundPopoverBody chains file/open → set skydome-custom-path
    //     → set skydome-slot. In browser mode (with no real picker)
    //     the call still resolves with ok:false so the chain aborts
    //     cleanly without surfacing a raw rejection. The signal that
    //     "this is a fake/cancelled pick" is the lack of `path`.

    case "file/new":
      // Mirror the native host's data-loss guard: a dirty document is only
      // replaced once the user chose Don't Save (discardUnsaved).
      if (snapshotEngineState().dirty && !req.params?.discardUnsaved) {
        return { ok: false, error: "unsaved-changes" };
      }
      // Reset engine state to defaults, clear currentFilePath, clear
      // dirty. Emit dirty/changed (always — markClean dedupes on
      // already-clean, but file/new from a clean state may still
      // need to fire if anything else listens to "I just made a new
      // file"). For consistency: only emit if there was a change.
      useMockEngineState.getState().applyPatch({
        ...makeDefaultEngineState(),
      });
      this.markClean();
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      // Legacy parity: the default root (id 0) is selected after New. The
      // snapshot (above) carries it, but EmitterTree tracks selection via the
      // `emitters/selected` event (it doesn't re-read the snapshot post-mount),
      // so fire it explicitly — mirrors the native host's file/new.
      this.emit({ kind: "emitters/selected", payload: { id: 0 } });
      return {};

    case "file/pick-open":
      // Non-mutating picker: no native dialog in browser mode, so report no path
      // acquired (callers branch on `ok`). Never touches document state.
      return { ok: false, error: "browser-mode" };

    case "file/open": {
      // If the caller passed a path explicitly (e.g. Recent Files), use it.
      // Otherwise we simulate a cancelled native picker — the picker
      // doesn't exist in browser mode. The contract callers branch on
      // `ok` so this is the cleanest signal of "no path acquired".
      // `req.params.filter` ("alo" | "skydome" | "ground") is accepted
      // for type-compat but ignored here: there's no native dialog to
      // re-filter, and the browser-mode return value is the same
      // regardless of which surface invoked the picker.
      //
      // Opening an .alo replaces the document, so the native host's
      // data-loss guard applies first (texture filters never touch it).
      if ((req.params?.filter ?? "alo") === "alo" &&
          snapshotEngineState().dirty && !req.params?.discardUnsaved) {
        return { ok: false, error: "unsaved-changes" };
      }
      const explicit = req.params?.path;
      if (!explicit) {
        return { ok: false, error: "browser-mode" };
      }
      this.commitFilePath(explicit);
      this.markClean();
      return { ok: true, path: explicit };
    }

    case "file/save": {
      const explicit = req.params?.path;
      const cur = snapshotEngineState().currentFilePath;
      const target = explicit ?? cur ?? "/mock/untitled.alo";
      this.commitFilePath(target);
      this.markClean();
      return { ok: true, path: target };
    }

    case "file/save-as": {
      // Always "open the picker" — mock answers with a fixed path so
      // tests can assert deterministic behaviour. The native host
      // calls GetSaveFileNameW.
      const target = "/mock/saved-as.alo";
      this.commitFilePath(target);
      this.markClean();
      return { ok: true, path: target };
    }

    case "file/recent/list":
      return { paths: useMockRecentFiles.getState().paths };

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
