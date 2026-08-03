// move-enabled.ts — shared "can this batch move do anything?" predicate, so
// the toolbar Move Up/Down buttons and the row context-menu Move items agree
// with what emitters/move-many actually does.
//
// Matches move-many's preserve rule: a move is effective iff at least one ROOT
// is in the target set AND the edge-most root in the move direction is NOT in
// the target (otherwise the block is pinned against the edge and the move
// freezes). With a single-id target this reduces to the old "not already at the
// edge" check, so single-emitter behaviour is unchanged.

/** @param targetIds the ids the move would act on (selection, or one row).
 *  @param rootIdsInOrder the root emitter ids, top-to-bottom. */
export function canMoveSelection(
  targetIds: number[],
  rootIdsInOrder: number[],
  direction: "up" | "down",
): boolean {
  if (rootIdsInOrder.length === 0) return false;
  const target = new Set(targetIds);
  if (!rootIdsInOrder.some((id) => target.has(id))) return false; // no root in target
  const edge =
    direction === "up"
      ? rootIdsInOrder[0]!
      : rootIdsInOrder[rootIdsInOrder.length - 1]!;
  return !target.has(edge);
}
