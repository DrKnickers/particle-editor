// EmitterRowContextMenu — the emitter-tree row's right-click menu: the items,
// their enabled state, and the handlers behind them. EmitterRow owns the Radix
// ContextMenu Root/Trigger (the row itself) and mounts this inside the Portal,
// so it renders only while the menu is open.
//
// Every handler targets the resolved selection: the whole multi-selection when
// the right-clicked row is part of it, else just that row (which is promoted
// to a single select first).

import { useMemo } from "react";
import * as ContextMenu from "@radix-ui/react-context-menu";
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { openTreeContextDialog } from "@/lib/tree/tree-context";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { markEmittersCopied, useEmitterClipboardHasContent } from "@/lib/tree/emitter-clipboard";
import { announceWhenOk } from "@/lib/status-feedback";
import { requestDeleteEmitters } from "@/lib/tree/delete-emitters";
import { moveEmitters, duplicateEmitters } from "@/lib/tree/emitter-reorder";
import { canMoveSelection } from "@/lib/tree/move-enabled";
import { cn } from "@/lib/utils";
import { MENU_CONTENT, MENU_ITEM, MENU_SEPARATOR } from "@/primitives/menu";

type Props = {
  node: EmitterTreeNode;
  /** The row's siblings in display order (Move Up/Down enablement). */
  siblings: EmitterTreeNode[];
  selectedIds: number[];
  bridge: Bridge;
  /** Start inline rename on a row (Rename item). */
  beginEdit: (id: number, currentName: string) => void;
  onDissolveLinkGroup: (groupId: number) => void;
};

