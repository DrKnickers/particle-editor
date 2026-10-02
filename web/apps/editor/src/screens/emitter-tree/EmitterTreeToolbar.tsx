import * as Menubar from "@radix-ui/react-menubar";
import { ChevronDown, ChevronUp, Copy, Eye, EyeOff, Plus, Trash2 } from "lucide-react";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { announceWhenOk } from "@/lib/status-feedback";
import { cn } from "@/lib/utils";
import { IconButton, iconButtonClass } from "@/primitives/IconButton";
import { MENU_CONTENT, MENU_ITEM } from "@/primitives/menu";
import { Tip } from "@/primitives/Tip";
import { requestDeleteEmitters } from "@/lib/tree/delete-emitters";
import { moveEmitters, duplicateEmitters } from "@/lib/tree/emitter-reorder";
import { canMoveSelection } from "@/lib/tree/move-enabled";

// ─── Panel-header toolbar ────────────────────────────────────────────
// Restores the legacy Win32 editor's emitter-list toolbar, in legacy order:
//   [New ▾] [Duplicate] [Delete] [▲ Move Up] [▼ Move Down] [Show All] [Hide All]
// Every button uses an existing bridge call.

// The footer buttons are IconButton's "tree" variant (28px square, matching
// the main toolbar's `.tb-btn` height); the New Emitter dropdown wears the
// shared menu styling (primitives/menu.ts).

function findNodeInTree(
  tree: EmitterTreeDto | null,
  id: number | null,
): { node: EmitterTreeNode; siblings: EmitterTreeNode[]; indexInSiblings: number } | null {
  if (tree === null || id === null) return null;
  const walk = (
    siblings: EmitterTreeNode[],
  ): ReturnType<typeof findNodeInTree> => {
    for (let i = 0; i < siblings.length; i++) {
      const n = siblings[i];
      if (n.id === id) return { node: n, siblings, indexInSiblings: i };
      const hit = walk(n.children);
      if (hit !== null) return hit;
    }
    return null;
  };
  return walk(tree.root.children);
}

type ToolbarProps = {
  bridge: Bridge;
  tree: EmitterTreeDto | null;
  primaryId: number | null;
};

export function EmitterTreeToolbar({ bridge, tree, primaryId }: ToolbarProps) {
  const primary = findNodeInTree(tree, primaryId);
  const hasPrimary = primary !== null;
  const selIds = useEmitterSelectionStore((s) => s.ids);
  // Lifetime/Death child adds require both a primary AND a free slot
  // (parents can hold at most one of each role).
  const canAddLifetime =
    hasPrimary && !primary!.node.children.some((c) => c.role === "lifetime");
  const canAddDeath =
    hasPrimary && !primary!.node.children.some((c) => c.role === "death");
  // Move is a root-only operation — same gate as the per-row context
  // menu. Sibling reordering at lifetime/death depth is a separate
  // capability not exposed by the legacy panel toolbar either.
  // Move enabled state mirrors move-many's preserve rule, via the shared
  // helper (same predicate the row context-menu Move items use).
  const rootIds = (tree?.root.children ?? []).map((c) => c.id);
  const canMoveUp = canMoveSelection(selIds, rootIds, "up");
  const canMoveDown = canMoveSelection(selIds, rootIds, "down");

  const addRoot = () =>
    announceWhenOk(bridge.request({ kind: "emitters/add-root", params: {} }), "Added emitter — Ctrl+Z to undo");
  const addLifetime = () => {
    if (primaryId === null) return;
    announceWhenOk(bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: primaryId },
    }), "Added lifetime child — Ctrl+Z to undo");
  };
  const addDeath = () => {
    if (primaryId === null) return;
    announceWhenOk(bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: primaryId },
    }), "Added death child — Ctrl+Z to undo");
  };
  const duplicatePrimary = () => {
    // Toolbar Duplicate is selection-aware (like the trash button); the new
    // copies become the selection.
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    announceWhenOk(duplicateEmitters(bridge, ids) as Promise<unknown>, `Duplicated ${ids.length === 1 ? "emitter" : `${ids.length} emitters`} — Ctrl+Z to undo`);
  };
  const del = () => {
    // Selection-aware: the trash button deletes the WHOLE multi-selection
    // (matching right-click → Delete and the Delete key), not just the
    // primary row. ids and primary stay in sync, so an empty selection ==
    // no primary == nothing to delete.
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    requestDeleteEmitters(bridge, ids);
  };
  const moveUp = () => {
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    void moveEmitters(bridge, ids, "up");
  };
  const moveDown = () => {
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    void moveEmitters(bridge, ids, "down");
  };
  const showAll = () =>
    void bridge.request({
      kind: "emitters/set-all-visible",
      params: { visible: true },
    });
  const hideAll = () =>
    void bridge.request({
      kind: "emitters/set-all-visible",
      params: { visible: false },
    });

  return (
    <div
      data-testid="emitter-tree-toolbar"
      className="tree-actions"
    >
      <Menubar.Root>
        <Menubar.Menu>
          <Tip content="New Emitter">
            <Menubar.Trigger
              className={iconButtonClass("tree")}
              aria-label="New Emitter"
            >
              <Plus className="size-4" />
            </Menubar.Trigger>
          </Tip>
          <Menubar.Portal>
            <Menubar.Content
              className={cn(MENU_CONTENT, "min-w-[160px]")}
              align="start"
              sideOffset={4}
            >
              <Menubar.Item
                onSelect={addRoot}
                className={MENU_ITEM}
                data-testid="new-emitter-root"
              >
                Root Emitter
              </Menubar.Item>
              <Menubar.Item
                onSelect={addLifetime}
                disabled={!canAddLifetime}
                className={MENU_ITEM}
                data-testid="new-emitter-lifetime-child"
              >
                Lifetime Child
              </Menubar.Item>
              <Menubar.Item
                onSelect={addDeath}
                disabled={!canAddDeath}
                className={MENU_ITEM}
                data-testid="new-emitter-death-child"
              >
                Death Child
              </Menubar.Item>
            </Menubar.Content>
          </Menubar.Portal>
        </Menubar.Menu>
      </Menubar.Root>
      {/* tipWhenDisabled: disabled buttons fire no pointer events, so the Tip
          listens on a wrapping span that stays interactive while the button
          inside is disabled. */}
      <IconButton
        variant="tree"
        label="Duplicate emitter"
        tip="Duplicate"
        tipWhenDisabled
        data-testid="emitter-duplicate-btn"
        disabled={!hasPrimary}
        onClick={duplicatePrimary}
      >
        <Copy className="size-4" />
      </IconButton>
      <IconButton variant="tree" label="Delete emitter" tip="Delete" tipWhenDisabled disabled={!hasPrimary} onClick={del}>
        <Trash2 className="size-4" />
      </IconButton>
      <IconButton variant="tree" label="Move emitter up" tip="Move Up" tipWhenDisabled disabled={!canMoveUp} onClick={moveUp}>
        <ChevronUp className="size-4" />
      </IconButton>
      <IconButton variant="tree" label="Move emitter down" tip="Move Down" tipWhenDisabled disabled={!canMoveDown} onClick={moveDown}>
        <ChevronDown className="size-4" />
      </IconButton>
      <IconButton variant="tree" label="Show all emitters" tip="Show All Emitters" onClick={showAll}>
        <Eye className="size-4" />
      </IconButton>
      <IconButton variant="tree" label="Hide all emitters" tip="Hide All Emitters" onClick={hideAll}>
        <EyeOff className="size-4" />
      </IconButton>
    </div>
  );
}
