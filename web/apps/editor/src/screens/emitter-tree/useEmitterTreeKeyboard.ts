import { useCallback, type KeyboardEvent as ReactKeyboardEvent, type RefObject } from "react";
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { markEmittersCopied } from "@/lib/emitter-clipboard";
import { requestDeleteEmitters } from "@/lib/delete-emitters";
import { announceWhenOk } from "@/lib/status-feedback";
import type { RenameEditingState } from "./useEmitterRename";

type KeyboardRow = {
  node: EmitterTreeNode;
};

type UseEmitterTreeKeyboardOptions = {
  bridge: Bridge;
  flatRows: KeyboardRow[];
  orderedIds: number[];
  primaryId: number | null;
  editingRef: RefObject<RenameEditingState>;
  beginEdit: (id: number, currentName: string) => void;
  treeContainerRef: RefObject<HTMLDivElement | null>;
};

export function useEmitterTreeKeyboard({
  bridge,
  flatRows,
  orderedIds,
  primaryId,
  editingRef,
  beginEdit,
  treeContainerRef,
}: UseEmitterTreeKeyboardOptions) {
  const focusRowById = useCallback((id: number) => {
    if (treeContainerRef.current === null) return;
    const btn = treeContainerRef.current.querySelector(
      `[data-emitter-id="${id}"]`,
    ) as HTMLElement | null;
    btn?.focus();
  }, [treeContainerRef]);

  const handleTreeKeyDown = useCallback(
    (e: ReactKeyboardEvent<HTMLDivElement>) => {
      // Never steal keystrokes when the focus is in a text input
      // (inline-rename, spinners, modal text fields that might bubble).
      const target = e.target as HTMLElement | null;
      if (target !== null && target.tagName === "INPUT") return;
      // Editing mode disables the global keyboard nav so the input
      // owns all keys. (The input's onKeyDown stops propagation too;
      // belt + braces.)
      if (editingRef.current !== null) return;

      // Resolve the focused row id from the active element's
      // `data-emitter-id`, falling back to the React-side primary so
      // the first keypress lands somewhere sensible.
      const active = document.activeElement as HTMLElement | null;
      const activeIdStr = active?.getAttribute("data-emitter-id") ?? null;
      const focusedId = activeIdStr !== null ? Number.parseInt(activeIdStr, 10) : primaryId;
      const focusedIdx = focusedId !== null ? orderedIds.indexOf(focusedId) : -1;

      // Helpers — used by multiple branches below.
      const moveFocus = (nextIdx: number) => {
        if (nextIdx < 0 || nextIdx >= orderedIds.length) return;
        const nextId = orderedIds[nextIdx]!;
        e.preventDefault();
        useEmitterSelectionStore.getState().setSingle(nextId);
        void bridge.request({
          kind: "emitters/select",
          params: { id: nextId },
        });
        focusRowById(nextId);
      };

      if (e.key === "ArrowDown") {
        moveFocus(focusedIdx + 1);
        return;
      }
      if (e.key === "ArrowUp") {
        moveFocus(focusedIdx - 1);
        return;
      }
      if (e.key === "Home") {
        moveFocus(0);
        return;
      }
      if (e.key === "End") {
        moveFocus(orderedIds.length - 1);
        return;
      }
      if (e.key === "F2") {
        if (focusedId === null) return;
        const node = flatRows.find((r) => r.node.id === focusedId)?.node ?? null;
        if (node === null) return;
        e.preventDefault();
        beginEdit(focusedId, node.name);
        return;
      }
      if (e.key === "Delete") {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        // Descending-order delete + the destructive-confirm gate both live in
        // requestDeleteEmitters → performDelete now.
        requestDeleteEmitters(bridge, [...cur]);
        return;
      }
      // Ctrl+C / Ctrl+X / Ctrl+V on the focused tree. Cmd+* on macOS
      // routes through metaKey, same handler.
      const mod = e.ctrlKey || e.metaKey;
      if (mod && (e.key === "c" || e.key === "C")) {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        void bridge.request({ kind: "emitters/copy", params: { ids: cur } });
        markEmittersCopied();
        return;
      }
      if (mod && (e.key === "x" || e.key === "X")) {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        announceWhenOk(bridge.request({ kind: "emitters/cut", params: { ids: cur } }), `Cut ${cur.length === 1 ? "emitter" : `${cur.length} emitters`} — Ctrl+Z to undo`);
        markEmittersCopied();
        return;
      }
      if (mod && (e.key === "v" || e.key === "V")) {
        e.preventDefault();
        announceWhenOk(bridge.request({ kind: "emitters/paste", params: {} }), "Pasted — Ctrl+Z to undo");
        return;
      }
    },
    [bridge, beginEdit, editingRef, flatRows, focusRowById, orderedIds, primaryId],
  );

  return { handleTreeKeyDown };
}