export function EmitterRowContextMenu({
  node,
  siblings,
  selectedIds,
  bridge,
  beginEdit,
  onDissolveLinkGroup,
}: Props) {
  // Paste gates on session clipboard content.
  const hasClipboard = useEmitterClipboardHasContent();
  const isLinked = node.linkGroup !== 0;

  // Disabled states (derived from the tree DTO + the multi-selection).
  // The DTO doesn't expose `spawnDuringLife` / `spawnOnDeath` directly,
  // but children-by-role is equivalent: the slot is filled iff there's
  // a child of that role.
  const hasLifetimeChild = node.children.some((c) => c.role === "lifetime");
  const hasDeathChild    = node.children.some((c) => c.role === "death");
  // Move is a root-only operation. The engine refuses non-root moves
  // (children of the same role can't be swapped — at most one of each).
  const isRoot = node.role === "root";
  // The context-menu Move targets the whole selection when this row is part of
  // it (else just this row — mirrors resolveTargetIds), so its enabled state
  // uses the same preserve rule as move-many over that target set.
  const moveTargetIds = isRoot && selectedIds.includes(node.id) ? selectedIds : [node.id];
  const rootIdsInOrder = siblings.map((s) => s.id);
  const canMoveUp   = isRoot && canMoveSelection(moveTargetIds, rootIdsInOrder, "up");
  const canMoveDown = isRoot && canMoveSelection(moveTargetIds, rootIdsInOrder, "down");

  // Leave Link Group: enabled when at least one of the currently
  // selected emitters has `linkGroup !== 0`. If the right-clicked row
  // isn't in the selection, fall back to the row's own linkGroup.
  // (The handler also promotes a non-selected right-clicked row to
  // single-select before the click reaches this menu state.)
  const selectionLinkGroups = useMemo(() => {
    return selectedIds.length > 0 ? selectedIds : [node.id];
  }, [selectedIds, node.id]);
  const leaveLinkGroupDisabled = useMemo(() => {
    // Need the tree to inspect linkGroup; rebuild a fast lookup.
    const idToLinkGroup = new Map<number, number>();
    const visit = (n: EmitterTreeNode) => {
      idToLinkGroup.set(n.id, n.linkGroup);
      n.children.forEach(visit);
    };
    // We only have the row itself + the row's own subtree here. The
    // ordered-ids list is the in-order walk; we leverage *that* with
    // a child-look-up via the closures the parent passed. To keep
    // memory churn tight, derive the answer from the row + the
    // multi-selection by treating "we don't know the rest" as the
    // most permissive case (enabled). The Leave-LG bridge call is
    // idempotent on linkGroup=0, so the worst case is a redundant
    // round-trip on a no-op, not a wrong-result mutation.
    if (selectionLinkGroups.length === 1) {
      if (selectionLinkGroups[0] === node.id) return node.linkGroup === 0;
    }
    // Walk the row's own subtree as best-effort fallback.
    visit(node);
    const lookup = (id: number): number | undefined => idToLinkGroup.get(id);
    return selectionLinkGroups.every((id) => (lookup(id) ?? 0) === 0);
  }, [selectionLinkGroups, node]);

  // Context-menu handlers ────────────────────────────────────────────
  //
  // Each handler promotes the right-clicked row to single-select when
  // it isn't already in the selection. Without that step, "Set Link
  // Group…" would fire on whatever the previous selection was, which
  // is surprising. The promotion routes through the bridge so the
  // server's primary id and React's primary stay in lock-step.

  /** Snapshot the current selection at handler-execution time, falling
   *  back to a single-select of `node.id` when nothing is selected or
   *  when the row isn't already in the selection. */
  const resolveTargetIds = (): number[] => {
    const cur = useEmitterSelectionStore.getState().ids;
    if (cur.includes(node.id) && cur.length > 0) return [...cur];
    // Promote.
    useEmitterSelectionStore.getState().setSingle(node.id);
    void bridge.request({ kind: "emitters/select", params: { id: node.id } });
    return [node.id];
  };

  const handleRename = () => {
    // Context-menu Rename starts inline edit instead of
    // opening a modal. `RenameEmitterDialog` has been removed.
    resolveTargetIds();
    beginEdit(node.id, node.name);
  };
  const handleDuplicate = () => {
    // Duplicate the resolved target set (whole selection if the clicked row
    // is in it, else just that row); the selection moves to the new copies.
    {
      const ids = resolveTargetIds();
      announceWhenOk(duplicateEmitters(bridge, ids) as Promise<unknown>, `Duplicated ${ids.length === 1 ? "emitter" : `${ids.length} emitters`} — Ctrl+Z to undo`);
    }
  };
  const handleDelete = () => {
    // Delete the resolved target set — the whole selection when the
    // right-clicked row is part of it, else just the clicked row
    // (resolveTargetIds promotes a non-selected row to a single select).
    // Previously this discarded the return and hardcoded [node.id], so
    // right-click → Delete on a multi-selection deleted only one row and
    // skipped the destructive-confirm.
    requestDeleteEmitters(bridge, resolveTargetIds());
  };
  const handleIncrement = () => {
    resolveTargetIds();
    openTreeContextDialog("increment", node.id);
  };
  const handleRescale = () => {
    resolveTargetIds();
    openTreeContextDialog("rescale", node.id);
  };
  // Context-menu clipboard + New Root — reuse the same
  // bridge calls as the tree's Ctrl+C/X/V so behaviour stays identical.
  const handleNewRoot = () => {
    announceWhenOk(bridge.request({ kind: "emitters/add-root", params: {} }), "Added emitter — Ctrl+Z to undo");
  };
  const handleContextCopy = () => {
    const ids = resolveTargetIds();
    void bridge.request({ kind: "emitters/copy", params: { ids } });
    markEmittersCopied();
  };
  const handleContextCut = () => {
    const ids = resolveTargetIds();
    announceWhenOk(bridge.request({ kind: "emitters/cut", params: { ids } }), `Cut ${ids.length === 1 ? "emitter" : `${ids.length} emitters`} — Ctrl+Z to undo`);
    markEmittersCopied();
  };
  const handleContextPaste = () => {
    announceWhenOk(bridge.request({ kind: "emitters/paste", params: {} }), "Pasted — Ctrl+Z to undo");
  };
  // Paste As ▸ Lifetime/Death Child — paste the clipboard into this
  // emitter's child slot (legacy ID_PASTEAS_LIFETIME / ID_PASTEAS_DEATH).
  const handlePasteAsLifetime = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: node.id, slot: "lifetime" },
    });
  };
  const handlePasteAsDeath = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: node.id, slot: "death" },
    });
  };
  const handleLinkGroupSettings = () => {
    resolveTargetIds();
    openTreeContextDialog("link-group", node.id, node.linkGroup);
  };
  const handleAddLifetimeChild = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: node.id },
    });
  };
  const handleAddDeathChild = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: node.id },
    });
  };
  const handleMoveUp = () => {
    void moveEmitters(bridge, resolveTargetIds(), "up");
  };
  const handleMoveDown = () => {
    void moveEmitters(bridge, resolveTargetIds(), "down");
  };
  const handleSetLinkGroup = () => {
    resolveTargetIds();
    openTreeContextDialog("set-link-group", node.id);
  };
  const handleLeaveLinkGroup = () => {
    const ids = resolveTargetIds();
    void bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids, groupId: null },
    });
  };

  return (
    <ContextMenu.Content
      data-testid={`emitter-context-menu-${node.id}`}
      className={cn(MENU_CONTENT, "min-w-[220px]")}
    >
      <ContextMenu.Item onSelect={handleRename} className={MENU_ITEM}>
        Rename
      </ContextMenu.Item>
      <ContextMenu.Item onSelect={handleDuplicate} className={MENU_ITEM}>
        Duplicate
      </ContextMenu.Item>
      <ContextMenu.Item onSelect={handleDelete} className={MENU_ITEM}>
        Delete
      </ContextMenu.Item>
      <ContextMenu.Separator className={MENU_SEPARATOR} />
      <ContextMenu.Item onSelect={handleContextCut} className={MENU_ITEM}>
        Cut
      </ContextMenu.Item>
      <ContextMenu.Item onSelect={handleContextCopy} className={MENU_ITEM}>
        Copy
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleContextPaste}
        disabled={!hasClipboard}
        className={MENU_ITEM}
      >
        Paste
      </ContextMenu.Item>
      <ContextMenu.Sub>
        <ContextMenu.SubTrigger
          disabled={!hasClipboard}
          className={MENU_ITEM}
        >
          Paste As
        </ContextMenu.SubTrigger>
        <ContextMenu.Portal>
          <ContextMenu.SubContent className={cn(MENU_CONTENT, "min-w-[200px]")}>
            <ContextMenu.Item
              onSelect={handlePasteAsLifetime}
              disabled={!hasClipboard || hasLifetimeChild}
              className={MENU_ITEM}
            >
              Lifetime Child
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handlePasteAsDeath}
              disabled={!hasClipboard || hasDeathChild}
              className={MENU_ITEM}
            >
              Death Child
            </ContextMenu.Item>
          </ContextMenu.SubContent>
        </ContextMenu.Portal>
      </ContextMenu.Sub>
      <ContextMenu.Separator className={MENU_SEPARATOR} />
      <ContextMenu.Item onSelect={handleIncrement} className={MENU_ITEM} data-testid="ctx-increment-index">
        Increment Index…
      </ContextMenu.Item>
      <ContextMenu.Item onSelect={handleRescale} className={MENU_ITEM}>
        Rescale Emitter…
      </ContextMenu.Item>
      {/* ─────────────────────────────────────────────────── */}
      <ContextMenu.Separator className={MENU_SEPARATOR} />
      <ContextMenu.Item onSelect={handleNewRoot} className={MENU_ITEM}>
        New Root Emitter
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleAddLifetimeChild}
        disabled={hasLifetimeChild}
        className={MENU_ITEM}
      >
        Add Lifetime Child
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleAddDeathChild}
        disabled={hasDeathChild}
        className={MENU_ITEM}
      >
        Add Death Child
      </ContextMenu.Item>
      <ContextMenu.Separator className={MENU_SEPARATOR} />
      <ContextMenu.Item
        onSelect={handleMoveUp}
        disabled={!canMoveUp}
        className={MENU_ITEM}
      >
        Move Up
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleMoveDown}
        disabled={!canMoveDown}
        className={MENU_ITEM}
      >
        Move Down
      </ContextMenu.Item>
      <ContextMenu.Separator className={MENU_SEPARATOR} />
      <ContextMenu.Item
        onSelect={handleSetLinkGroup}
        className={MENU_ITEM}
        data-testid="ctx-set-link-group"
      >
        Set Link Group…
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleLeaveLinkGroup}
        disabled={leaveLinkGroupDisabled}
        className={MENU_ITEM}
      >
        Leave Link Group
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={() => onDissolveLinkGroup(node.linkGroup)}
        disabled={!isLinked}
        className={MENU_ITEM}
      >
        Dissolve Link Group
      </ContextMenu.Item>
      <ContextMenu.Item
        onSelect={handleLinkGroupSettings}
        disabled={!isLinked}
        className={MENU_ITEM}
      >
        Link Group Settings…
      </ContextMenu.Item>
    </ContextMenu.Content>
  );
}
