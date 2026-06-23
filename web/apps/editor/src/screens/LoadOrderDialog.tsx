// LoadOrderDialog — build the ordered mod-layer stack (replaces SubmodsDialog).
// Available pane: the layer catalog grouped by parent, searchable. Load order pane:
// the working stack, numbered, reorder via ↑/↓ buttons (canonical/AT path) AND drag
// (the shared useStackReorder glide engine — same one the Mods dropdown uses); remove
// via ×; base game is implicit (a pinned, non-removable footer row). Apply dispatches
// mods/set-layers once and calls onApplied so the menu summary refreshes.
import { Fragment, useEffect, useState } from "react";
import { Search, Package, Check, Plus, GripVertical, ChevronUp, ChevronDown, X, Lock, Triangle, Layers } from "lucide-react";
import type { Bridge, LayerRef } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { cn } from "@/lib/utils";
import { useStackReorder } from "@/lib/use-stack-reorder";

type Props = { bridge: Bridge; open: boolean; onOpenChange: (open: boolean) => void; onApplied?: () => void };

const eqPath = (a: string, b: string) =>
  a.replace(/[\\/]+$/, "").toLowerCase() === b.replace(/[\\/]+$/, "").toLowerCase();
// Last non-empty path segment (splits on / and \). Used as the label fallback
// for an in-stack path absent from the catalog (matches MenuBar's basename).
const basename = (p: string): string => {
  const parts = p.split(/[\\/]+/).filter((s) => s.length > 0);
  return parts.length > 0 ? parts[parts.length - 1] : p;
};

