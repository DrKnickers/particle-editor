import { useEffect, useRef } from "react";
import * as ContextMenu from "@radix-ui/react-context-menu";
import { Eye, EyeOff, TriangleAlert } from "lucide-react";
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { colorForGroup } from "@/lib/tree/link-group-colors";
import { formatChainWarning, type ChainWarning } from "@/lib/chain-load";
import { Tip } from "@/primitives/Tip";
import { ChainWarningTip } from "../ChainWarningTip";
import type { RenameEditingState } from "./useEmitterRename";
import { EmitterRowContextMenu } from "./EmitterRowContextMenu";
import type { FlatRow, DropIndicator } from "./types";

/** Map role → display glyph. Pure presentational — no role-specific
 *  behaviour wiring this batch. */
function roleGlyph(role: EmitterTreeNode["role"]): string {
  switch (role) {
    case "root":     return "●";
    case "lifetime": return "↻";
    case "death":    return "✕";
  }
}

/** Aria label for the role glyph; assistive tech reads this. */
function roleLabel(role: EmitterTreeNode["role"]): string {
  switch (role) {
    case "root":     return "root emitter";
    case "lifetime": return "lifetime child";
    case "death":    return "death child";
  }
}

type RowProps = {
  row: FlatRow;
  primaryId: number | null;
  selectedIds: number[];
  orderedIds: number[];
  onRowClick: (id: number, mods: { ctrlKey: boolean; metaKey: boolean; shiftKey: boolean }) => void;
  bridge: Bridge;
  // [pointer-drag] wiring from the parent. The parent owns the drag
  // controller (startDrag); the row just initiates on pointerdown and
  // reads draggingId / indicator for its visual state.
  draggingId: number | null;
  // Ids of every row in the lifted multi-drag block (all dim while dragging).
  draggingIds: number[];
  indicator: DropIndicator;
  startDrag: (node: EmitterTreeNode, e: React.PointerEvent) => void;
  // Inline rename. `editing.id === node.id` means this row
  // renders an `<input>` instead of the label span. `beginEdit` starts
  // a new rename session against this row; `setEditValue` updates the
  // live value; `commitEdit` / `cancelEdit` end the session.
  editing: RenameEditingState;
  beginEdit: (id: number, currentName: string) => void;
  setEditValue: (value: string) => void;
  handleRenameInputKeyDown: (e: React.KeyboardEvent<HTMLInputElement>) => void;
  handleRenameInputBlur: () => void;
  // True when this row's link group is the one currently hovered —
  // the row paints a subtle tint so the user sees the whole group light up
  // together. `onHoverLinkGroup` reports this row's group on pointer enter
  // (null on leave) so the parent can drive the tint + bracket highlight.
  linkHover: boolean;
  onHoverLinkGroup: (groupId: number | null) => void;
  // Dissolve the entire link group this row belongs to.
  onDissolveLinkGroup: (groupId: number) => void;
  // Badge / spine click: select every member of this row's link group.
  onSelectLinkGroup: (groupId: number) => void;
  // non-null when this row sits on a chain whose estimated alive
  // count crosses the warning threshold (guard cap, or the 10k advisory).
  chainWarning: ChainWarning | null;
  // True on the render where this row first appears (add/
  // paste/duplicate) — the li wears .row-fade-in for its mount.
  entering: boolean;
};

