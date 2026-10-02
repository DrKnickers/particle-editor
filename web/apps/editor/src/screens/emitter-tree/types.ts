import type { EmitterTreeNode } from "@particle-editor/bridge-schema";

/** Flatten the rendered tree into a depth-first list of `(node, depth,
 *  siblings, indexInSiblings)`. Used by both rendering and the shift-
 *  click range computation. */
export type FlatRow = {
  node: EmitterTreeNode;
  depth: number;
  siblings: EmitterTreeNode[];
  indexInSiblings: number;
};

// Drop indicator state. Owned at the EmitterTree level so only one row
// at a time displays a visual indicator. `targetId` is the row currently
// being hovered. `null` means no active drag-over. Both single and multi
// drags resolve geometrically (no live-DOM hovered-row semantics):
//   - "gap":  a make-room spacer at a resolved root gap (reorder — single
//             root or multi block); carries the lifted block's measured height.
//   - "onto": a reparent ring on a target row (single-drag only — drop onto
//             the middle third of a row to nest under it).
export type DropIndicator =
  | { kind: "gap"; gapIndex: number; gapHeight: number }
  | { kind: "onto"; targetId: number }
  | null;