export function LoadOrderDialog({ bridge, open, onOpenChange, onApplied }: Props) {
  const [catalog, setCatalog] = useState<LayerRef[]>([]);
  const [order, setOrder] = useState<string[]>([]);   // working stack (paths, front = top)
  const [query, setQuery] = useState("");

  // An unresolved in-stack path (absent from the catalog) falls back to its
  // basename rather than the full path, matching MenuBar's labelFor.
  const labelFor = (p: string) => catalog.find((l) => eqPath(l.path, p))?.label ?? basename(p);
  const inStack = (p: string) => order.some((o) => eqPath(o, p));
  const add = (p: string) => setOrder((o) => (inStack(p) ? o : [...o, p]));
  const remove = (i: number) => setOrder((o) => o.filter((_, j) => j !== i));
  const move = (i: number, dir: -1 | 1) => setOrder((o) => {
    const j = i + dir; if (j < 0 || j >= o.length) return o;
    const n = o.slice(); [n[i], n[j]] = [n[j], n[i]]; return n;
  });
  // Move `from` to insertion gap `target` (target === order.length → append).
  const reorder = (from: number, target: number) => setOrder((o) => {
    if (from < 0 || from >= o.length || from === target) return o;
    const n = o.slice();
    const [m] = n.splice(from, 1);
    const t = from < target ? target - 1 : target;
    n.splice(Math.max(0, Math.min(t, n.length)), 0, m);
    return n;
  });

  // Shared drag/glide engine (also powers the Mods dropdown's in-place reorder).
  const drag = useStackReorder({ order, labelFor, onReorder: reorder });

  useEffect(() => {
    // Abort any interrupted drag on open/close (a row can unmount mid-drag if the
    // modal is dismissed before pointerup), then load the catalog + current stack.
    drag.cancel();
    if (!open) return;
    let cancelled = false;
    setQuery("");
    bridge.request({ kind: "mods/list", params: {} }).then((r) => {
      if (cancelled) return;
      setCatalog(Array.isArray(r?.layers) ? r.layers : []);
      // Initialise the working order to the FULL incoming stack as-is — the host
      // already validated/ghost-dropped it on restore. Do NOT filter to only the
      // catalog: the catalog excludes mod roots without Data\Art (a migrated Mod
      // root, a MEG-packed mod), so dropping them here would silently erase those
      // layers on Apply (the data-loss bug this guards against).
      setOrder(Array.isArray(r?.stack) ? r.stack : []);
    }).catch((err) => console.warn("[LoadOrderDialog] mods/list failed:", err));
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, bridge]);

  const apply = () => { void bridge.request({ kind: "mods/set-layers", params: { paths: order } }); onApplied?.(); onOpenChange(false); };

  // Group the catalog by parent PATH (mods are their own group, keyed by their own
  // path; nested layers group under their owning mod's parentPath). Keying on the
  // stable path — not the display label — keeps two mods that share a label (same
  // nickname, or same folder name across corruption\Mods + GameData\Mods) in
  // separate groups instead of merging their nested layers. The header text still
  // uses the human label (parentLabel for nested, the mod's label otherwise).
  const groups: { key: string; label: string; items: LayerRef[] }[] = [];
  for (const l of catalog) {
    const key = l.kind === "nested" ? (l.parentPath ?? l.path) : l.path;
    const headerLabel = l.kind === "nested" ? (l.parentLabel ?? l.label) : l.label;
    let grp = groups.find((x) => eqPath(x.key, key));
    if (!grp) { grp = { key, label: headerLabel, items: [] }; groups.push(grp); }
    grp.items.push(l);
  }
  // Search filter (case-insensitive on the label); empty groups drop out.
  const q = query.trim().toLowerCase();
  const visibleGroups = q
    ? groups.map((g) => ({ ...g, items: g.items.filter((l) => l.label.toLowerCase().includes(q)) })).filter((g) => g.items.length > 0)
    : groups;

  const iconBtn = "flex size-5 shrink-0 items-center justify-center rounded-[var(--radius-xs)] text-text-2 hover:bg-hover hover:text-text disabled:pointer-events-none disabled:opacity-40 focus-ring";
  // Make-room gap spacer (matches EmitterTree): role=presentation so it's never a
  // load-order listitem; bg-accent-soft + inset sky-400 ring previews the landing.
  const gapSpacer = (
    <li
      aria-hidden
      role="presentation"
      className="pointer-events-none mx-0.5 rounded bg-accent-soft ring-1 ring-inset ring-accent"
      style={{ height: `${drag.gapHeight}px` }}
    />
  );

  return (
    <Modal open={open} onOpenChange={onOpenChange} title="Mod Load Order" size="lg">
      <Modal.Body>
        <p className="mb-3 text-[11px] leading-relaxed text-text-3">
          Top of the stack <span className="text-text-2">wins</span> on a shared file. Drag or use the
          arrows to reorder. <span className="text-text-2">Base game</span> is always the bottom layer.
        </p>
        <div className="flex items-stretch gap-3.5">

          {/* LEFT — available */}
          <div className="flex min-w-0 flex-1 flex-col">
            <div className="mb-[7px] text-[10px] font-semibold uppercase tracking-[0.06em] text-text-3">
              Available mods
            </div>
            <div className="mb-2 flex h-[var(--row-h)] items-center gap-1.5 rounded-[var(--radius-sm)] border border-border-2 bg-bg-3 px-2">
              <Search className="size-3 shrink-0 text-text-3" strokeWidth={1.5} />
              <input
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="Search mods…"
                aria-label="Search mods"
                className="min-w-0 flex-1 border-none bg-transparent text-xs text-text outline-none placeholder:text-text-3"
              />
            </div>
            <div className="flex flex-col gap-2">
              {visibleGroups.map((g) => (
                <div key={g.key}>
                  <div className="px-1.5 pb-1 text-[10px] font-semibold uppercase tracking-[0.06em] text-text-3">{g.label}</div>
                  <div className="flex flex-col gap-px">
                    {g.items.map((l) => {
                      const added = inStack(l.path);
                      const nested = l.kind === "nested";
                      return (
                        <div
                          key={l.path}
                          className={cn(
                            "flex h-[var(--row-h)] items-center gap-1.5 rounded-[var(--radius-xs)] pl-1.5 pr-1 text-xs hover:bg-hover",
                            nested && "ml-3",
                            added ? "text-text-3" : "text-text",
                          )}
                        >
                          <span className="flex w-3 shrink-0 justify-center text-text-3">
                            {!nested && <ChevronDown className="size-2.5" strokeWidth={1.8} />}
                          </span>
                          <Package className="size-3 shrink-0 text-text-3" strokeWidth={1.5} />
                          <span className="min-w-0 flex-1 truncate">{l.label}</span>
                          {added ? (
                            <span className="flex shrink-0 items-center gap-1 text-[10px] text-text-3">
                              <Check className="size-2.5 text-success-fg" strokeWidth={1.8} />
                              in stack
                            </span>
                          ) : (
                            <button
                              type="button"
                              aria-label={`Add ${l.label}`}
                              onClick={() => add(l.path)}
                              className="flex shrink-0 items-center gap-0.5 rounded-[var(--radius-xs)] px-1.5 py-0.5 text-[11px] font-semibold text-accent hover:bg-accent-soft focus-ring"
                            >
                              <Plus className="size-2.5" strokeWidth={1.8} />
                              add
                            </button>
                          )}
                        </div>
                      );
                    })}
                  </div>
                </div>
              ))}
              {visibleGroups.length === 0 && (
                <div className="px-1.5 py-2 text-[11px] text-text-3">No mods match “{query}”.</div>
              )}
            </div>
          </div>

          {/* direction divider */}
          <div className="w-px shrink-0 self-stretch bg-gradient-to-b from-transparent via-border-2 to-transparent" />

          {/* RIGHT — load order */}
          <div className="flex min-w-0 flex-1 flex-col">
            <div className="mb-[7px] flex items-center gap-[7px]">
              <span className="text-[10px] font-semibold uppercase tracking-[0.06em] text-text-3">Load order</span>
              <span className="inline-flex items-center gap-[3px] rounded-[9px] bg-accent-soft px-1.5 py-px text-[9px] font-semibold uppercase tracking-[0.04em] text-accent">
                <Triangle className="size-2 fill-current" strokeWidth={0} />
                top wins
              </span>
            </div>

            <div className="flex gap-2">
              {/* precedence rail */}
              <div className="my-0.5 w-[3px] shrink-0 rounded-[var(--radius-2xs)] bg-gradient-to-b from-accent via-accent-2 to-border-2" />

              <div className="flex min-w-0 flex-1 flex-col">
                {order.length === 0 ? (
                  <div className="mb-1.5 flex flex-col items-center justify-center gap-1.5 rounded-md border border-dashed border-border-2 px-3 py-[22px] text-center">
                    <Layers className="size-[22px] text-text-3" strokeWidth={1.3} />
                    <div className="text-xs text-text-2">No mods added</div>
                    <div className="text-[11px] text-text-3">Base game only — add mods from the left.</div>
                  </div>
                ) : (
                  <ul ref={drag.listRef} className="flex flex-col gap-[5px]" aria-label="Load order">
                    {order.map((p, i) => (
                      <Fragment key={p}>
                        {drag.gap === i && drag.dragIndex !== null && gapSpacer}
                        <li
                          data-flip-key={p}
                          onPointerDown={drag.startDrag(i)}
                          className={cn(
                            "relative flex h-[var(--row-h)] touch-none select-none items-center gap-[7px] rounded-[var(--radius-sm)] border border-border-2 bg-bg-3 pl-[5px] pr-1 text-xs",
                            drag.dragIndex === i ? "cursor-grabbing opacity-40 saturate-50" : "cursor-grab",
                          )}
                        >
                          <span className="flex w-[14px] shrink-0 items-center justify-center text-text-3" aria-hidden>
                            <GripVertical className="size-2.5" />
                          </span>
                          <span className="min-w-[13px] shrink-0 text-right text-[11px] font-semibold tabular-nums text-text-3" aria-hidden="true">{i + 1}</span>
                          <span className="min-w-0 flex-1 truncate text-text">{labelFor(p)}</span>
                          <div className="flex shrink-0 items-center gap-px">
                            <button type="button" aria-label={`Move ${labelFor(p)} up`} disabled={i === 0}
                                    onClick={() => move(i, -1)} className={iconBtn} title="Move up">
                              <ChevronUp className="size-3" strokeWidth={1.6} />
                            </button>
                            <button type="button" aria-label={`Move ${labelFor(p)} down`} disabled={i === order.length - 1}
                                    onClick={() => move(i, 1)} className={iconBtn} title="Move down">
                              <ChevronDown className="size-3" strokeWidth={1.6} />
                            </button>
                            <button type="button" aria-label={`Remove ${labelFor(p)}`} onClick={() => remove(i)}
                                    className={cn(iconBtn, "text-text-3 hover:text-danger-fg")} title="Remove">
                              <X className="size-3" strokeWidth={1.5} />
                            </button>
                          </div>
                        </li>
                      </Fragment>
                    ))}
                    {/* end gap (insert after the last row) */}
                    {drag.gap === order.length && drag.dragIndex !== null && gapSpacer}
                  </ul>
                )}

                {/* base game floor — pinned, non-removable */}
                <div className="mt-[5px] flex h-[var(--row-h)] items-center gap-2 rounded-[var(--radius-sm)] border border-dashed border-border-2 pl-1.5 pr-2 text-text-3">
                  <span className="flex w-[14px] shrink-0 items-center justify-center" aria-hidden>
                    <Lock className="size-[11px]" strokeWidth={1.4} />
                  </span>
                  <span className="min-w-0 flex-1 truncate text-xs">Base game</span>
                  <span className="shrink-0 text-[10px]">always last</span>
                </div>
              </div>
            </div>
          </div>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton onClick={() => onOpenChange(false)}>Cancel</Modal.CancelButton>
        <Modal.OkButton onClick={apply}>Apply</Modal.OkButton>
      </Modal.Footer>

      {drag.chipNode}
    </Modal>
  );
}
