// link-group-colors.ts — palette for the EmitterTree's link-group
// bracket gutter (Phase 3 Screen 4 Batch C).
//
// Mirrors the legacy `kBracketPalette` (visual port): 8 colours
// cycled by `linkGroup % 8`. Group 0 is "unlinked" and never gets a
// bracket — colorForGroup(0) returns null so callers know to skip
// rendering.
//
// Chosen for contrast against the tree's `bg-neutral-950` background
// AND visual distinction across adjacent groups. Hex literals rather
// than Tailwind class names so the renderer can drop them straight
// into inline `style` props (Tailwind purges unused arbitrary
// values, which would break per-group colours derived at runtime).

const BRACKET_PALETTE: readonly string[] = Object.freeze([
  "#38bdf8", // sky-400
  "#f472b6", // pink-400
  "#a78bfa", // violet-400
  "#facc15", // yellow-400
  "#34d399", // emerald-400
  "#fb923c", // orange-400
  "#f87171", // red-400
  "#22d3ee", // cyan-400
]);

/** Number of distinct colours cycled by `colorForGroup`. */
export const BRACKET_PALETTE_SIZE = BRACKET_PALETTE.length;

/** Returns the bracket colour for `linkGroup`. Returns null for
 *  group 0 (unlinked emitters never render a bracket). For non-zero
 *  groups, cycles through the 8-colour palette via `(group-1) % 8` —
 *  starting at 0 so group 1 → palette[0]. */
export function colorForGroup(group: number): string | null {
  if (group <= 0) return null;
  const idx = (group - 1) % BRACKET_PALETTE_SIZE;
  return BRACKET_PALETTE[idx] ?? null;
}

/** A bracket descriptor produced by `computeLinkGroupBrackets`.
 *  `lane` is 0-based — one DEDICATED lane per group (stable, ordered
 *  by groupId), so the renderer's horizontal offset for a group never
 *  changes between renders. `memberRowIndices` lists EVERY row in the
 *  group (ascending render order) so the renderer can draw a stub at
 *  each member, not just the first/last caps. */
export type LinkGroupBracket = {
  groupId: number;
  color: string;
  firstRowIndex: number;
  lastRowIndex: number;
  memberRowIndices: number[];
  lane: number;
};

export function computeLinkGroupBrackets<T extends { linkGroup: number }>(
  rows: ReadonlyArray<T>,
): LinkGroupBracket[] {
  // Pass 1: collect EVERY member row index per group, in render order.
  const members = new Map<number, number[]>();
  rows.forEach((row, idx) => {
    const g = row.linkGroup;
    if (g <= 0) return;
    const arr = members.get(g);
    if (arr === undefined) members.set(g, [idx]);
    else arr.push(idx);
  });

  // Pass 2: emit descriptors for groups with ≥ 2 members. firstRow /
  // lastRow come from the (ascending) member list; memberRowIndices
  // carries the full set so the renderer can stub each member.
  const descriptors: Omit<LinkGroupBracket, "lane">[] = [];
  members.forEach((indices, groupId) => {
    if (indices.length < 2) return;
    const color = colorForGroup(groupId);
    if (color === null) return;
    descriptors.push({
      groupId,
      color,
      firstRowIndex: indices[0]!,
      lastRowIndex: indices[indices.length - 1]!,
      memberRowIndices: indices,
    });
  });

  // Pass 3: ONE dedicated lane per group, stable by groupId (ascending).
  // No reuse across non-overlapping groups — each group keeps its own
  // column, so a group's lane never changes between renders (the
  // "bouncing gutter" ROADMAP called out). Gutter width grows with
  // the number of groups (laneCount === #groups).
  descriptors.sort((a, b) => a.groupId - b.groupId);
  return descriptors.map((d, lane) => ({ ...d, lane }));
}

/** Number of lanes used by the given bracket set. The gutter
 *  renderer multiplies this by `LANE_WIDTH_PX` (+ a small left pad)
 *  to size its container. Returns 0 for an empty set so the
 *  renderer can collapse the gutter to its minimum width. */
export function laneCount(brackets: ReadonlyArray<LinkGroupBracket>): number {
  let max = 0;
  brackets.forEach((b) => {
    if (b.lane >= max) max = b.lane + 1;
  });
  return max;
}