export function EmitterRow({
  row, primaryId, selectedIds, orderedIds, onRowClick, bridge,
  draggingId, draggingIds, indicator, startDrag,
  editing, beginEdit, setEditValue, handleRenameInputKeyDown, handleRenameInputBlur,
  linkHover, onHoverLinkGroup, onDissolveLinkGroup, onSelectLinkGroup, chainWarning,
  entering,
}: RowProps) {
  const { node, depth, siblings } = row;
  const isPrimary = primaryId === node.id;
  const isSelected = selectedIds.includes(node.id);
  const isLinked = node.linkGroup !== 0;
  // Group hue (null when unlinked). `linkColor` is the raw colour used by
  // the always-on badge; `spineTintColor` is the same hue gated to the
  // spine + row-tint case (linked AND unselected — selection styling wins
  // otherwise). Computed once so the style object below doesn't re-call
  // colorForGroup several times per render.
  const linkColor = colorForGroup(node.linkGroup);
  const spineTintColor = isLinked && !isSelected ? linkColor : null;
  const isEditing = editing !== null && editing.id === node.id;
  const inputRef = useRef<HTMLInputElement | null>(null);

  // Auto-focus + select-all on the input the moment editing toggles to
  // this row. The ref binds in the same render where `isEditing` flips
  // to true; the effect fires post-mount.
  useEffect(() => {
    if (isEditing && inputRef.current !== null) {
      inputRef.current.focus();
      inputRef.current.select();
    }
  }, [isEditing]);

  // Indent by 12px per depth level.
  const indentPx = depth * 12;

  // Reference orderedIds so eslint-no-unused-vars in the trimmed file
  // doesn't complain when the only consumer is the parent's click
  // handler. Passing it through props keeps the row symmetric with the
  // tree-flatten output.
  void orderedIds;

  // ── drag/drop handlers ────────────────────────────────
  //
  // The row is both a drag source and a drop target. Drop semantics:
  //   - drop above/below a root → reorder (a make-room gap renders at the
  //     resolved root gap; see the parent's flatRows map)
  //   - drop on any row, middle third → reparent under target (this row's
  //     onto-ring, below)
  // The drag is driven by the parent's pointer-drag controller (startDrag,
  // wired to this row's button below); the row only renders the onto-ring
  // from `indicator` and initiates on pointerdown.

  // Dimmed while lifted: the grabbed row OR any row in the lifted block
  // (selected roots + all their descendants).
  const isDragging = draggingId === node.id || draggingIds.includes(node.id);
  // Reparent target visual: tint the row + ring when this row is the
  // single-drag "onto" target. (Reorder uses the make-room gap, not a ring.)
  const reparentTintClass =
    indicator?.kind === "onto" && indicator.targetId === node.id
      ? "bg-accent-soft ring-1 ring-accent"
      : "";

  // Selected-row styling:
  //   - primary       : strong sky-500 left border + sky-500/15 bg
  //   - non-primary   : softer sky-400/50 left border + sky-500/15 bg
  //   - unselected    : transparent border + hover bg
  const borderClass = isPrimary
    ? "border-accent"
    : isSelected
      ? "border-accent/50"
      : "border-transparent";
  const rowBgClass = isSelected
    ? "bg-accent-soft text-text"
    : "text-text-2 hover:bg-bg-2/40";
  const fontClass = isPrimary ? "font-medium" : "";

  return (
    <li
      // Presentational: `treeitem` and `aria-selected` live on the focusable
      // row DIV below, NOT here. Keyboard focus and the roving tabindex target
      // that div, so putting the role on this non-focusable <li> meant the
      // element a screen-reader user actually lands on had no role and no
      // selected state (WCAG 4.1.2). Role and focus must be the same element.
      role="none"
      // [glide] the FLIP pass measures + animates rows via this attribute;
      // stableId survives reorders (unlike the positional node.id).
      data-stable-id={node.stableId}
      className={entering ? "relative row-fade-in" : "relative"}
    >
      {/* The reorder affordance is the "make room" gap — a flow spacer in the
          EmitterTree list (see the flatRows map), not a per-row overlay, so the
          rows shift to reveal where the dragged emitter(s) will land. This row
          only paints the reparent onto-ring (reparentTintClass, below). */}
      {/* Row surface is a focusable DIV, not a <button> —
          the visibility toggle inside is a real <button> now, and
          button-in-button is invalid HTML with ambiguous AT semantics.
          Roving tabindex: the PRIMARY row is the tree's single Tab stop
          (tabIndex 0); all other rows are -1 and reached via the
          container-level arrow-key handler, which focuses by
          [data-emitter-id]. Enter/Space activate (select) like the old
          button, guarded to the row itself so the inline-rename input
          and the eye button keep their own keys. (Comment sits OUTSIDE the
          Trigger: asChild requires exactly one child.) */}
      <ContextMenu.Root>
        <ContextMenu.Trigger asChild>
          <div
            // Role + selected state live HERE, on the element that actually
            // takes focus and carries the roving tabindex — see the <li>.
            role="treeitem"
            aria-selected={isSelected}
            tabIndex={isPrimary ? 0 : -1}
            onPointerDown={(e) => startDrag(node, e)}
            // Hovering a linked row lights up its whole group.
            onPointerEnter={() => onHoverLinkGroup(node.linkGroup || null)}
            onPointerLeave={() => onHoverLinkGroup(null)}
            onClick={(e) =>
              onRowClick(node.id, {
                ctrlKey: e.ctrlKey,
                metaKey: e.metaKey,
                shiftKey: e.shiftKey,
              })
            }
            onKeyDown={(e) => {
              if (e.target !== e.currentTarget) return;
              if (e.key === "Enter" || e.key === " ") {
                e.preventDefault();
                onRowClick(node.id, {
                  ctrlKey: e.ctrlKey,
                  metaKey: e.metaKey,
                  shiftKey: e.shiftKey,
                });
              }
            }}
            // Right-click also promotes a non-selected row to single-
            // select before the Radix menu opens — keeps menu state
            // consistent with what the user just targeted.
            onContextMenu={() => {
              const cur = useEmitterSelectionStore.getState().ids;
              if (!cur.includes(node.id)) {
                useEmitterSelectionStore.getState().setSingle(node.id);
                void bridge.request({
                  kind: "emitters/select",
                  params: { id: node.id },
                });
              }
            }}
            data-emitter-id={node.id}
            // --record clips target a row by its positional emitter index
            // (testid:emitter-row:<node.id>) — node.id is the same handle
            // emitters/move/select use, so it's stable within a render.
            data-testid={`emitter-row:${node.id}`}
            data-link-group={node.linkGroup}
            data-link-hover={linkHover ? "true" : "false"}
            data-selected={isSelected ? "true" : "false"}
            data-primary={isPrimary ? "true" : "false"}
            data-dragging={isDragging ? "true" : "false"}
            className={[
              "grid w-full items-center gap-1.5 py-0.5 pr-2 text-left text-sm transition-colors motion-reduce:transition-none",
              "focus-ring-inset",
              "border-l-2",
              borderClass,
              rowBgClass,
              reparentTintClass,
              fontClass,
              // Hovering a group tints its member rows. On linked
              // rows the inline group-tint (below) already covers this and
              // would override the class, so only apply the generic accent
              // tint where there's no inline tint (selected linked rows, or
              // an unlinked row sharing the hovered state).
              linkHover && spineTintColor === null ? "bg-accent/10" : "",
              // Lifted rows read distinctly from hidden ones (plain
              // opacity-50): dragging also desaturates. Dragging wins when
              // both apply.
              isDragging ? "opacity-40 saturate-50" : node.visible ? "" : "opacity-50",
            ].join(" ")}
            style={{
              paddingLeft: `${8 + indentPx}px`,
              // Visual columns: [eye | role-glyph | name]. The role glyph
              // (children only) sits in col 2; the name in col 3. The badge
              // (linked rows) is appended INLINE to the END of the name cell
              // — no own column. DOM order stays [eye, name(+badge), role];
              // badge is aria-hidden — so the accessibility tree (and the
              // emitter-tree a11y goldens) are unchanged.
              // Eye auto-places into column 1.
              // warned rows append a 16px col 4 for the chain-load
              // glyph; unwarned rows keep the 3-column template.
              gridTemplateColumns: chainWarning !== null
                ? "18px 18px 1fr 16px"
                : "18px 18px 1fr",
              // Spine: override the left-border colour to the group hue on
              // linked, unselected rows. Selected rows keep the accent border
              // (selection wins). Tint: soft full-row wash on the same rows —
              // 20% alpha while the group is hovered, 12% at rest (selected
              // rows fall through to the existing rowBgClass).
              ...(spineTintColor !== null
                ? {
                    borderLeftColor: spineTintColor,
                    backgroundColor: spineTintColor + (linkHover ? "33" : "1f"),
                  }
                : {}),
            }}
          >
            {/* Visibility toggle on the LEFT (replaces the old role
                dot). Always rendered so the grid columns stay stable
                during inline rename. */}
            {/* Real <button> now that the row wrapper is a
                div (was span[role=button] nested in a button — invalid).
                Native Enter/Space activation; keydown stops propagating so
                the row's own activation handler doesn't double-fire. Roving:
                tabbable only on the primary row (one extra stop past the
                row itself keeps the visibility control keyboard-reachable). */}
            <Tip
              content={node.visible ? "Hide emitter" : "Show emitter"}
              side="right"
            >
              <button
                type="button"
                tabIndex={isPrimary ? 0 : -1}
                data-testid={`emitter-vis-${node.id}`}
                onPointerDown={(e) => e.stopPropagation()}
                onClick={(e) => {
                  e.stopPropagation();
                  void bridge.request({
                    kind: "emitters/set-visible",
                    params: { id: node.id, visible: !node.visible },
                  });
                }}
                onKeyDown={(e) => e.stopPropagation()}
                aria-label={node.visible ? "Hide emitter" : "Show emitter"}
                className="grid place-items-center w-4 h-4 shrink-0 rounded text-text-3 transition-colors motion-reduce:transition-none hover:bg-panel-2 hover:text-text cursor-pointer focus-ring"
              >
                {node.visible
                  ? <Eye className="size-3" />
                  : <EyeOff className="size-3" />}
              </button>
            </Tip>
            {/* Name cell (col 3): the label (or inline-rename input) with the
                link dot appended INLINE at its right end on linked rows.
                Wrapping name + dot in one flex cell lets the dot sit at the
                END of the name text and reclaims the old reserved dot column. */}
            <div
              style={{ gridColumn: 3, gridRow: 1, minWidth: 0 }}
              className="flex items-center gap-1.5"
            >
              {isEditing ? (
                // Inline-rename input. Stops click + drag propagation so
                // typing doesn't accidentally re-trigger row selection /
                // drag-start. Commit on Enter, cancel on Esc; blur also
                // commits. Empty value reverts on commit (handled in the
                // parent's `commitEdit`).
                <input
                  ref={inputRef}
                  data-testid={`emitter-rename-input-${node.id}`}
                  value={editing!.value}
                  onChange={(e) => setEditValue(e.target.value)}
                  onKeyDown={handleRenameInputKeyDown}
                  onBlur={handleRenameInputBlur}
                  onClick={(e) => e.stopPropagation()}
                  onPointerDown={(e) => e.stopPropagation()}
                  onMouseDown={(e) => e.stopPropagation()}
                  onDoubleClick={(e) => e.stopPropagation()}
                  className="min-w-0 flex-1 rounded border border-accent bg-bg px-1 py-0 text-sm text-text outline-none"
                />
              ) : (
                <span
                  className="truncate min-w-0 flex-1"
                  data-emitter-name
                  onDoubleClick={(e) => {
                    // Double-click on the label starts inline rename. The
                    // stopPropagation prevents the click-handler chain
                    // above from re-firing single-select on the second
                    // click.
                    e.stopPropagation();
                    beginEdit(node.id, node.name);
                  }}
                >
                  {node.name}
                </span>
              )}
              {/* Badge: small group-number chip at the right end of the name
                  cell, coloured to the group. Shrinks-0 so it stays visible
                  even when the name truncates. Clicking selects the whole
                  group. */}
              {isLinked && (
                <Tip
                  content={`Select link group ${node.linkGroup}`}
                  side="right"
                >
                  <span
                    aria-hidden
                    data-testid={`emitter-link-badge-${node.id}`}
                    className="shrink-0 cursor-pointer rounded"
                    style={{
                      fontSize: 10,
                      padding: "2px 5px",
                      border: `1px solid ${linkColor ?? undefined}`,
                      color: linkColor ?? undefined,
                      background: "transparent",
                      pointerEvents: "auto",
                    }}
                    onPointerDown={(e) => e.stopPropagation()}
                    onClick={(e) => {
                      e.stopPropagation();
                      onSelectLinkGroup(node.linkGroup);
                    }}
                  >
                    {node.linkGroup}
                  </span>
                </Tip>
              )}
            </div>
            {/* Spawn-role glyph for child emitters (lifetime ↻ / on-death ✕),
                placed VISUALLY in column 2 (between the eye and the name) via
                grid-column. Rendered after the name cell in DOM so the
                accessible name stays "…default lifetime child" and the
                goldens are stable. Root rows omit it; column 2 then sits empty
                and the name stays in column 3. */}
            {node.role !== "root" && (
              <span
                aria-label={roleLabel(node.role)}
                style={{ gridColumn: 2, gridRow: 1 }}
                className="inline-block w-full shrink-0 text-center font-mono text-xs text-text-3"
              >
                {roleGlyph(node.role)}
              </span>
            )}
            {/* soft chain-load warning glyph, placed VISUALLY in
                col 5 (right of the name) via grid-column but rendered
                LAST in DOM — same convention as the role glyph / link
                dot, so unwarned rows' accessible names stay byte-
                identical and the a11y goldens hold. The rich
                ChainWarningTip carries the root→offender breakdown for
                sighted users; the aria-label keeps the FULL plain-text
                breakdown for screen readers. side="right" so
                tree tooltips open toward the viewport. */}
            {chainWarning !== null && (
              <Tip
                content={<ChainWarningTip warning={chainWarning} />}
                side="right"
              >
                <span
                  style={{ gridColumn: 4, gridRow: 1 }}
                  data-testid={`emitter-chain-warning-${node.id}`}
                  aria-label={formatChainWarning(chainWarning)}
                  className="grid place-items-center w-4 h-4 shrink-0 justify-self-center text-warning-fg"
                >
                  <TriangleAlert className="size-3" />
                </span>
              </Tip>
            )}
          </div>
        </ContextMenu.Trigger>
        <ContextMenu.Portal>
          {/* Mounted only while open, so the menu's derived state (clipboard,
              child slots, move/link enablement) costs nothing per row. */}
          <EmitterRowContextMenu
            node={node}
            siblings={siblings}
            selectedIds={selectedIds}
            bridge={bridge}
            beginEdit={beginEdit}
            onDissolveLinkGroup={onDissolveLinkGroup}
          />
        </ContextMenu.Portal>
      </ContextMenu.Root>
      {/* Spine hit-strip: 8px wide invisible strip over the row's left
          edge, so the 2px coloured border is easy to click. Rendered
          only on linked rows. Sits absolutely inside the relative <li>.
          Width 8px stays inside the gutter (eye toggle begins at
          paddingLeft ≥ 8px) so it never overlaps content. */}
      {isLinked && (
        <Tip
          content={`Select link group ${node.linkGroup}`}
          side="right"
        >
          <span
            aria-hidden
            data-testid={`emitter-link-spine-${node.id}`}
            className="cursor-pointer"
            style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: 8, pointerEvents: "auto" }}
            onPointerDown={(e) => e.stopPropagation()}
            onClick={(e) => {
              e.stopPropagation();
              onSelectLinkGroup(node.linkGroup);
            }}
          />
        </Tip>
      )}
    </li>
  );
}
