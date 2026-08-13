import { Fragment, useEffect, useState } from "react";
import * as Menubar from "@radix-ui/react-menubar";
import { Check, ChevronRight, X, Layers, GripVertical, Search, Plus } from "lucide-react";
import type { Bridge, LayerRef } from "@particle-editor/bridge-schema";
import { cn } from "@/lib/utils";
import { useStackReorder } from "@/lib/use-stack-reorder";
import { useFileOpErrorStore } from "@/lib/file-op";
import { runWhenIdle } from "@/lib/run-after-paint";
import { moveItemToGap, refreshModStack } from "@/lib/mod-stack";
import { basename, eqPath } from "@/lib/paths";
import { parseOpenPickerMessage, parsePoseDragMessage } from "@/lib/record-focus-bridge";
import { LoadOrderDialog } from "@/screens/LoadOrderDialog";

const MODS_MENU_VALUE = "mods";

// Small inline glyphs from the Mods-menu design (kept inline to avoid lucide
// name churn): a refresh arc, an expand-to-modal arrow, a filled "top wins"
// triangle.
const RefreshIcon = () => (
  <svg aria-hidden width="13" height="13" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth={1.4} strokeLinecap="round" strokeLinejoin="round"><path d="M13 8a5 5 0 1 1-1.5-3.5" /><path d="M13 2.5V5h-2.5" /></svg>
);
const ExpandIcon = () => (
  <svg aria-hidden width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth={1.4} strokeLinecap="round" strokeLinejoin="round"><path d="M9.5 2.5h4v4M13.5 2.5l-5 5M7 3.5H3.5a1 1 0 0 0-1 1v8a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V9" /></svg>
);
const TopWinsBadge = () => (
  <span aria-hidden className="inline-flex items-center gap-1 rounded-full bg-accent-soft px-1.5 py-px text-[9px] font-semibold uppercase tracking-wide text-accent">
    <svg width="9" height="9" viewBox="0 0 16 16" fill="currentColor"><path d="M8 3l5 6H3z" /></svg>
    top wins
  </span>
);

type ModsMenuProps = {
  bridge: Bridge;
  onMenuValueChange: (value: string) => void;
  triggerClass: string;
  contentClass: string;
  itemClass: string;
  separatorClass: string;
};

