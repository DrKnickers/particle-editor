import { useCallback, useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

// Inline-rename state. `editing.id` is the row currently in
// rename mode; `editing.value` is the live input value. The original
// name is captured at edit-start (`original`) so an empty-commit can
// revert without a tree re-fetch round-trip.
export type RenameEditingState = {
  id: number;
  value: string;
  original: string;
} | null;

type UseEmitterRenameOptions = {
  bridge: Bridge;
  restoreFocus: (id: number) => void;
};

export function useEmitterRename({ bridge, restoreFocus }: UseEmitterRenameOptions) {
  // Inline rename. Local component state because only the
  // tree owns both the focus target (each row's button) and the input
  // (mounted inside the row). One row at a time; null = no edit in
  // progress.
  const [editing, setEditing] = useState<RenameEditingState>(null);
  const editingRef = useRef<RenameEditingState>(null);
  useEffect(() => { editingRef.current = editing; }, [editing]);

  // [design pass B2, pre-PR fix] Rename commit/cancel unmounts the inline
  // input, which drops focus to <body> — return it to the edited row so
  // keyboard flow (F2 → Enter/Escape) continues where it was. Only when
  // focus actually fell to body: if the user clicked elsewhere mid-edit,
  // their focus target wins. Declared BEFORE the remount-restore effect
  // below, so on a commit-driven rebuild this targeted restore runs first
  // and that effect's body-check then no-ops.
  const lastEditIdRef = useRef<number | null>(null);
  useEffect(() => {
    if (editing !== null) {
      lastEditIdRef.current = editing.id;
      return;
    }
    const id = lastEditIdRef.current;
    if (id === null) return;
    lastEditIdRef.current = null;
    if (document.activeElement !== document.body) return;
    restoreFocus(id);
  }, [editing]);

  const beginEdit = useCallback((id: number, currentName: string) => {
    setEditing({ id, value: currentName, original: currentName });
  }, []);
  const setEditValue = useCallback((value: string) => {
    setEditing((cur) => (cur === null ? null : { ...cur, value }));
  }, []);
  const cancelEdit = useCallback(() => { setEditing(null); }, []);
  const commitEdit = useCallback(() => {
    const cur = editingRef.current;
    if (cur === null) return;
    const trimmed = cur.value.trim();
    // Empty name → silent revert (no bridge call). Matches legacy: an
    // empty rename was rejected at the TreeView level.
    // Unchanged name → still revert without firing the bridge call;
    // saves a wire round-trip on a no-op commit (e.g. F2 → Enter).
    if (trimmed.length === 0 || trimmed === cur.original) {
      setEditing(null);
      return;
    }
    void bridge.request({
      kind: "emitters/rename",
      params: { id: cur.id, name: trimmed },
    });
    setEditing(null);
  }, [bridge]);

  const handleRenameInputKeyDown = (e: ReactKeyboardEvent<HTMLInputElement>) => {
    // Stop the tree-level keyboard handler from snatching
    // Backspace / Enter / Esc / arrows from the input.
    e.stopPropagation();
    if (e.key === "Enter") {
      e.preventDefault();
      commitEdit();
    } else if (e.key === "Escape") {
      e.preventDefault();
      cancelEdit();
    }
  };

  const handleRenameInputBlur = () => {
    // Blur happens AFTER Enter / Esc handlers fire and toggle `editing` off;
    // the conditional in commitEdit makes a second commit a no-op. Safer to
    // always route through commitEdit on blur so click-outside works without
    // an explicit click handler.
    commitEdit();
  };

  return {
    editing,
    editingRef,
    beginEdit,
    setEditValue,
    handleRenameInputKeyDown,
    handleRenameInputBlur,
  };
}