export function ModsMenu({
  bridge,
  onMenuValueChange,
  triggerClass: TRIGGER,
  contentClass: CONTENT,
  itemClass: ITEM,
  separatorClass: SEPARATOR,
}: ModsMenuProps) {
  // list of discovered mods, fetched separately from the
  // engine snapshot because it has a much lower change cadence (only
  // shifts on Refresh or disk mutation). The *active* mod is on the
  // snapshot so the menu's check mark stays reactive without a second
  // round-trip after a select.
  // The flat layer catalog (`layers` — mods with Data\Art + their nested
  // layers, what the Add mod… flyout lists) and the ordered content-layer stack
  // (`stack`, front = highest precedence). Both ride the mods/list payload (a
  // separate, low-cadence channel from the engine snapshot); we re-fetch after
  // every set-layers so the active-stack block + Add-mod checks stay current.
  const [layers, setLayers] = useState<LayerRef[]>([]);
  const [stack, setStack] = useState<string[]>([]);
  const [loadOrderOpen, setLoadOrderOpen] = useState(false);
  const [addQuery, setAddQuery] = useState(""); // Add mod… flyout search
  const applyModsResponse = (r: { layers?: unknown; stack?: unknown } | null | undefined) => {
    setLayers(Array.isArray(r?.layers) ? r.layers : []);
    setStack(Array.isArray(r?.stack) ? r.stack : []);
  };
  const refreshModsList = async () => {
    try {
      const r = await bridge.request({ kind: "mods/list", params: {} });
      // Defensive: a partial / mocked response that omits a field
      // shouldn't crash the menu. Fall back to empty defaults.
      applyModsResponse(r);
    } catch (err) {
      console.warn("[MenuBar] mods/list failed:", err);
    }
  };
  // Compare canonical paths case-insensitively, ignoring trailing slashes.
  const setLayerStack = async (paths: string[]) => {
    // Surface a failed apply instead of silently proceeding as if it worked
    // (release-audit #5). On failure the host did NOT persist the stack; we still
    // re-fetch so the menu reflects the host's ACTUAL state.
    try {
      const r = await bridge.request({ kind: "mods/set-layers", params: { paths } });
      if (!r.ok) {
        useFileOpErrorStore.getState().show(
          "error" in r && r.error
            ? `Couldn't apply the load order: ${r.error}`
            : "Couldn't apply the load order — the mod shaders failed to reload.",
          "Load order",
        );
      }
    } catch (err) {
      useFileOpErrorStore.getState().show(`Couldn't apply the load order: ${String(err)}`, "Load order");
    }
    // The snapshot carries activePath but not the full stack list, so re-fetch
    // mods/list to refresh the summary + per-layer checkmarks.
    await refreshModsList();
    // Also refresh the SHARED mod-stack store: its engine/state/changed
    // gate keys on activeModPath (the FRONT layer), so a same-front stack
    // edit (reorder/remove/append of a secondary layer) wouldn't reach it.
    refreshModStack();
  };
  const handleModRefresh = async () => {
    try {
      const r = await bridge.request({ kind: "mods/refresh", params: {} });
      applyModsResponse(r);
    } catch (err) {
      console.warn("[MenuBar] mods/refresh failed:", err);
    }
  };
  const labelFor = (p: string) =>
    layers.find((l) => eqPath(l.path, p))?.label ?? basename(p);
  // The menu composes the stack via TOGGLES (the modal owns ordering):
  // clicking a layer adds it (appended) if absent, or removes it if present.
  // toggle-add from an empty stack == the old quick-switch, but a composed
  // multi-layer stack is preserved instead of being replaced.
  const inStack = (p: string) => stack.some((s) => eqPath(s, p));
  const toggleLayer = (p: string) =>
    void setLayerStack(inStack(p) ? stack.filter((s) => !eqPath(s, p)) : [...stack, p]);
  const removeFromStack = (p: string) =>
    void setLayerStack(stack.filter((s) => !eqPath(s, p)));
  // In-menu stack reorder via the shared glide engine — live-commits
  // each reorder (the menu IS the editor; the modal is the "Expand" fallback). No
  // scroll container (the dropdown list is short; big stacks use Expand).
  const reorderStack = (from: number, target: number) => {
    const next = moveItemToGap(stack, from, target);
    if (next !== stack) void setLayerStack(next);
  };
  // --record only: a posed (frozen) drag for the mod stack, driven by ui/pose-drag.
  const [posedStackDrag, setPosedStackDrag] = useState<{ from: number; gap: number } | null>(null);
  const menuDrag = useStackReorder({
    order: stack,
    labelFor,
    onReorder: reorderStack,
    getScrollContainer: () => null,
    pose: posedStackDrag,
  });
  useEffect(() => {
    let cancelled = false;
    // Prime the mods list at mount — DEFERRED to the first idle slot after first
    // interactive paint (perf-audit P1a startup fan-out). Active mod arrives via
    // the eager snapshot; the list changes rarely and the live engine/state/changed
    // subscription above keeps it current, so the initial fetch is non-paint-critical.
    const cancelModsSeed = runWhenIdle(() => { if (!cancelled) void refreshModsList(); });
    return () => {
      cancelled = true;
      cancelModsSeed();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [bridge]);

  useEffect(() => {
    const wv = window.chrome?.webview as
      | {
          addEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
          removeEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
        }
      | undefined;
    if (!wv?.addEventListener) return;
    const onMsg = (e: { data: unknown }) => {
      const msg = parseOpenPickerMessage(e.data);
      if (msg?.which === "mods") {
        // The host may have changed the layer stack out-of-band (e.g. a --record
        // mods/set-layers, which goes straight through the bridge and never hits the
        // web setLayerStack wrapper). Re-fetch so the dropdown shows the live stack.
        if (msg.open) void refreshModsList();
        else setPosedStackDrag(null); // closing clears any posed drag so the chip can't ghost
        onMenuValueChange(msg.open ? MODS_MENU_VALUE : "");
      }
      const pd = parsePoseDragMessage(e.data);
      if (pd?.target === "stack") setPosedStackDrag({ from: pd.from, gap: pd.gap });
    };
    wv.addEventListener("message", onMsg);
    return () => wv.removeEventListener?.("message", onMsg);
  }, []);

  // Add mod… catalog — the layer catalog grouped by parent, searchable
  // (membership lives here now, out of the top-level menu). Mirrors the modal's
  // available pane: keyed by parentPath so duplicate-label mods stay separate.
  const addGroups: { key: string; label: string; items: LayerRef[] }[] = [];
  for (const l of layers) {
    const key = l.kind === "nested" ? (l.parentPath ?? l.path) : l.path;
    const headerLabel = l.kind === "nested" ? (l.parentLabel ?? l.label) : l.label;
    let grp = addGroups.find((x) => eqPath(x.key, key));
    if (!grp) { grp = { key, label: headerLabel, items: [] }; addGroups.push(grp); }
    grp.items.push(l);
  }
  const aq = addQuery.trim().toLowerCase();
  const addVisibleGroups = aq
    ? addGroups.map((g) => ({ ...g, items: g.items.filter((l) => l.label.toLowerCase().includes(aq)) })).filter((g) => g.items.length > 0)
    : addGroups;
  // Make-room spacer for the in-menu drag (matches the modal); role=presentation so
  // it never counts as a stack listitem.
  const menuGapSpacer = (
    <li
      aria-hidden
      role="presentation"
      className="pointer-events-none mx-0.5 rounded bg-accent-soft ring-1 ring-inset ring-accent"
      style={{ height: `${menuDrag.gapHeight}px` }}
    />
  );

  return (
    <>
      <Menubar.Menu value="mods">
        <Menubar.Trigger className={TRIGGER}>Mods</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={`${CONTENT} w-72`}
            align="start"
            sideOffset={4}
            // Guardrail: suppress the dropdown's auto-dismiss while a drag
            // is live, so a drag that strays past the menu edge can't close it
            // mid-gesture (the designer's #1 risk for in-popover drag).
            onEscapeKeyDown={(e) => { if (menuDrag.dragging) e.preventDefault(); }}
            onPointerDownOutside={(e) => { if (menuDrag.dragging) e.preventDefault(); }}
            onFocusOutside={(e) => { if (menuDrag.dragging) e.preventDefault(); }}
            onInteractOutside={(e) => { if (menuDrag.dragging) e.preventDefault(); }}
          >
            {/* Active load order — the menu IS the editor: drag rows by the grip to
                reorder in place (live-committed). Expand opens the full modal (kept
                as the demoted fallback + the keyboard/AT reorder path). */}
            <div className="mb-1 rounded-md border border-border bg-bg p-2">
              <div className="mb-1.5 flex items-center gap-1.5 px-0.5">
                <span className="text-[10px] font-semibold uppercase tracking-wide text-text-3">
                  Active load order
                </span>
                {stack.length > 0 && <TopWinsBadge />}
                <span className="flex-1" />
                {/* Expand → the full modal: the demoted fallback AND the keyboard/AT
                    reorder path (the modal has ↑/↓ + tabbable remove). A real
                    Menubar.Item so it's in the roving tab order + AT-announced
                    (the menu's drag/× are mouse conveniences on top of this). */}
                <Menubar.Item
                  aria-label="Expand to full editor"
                  title="Expand to full editor"
                  onSelect={() => setLoadOrderOpen(true)}
                  className="flex size-[18px] shrink-0 cursor-pointer items-center justify-center rounded text-text-3 outline-none hover:bg-hover hover:text-text data-[highlighted]:bg-hover data-[highlighted]:text-text"
                >
                  <ExpandIcon />
                </Menubar.Item>
              </div>
              {stack.length === 0 ? (
                <div className="flex h-[var(--row-h)] items-center rounded-[var(--radius-sm)] border border-dashed border-border-2 px-2 text-[11px] text-text-3">
                  Unmodded — base game only.
                </div>
              ) : (
                <div className="flex gap-1.5">
                  <div className="my-0.5 w-[3px] shrink-0 rounded-[var(--radius-2xs)] bg-gradient-to-b from-accent via-accent-2 to-border-2" />
                  <ul ref={menuDrag.listRef} className="flex min-w-0 flex-1 flex-col gap-[3px]" aria-label="Active load order">
                    {stack.map((p, i) => (
                      <Fragment key={p}>
                        {menuDrag.gap === i && menuDrag.dragIndex !== null && menuGapSpacer}
                        <li
                          data-flip-key={p}
                          // Positional (not path) id: the host returns Windows
                          // backslash paths and eqPath doesn't normalize slashes, so a
                          // --record clip targets the row by index (testid:stack-row:<i>).
                          data-testid={`stack-row:${i}`}
                          onPointerDown={menuDrag.startDrag(i)}
                          className={cn(
                            "relative flex h-[var(--row-h)] touch-none select-none items-center gap-1.5 rounded-[var(--radius-sm)] border border-border-2 bg-bg-3 pl-1 pr-1 text-xs",
                            menuDrag.dragIndex === i ? "cursor-grabbing opacity-40 saturate-50" : "cursor-grab",
                          )}
                        >
                          <span className="flex w-3.5 shrink-0 items-center justify-center text-text-3" aria-hidden>
                            <GripVertical className="size-2.5" />
                          </span>
                          <span className="min-w-[11px] shrink-0 text-right text-[11px] font-semibold tabular-nums text-text-3" aria-hidden="true">{i + 1}</span>
                          <Layers className="size-3 shrink-0 text-text-3" strokeWidth={1.3} />
                          <span className="min-w-0 flex-1 truncate text-text">{labelFor(p)}</span>
                          <button
                            type="button"
                            tabIndex={-1}
                            aria-label={`Remove ${labelFor(p)} from stack`}
                            onClick={() => removeFromStack(p)}
                            className="flex size-[18px] shrink-0 items-center justify-center rounded text-text-3 hover:bg-hover hover:text-danger-fg"
                          >
                            <X className="size-2.5" strokeWidth={1.6} />
                          </button>
                        </li>
                      </Fragment>
                    ))}
                    {menuDrag.gap === stack.length && menuDrag.dragIndex !== null && menuGapSpacer}
                  </ul>
                </div>
              )}
            </div>

            <Menubar.Separator className={SEPARATOR} />

            {/* Add mod… — the catalog + search lives here now (out of the top-level
                menu). Adding appends to the stack; removal is the × on a stack row.
                preventDefault keeps the flyout open so you can add several. */}
            <Menubar.Sub>
              <Menubar.SubTrigger className={ITEM}>
                <span className="flex size-3.5 shrink-0 items-center justify-center text-accent"><Plus className="size-3.5" strokeWidth={1.8} /></span>
                <span className="flex-1">Add mod…</span>
                <ChevronRight className="size-3.5 text-text-3" />
              </Menubar.SubTrigger>
              <Menubar.Portal>
                <Menubar.SubContent
                  className={`${CONTENT} w-60`}
                  sideOffset={2}
                  alignOffset={-4}
                >
                  {/* focus-within accent border = the keyboard-focus cue for the
                      borderless input inside (design pass; was focus-invisible). */}
                  <div className="mb-1 flex h-[var(--row-h)] items-center gap-1.5 rounded-[var(--radius-sm)] border border-border-2 bg-bg-3 px-2 transition-colors motion-reduce:transition-none focus-within:border-accent">
                    <Search className="size-3 shrink-0 text-text-3" strokeWidth={1.5} />
                    <input
                      value={addQuery}
                      onChange={(e) => setAddQuery(e.target.value)}
                      placeholder="Search mods…"
                      aria-label="Search mods"
                      // Let typing reach the input rather than Radix's menu typeahead.
                      onKeyDown={(e) => e.stopPropagation()}
                      className="min-w-0 flex-1 border-none bg-transparent text-xs text-text outline-none placeholder:text-text-3"
                    />
                  </div>
                  {addVisibleGroups.map((g) => (
                    <div key={g.key}>
                      <div className="px-2 pb-0.5 pt-1 text-[10px] font-semibold uppercase tracking-wide text-text-3">{g.label}</div>
                      {g.items.map((l) => {
                        const added = inStack(l.path);
                        const nested = l.kind === "nested";
                        return added ? (
                          <div key={l.path} className={cn("flex h-[var(--row-h)] items-center gap-1.5 px-2 text-xs text-text-3", nested && "pl-5")}>
                            <Layers className="size-3 shrink-0 text-text-3" strokeWidth={1.3} />
                            <span className="min-w-0 flex-1 truncate">{l.label}</span>
                            <span className="flex shrink-0 items-center gap-1 text-[10px]"><Check className="size-2.5 text-success-fg" strokeWidth={1.8} />in stack</span>
                          </div>
                        ) : (
                          <Menubar.Item
                            key={l.path}
                            className={cn(ITEM, nested && "pl-5")}
                            onSelect={(e) => { e.preventDefault(); toggleLayer(l.path); }}
                          >
                            <Layers className="size-3 shrink-0 text-text-3" strokeWidth={1.3} />
                            <span className="flex-1 truncate">{l.label}</span>
                            <Plus className="size-3 shrink-0 text-accent" strokeWidth={1.8} />
                          </Menubar.Item>
                        );
                      })}
                    </div>
                  ))}
                  {addVisibleGroups.length === 0 && (
                    <div className="px-2 py-2 text-[11px] text-text-3">No mods match.</div>
                  )}
                </Menubar.SubContent>
              </Menubar.Portal>
            </Menubar.Sub>

            <Menubar.Separator className={SEPARATOR} />

            {/* Reset — clears the stack (Unmodded). Stacked-layers glyph + dotted
                divider. preventDefault keeps the menu open
                so the cleared state shows live above. */}
            <Menubar.Item
              className={ITEM}
              onSelect={(e) => { e.preventDefault(); void setLayerStack([]); }}
            >
              <Layers className="size-3.5 shrink-0 text-text-3" strokeWidth={1.3} />
              <span aria-hidden className="h-3.5 w-0 border-l border-dotted border-border-2" />
              <span className="flex-1">Reset</span>
            </Menubar.Item>

            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={() => { void handleModRefresh(); }}
            >
              <span className="flex w-3.5 shrink-0 justify-center text-text-2"><RefreshIcon /></span>
              <span className="flex-1">Refresh Mod List</span>
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>
      {menuDrag.chipNode}

      <LoadOrderDialog
        bridge={bridge}
        open={loadOrderOpen}
        onOpenChange={setLoadOrderOpen}
        onApplied={() => void refreshModsList()}
      />
    </>
  );
}

