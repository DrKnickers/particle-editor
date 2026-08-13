// ReferenceObjectPicker — picks a real game/mod object to place in the
// preview as a scale reference, with a numeric transform + a unit grid toggle.
//
// The object list is enumerated live from the active mod/base's
// GameObjectFiles.xml via `engine/query/reference-object-list` (Name + the
// profile classification {domain, role, bucket}); selection drives
// `engine/set/reference-object`. The engine probes the chosen `.alo` lazily on
// select and reports `referenceObjectStatus` so the picker can warn when an
// object is skinned (a v1 deferral) or fails to load.
//
// The list shows only FIELDABLE units + structures + all heroes
// (filtered engine-side), and is built off the UI thread; while it builds the
// query returns `building:true` and the picker shows "Loading objects…". A
// search box narrows the list. The objects are presented as a COLLAPSIBLE TREE
// (Stage 2): top-level **Heroes / Ground / Space** sections, each Ground/Space
// section sub-grouped into bucket disclosures (Infantry, Vehicles, …, Fighters,
// Capitals, …). Searching force-expands so matches are always visible.
//
// The transform is six `Spinner`s (position X/Y/Z + rotation yaw/pitch/roll in
// degrees) → `engine/set/reference-object-transform`.
//
// Browser/mock mode: the list query returns a small canned set spanning the
// sections and the transform round-trips through the mock store.

import { type KeyboardEvent as ReactKeyboardEvent, useEffect, useMemo, useRef, useState } from "react";
import type {
  Bridge,
  EngineStateDto,
  ReferenceObjectBucket,
  ReferenceObjectEntry,
  Vec3,
} from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { loadPickerState, resolveFaction, savePickerState } from "@/lib/picker-state";
import { parseSetPickerCollapseMessage, parseSetPickerSearchMessage } from "@/lib/record-focus-bridge";

type BodyProps = {
  bridge: Bridge;
};

const NONE = ""; // empty Name = no object selected

// Bucket display order + plural labels, per domain. "None" folds into
// "Other" so an object whose fine bucket didn't resolve is still grouped, never
// dropped (the engine never lists Excluded objects, so role is Unit/Structure/Hero).
const GROUND_BUCKETS: readonly ReferenceObjectBucket[] = [
  "Infantry", "Vehicle", "Air", "Structure", "Other",
];
const SPACE_BUCKETS: readonly ReferenceObjectBucket[] = [
  "Fighter", "Bomber", "Corvette", "Frigate", "Capital", "Transport", "Structure", "Other",
];
const BUCKET_LABEL: Record<ReferenceObjectBucket, string> = {
  Infantry: "Infantry", Vehicle: "Vehicles", Air: "Air",
  Fighter: "Fighters", Bomber: "Bombers", Corvette: "Corvettes", Frigate: "Frigates",
  Capital: "Capitals", Transport: "Transports", Structure: "Structures",
  Hero: "Heroes", Other: "Other", None: "Other",
};

type BucketGroup = { key: string; label: string; items: string[] };
type Section = {
  key: string;       // "Heroes" | "Ground" | "Space"
  label: string;
  total: number;
  flat: string[];    // Heroes: the names directly (no bucket sub-headers)
  buckets: BucketGroup[]; // Ground/Space: bucket sub-groups
};

// Build the Heroes / Ground / Space / Props / Templates sections from the
// (search-filtered) entries. Heroes, Props, and Templates each go to their own flat
// top-level section (no bucket sub-headers) regardless of domain; Units/Structures group
// by domain then bucket in the fixed bucket order.
function buildSections(objects: ReferenceObjectEntry[], matches: (n: string) => boolean): Section[] {
  const heroes: string[] = [];
  const props: string[] = [];
  const templates: string[] = [];
  const ground = new Map<ReferenceObjectBucket, string[]>();
  const space = new Map<ReferenceObjectBucket, string[]>();
  const push = (m: Map<ReferenceObjectBucket, string[]>, b: ReferenceObjectBucket, n: string) => {
    const key: ReferenceObjectBucket = b === "None" ? "Other" : b;
    const arr = m.get(key) ?? [];
    arr.push(n);
    m.set(key, arr);
  };
  for (const o of objects) {
    if (!matches(o.name)) continue;
    if (o.role === "Hero") heroes.push(o.name);
    else if (o.role === "Prop") props.push(o.name);
    else if (o.role === "Template") templates.push(o.name);
    else if (o.domain === "Space") push(space, o.bucket, o.name);
    else push(ground, o.bucket, o.name); // Ground + Unknown fall here
  }
  const bucketGroups = (m: Map<ReferenceObjectBucket, string[]>, order: readonly ReferenceObjectBucket[]): BucketGroup[] =>
    order
      .filter((b) => (m.get(b)?.length ?? 0) > 0)
      .map((b) => ({ key: `${b}`, label: BUCKET_LABEL[b], items: m.get(b)!.slice().sort() }));
  const flatSection = (key: string, label: string, names: string[]): Section =>
    ({ key, label, total: names.length, flat: names.slice().sort(), buckets: [] });

  const sections: Section[] = [];
  if (heroes.length) sections.push(flatSection("Heroes", "Heroes", heroes));
  const g = bucketGroups(ground, GROUND_BUCKETS);
  if (g.length)
    sections.push({ key: "Ground", label: "Ground", total: g.reduce((a, b) => a + b.items.length, 0), flat: [], buckets: g });
  const s = bucketGroups(space, SPACE_BUCKETS);
  if (s.length)
    sections.push({ key: "Space", label: "Space", total: s.reduce((a, b) => a + b.items.length, 0), flat: [], buckets: s });
  // Props + Templates last: scenery / base objects, less often the primary reference.
  if (props.length) sections.push(flatSection("Props", "Props", props));
  if (templates.length) sections.push(flatSection("Templates", "Templates", templates));
  return sections;
}
/**
 * ReferenceObjectPickerBody — the picker content (collapsible object tree +
 * status + numeric transform + grid controls). Mounted by the toolbar dropdown.
 */
export function ReferenceObjectPickerBody({ bridge }: BodyProps) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);
  const [objects, setObjects] = useState<ReferenceObjectEntry[]>([]);
  const [ready, setReady] = useState(false); // got a non-building object list for the active catalog
  const [query, setQuery] = useState("");
  // "Props only" category chip: when on, the tree shows only prop-role objects (ANDed
  // with the faction filter + search). Session-only (resets when the popover reopens).
  const [propsOnly, setPropsOnly] = useState(false);
  // Faction filter (null = All); narrows the tree to objects affiliated with it.
  // Faction + collapsed groups + tree scroll persist across popover reopens so the
  // user keeps their place (the popover unmounts on close). See lib/picker-state.
  const [faction, setFaction] = useState<string | null>(() => loadPickerState().faction);
  // Collapsed group keys (default = everything expanded; collapse is opt-in).
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set(loadPickerState().collapsed));
  // --record only: sections force-collapsed by the host (ui/picker-collapse). Unlike
  // `collapsed`, these stay closed even while searching (which otherwise force-expands
  // everything) — lets a clip hide a section, e.g. Heroes, during a filter. Empty for users.
  const [forceCollapsed, setForceCollapsed] = useState<Set<string>>(new Set());
  const treeRef = useRef<HTMLDivElement | null>(null);
  const scrollRestored = useRef(false);
  // Roving-tabindex: exactly one tree row is tabbable at a time; arrow
  // keys move the active row + focus it. `itemRefs` maps a row key -> its DOM node.
  const [activeKey, setActiveKey] = useState<string>("none");
  const itemRefs = useRef(new Map<string, HTMLDivElement | null>());
  // LEVEL-triggered (not edge-triggered): re-query the list whenever we don't yet have a
  // ready list (readyRef avoids the stale closure). Once ready we STOP re-querying on
  // engine/state/changed, so a ~30 Hz gizmo drag doesn't thrash the UI thread.
  const readyRef = useRef(false);

  useEffect(() => {
    let cancelled = false;
    const markReady = (v: boolean) => { readyRef.current = v; setReady(v); };
    const fetchList = () => {
      bridge
        .request({ kind: "engine/query/reference-object-list", params: {} })
        .then((r) => {
          if (cancelled || r.building) return; // still building -> stay loading; we'll re-query
          setObjects(r.objects ?? []);
          markReady(true);
        })
        .catch((err) => {
          if (cancelled) return;
          console.warn("[ReferenceObjectPicker] list failed:", err);
          markReady(true); // unblock the UI; the list just stays empty
        });
    };
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setSnapshot(s); })
      .catch((err) => console.warn("[ReferenceObjectPicker] snapshot failed:", err));
    fetchList();
    const off = bridge.on("engine/state/changed", (e) => {
      if (cancelled) return;
      setSnapshot(e.payload);
      // A (re)build started (mod/submod switch) -> drop the stale list + reload when ready.
      if (e.payload.referenceCatalogBuilding) markReady(false);
      // Re-query only while we still need a list; a no-op (early-returns) while building.
      if (!readyRef.current) fetchList();
    });
    return () => { cancelled = true; off(); };
  }, [bridge]);

  // --record only: the host drives the search box via a ui/set-picker-search push
  // (the synthetic cursor can't type) — used to filter the tree to a few units so a
  // clip cursor can reach them instead of scrolling. Dormant outside --record.
  useEffect(() => {
    const wv = window.chrome?.webview as
      | {
          addEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
          removeEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
        }
      | undefined;
    if (!wv?.addEventListener) return;
    const onMsg = (e: { data: unknown }) => {
      const msg = parseSetPickerSearchMessage(e.data);
      if (msg) setQuery(msg.text);
      // Bare section names from the timeline (e.g. "Heroes") map to the prefixed
      // collapse keys the tree uses ("sec:Heroes"), matching `collapsed`/isOpen.
      const col = parseSetPickerCollapseMessage(e.data);
      if (col) setForceCollapsed(new Set(col.keys.map((k) => `sec:${k}`)));
    };
    wv.addEventListener("message", onMsg);
    return () => wv.removeEventListener?.("message", onMsg);
  }, []);

  const loading = !ready;

  const name = snapshot?.referenceObjectName ?? NONE;
  const visible = snapshot?.referenceObjectVisible ?? true;
  const locked = snapshot?.referenceObjectLocked ?? false;
  const status = snapshot?.referenceObjectStatus ?? "none";
  const pos: Vec3 = snapshot?.referenceObjectPosition ?? [0, 0, 0];
  const rot: Vec3 = snapshot?.referenceObjectRotation ?? [0, 0, 0];
  const snapEnabled = snapshot?.snapEnabled ?? false;

  // Live search: case-insensitive substring on Name.
  const ql = query.trim().toLowerCase();
  const matches = useMemo(() => (n: string) => ql === "" || n.toLowerCase().includes(ql), [ql]);

  // Faction filter. `affiliation` may be a comma list ("Rebel, Empire"); the chip
  // set is every distinct token across all objects, and an object passes when it carries the
  // selected faction. (Objects with no affiliation appear only under "All".)
  const factionsOf = (o: ReferenceObjectEntry) =>
    (o.affiliation ?? "").split(",").map((s) => s.trim()).filter(Boolean);
  const factions = useMemo(() => {
    const set = new Set<string>();
    for (const o of objects) for (const f of factionsOf(o)) set.add(f);
    return [...set].sort();
  }, [objects]);
  const hasProps = useMemo(() => objects.some((o) => o.role === "Prop"), [objects]);
  const visibleObjects = useMemo(() => {
    const byFaction = faction === null ? objects : objects.filter((o) => factionsOf(o).includes(faction));
    return propsOnly ? byFaction.filter((o) => o.role === "Prop") : byFaction;
  }, [objects, faction, propsOnly]);

  // Persist + validate the faction filter. Once the active content's faction
  // set is known, drop a saved faction that isn't present (e.g. after a mod switch)
  // so it can never filter the list to empty; otherwise persist the choice.
  useEffect(() => {
    // Validate only once the list is READY (not merely "factions non-empty"): a mod
    // whose units carry no affiliation has an empty faction set, and a stale saved
    // faction must still be cleared there (otherwise the list filters to empty with
    // no chip to recover, since the chip row only renders when factions exist).
    if (!ready) return; // still loading -> keep the saved faction untouched
    const resolved = resolveFaction(faction, factions);
    if (resolved !== faction) {
      setFaction(resolved);
      return;
    }
    savePickerState({ faction });
  }, [faction, factions, ready]);

  // Persist the collapsed-group set (serialized as a sorted array).
  useEffect(() => {
    savePickerState({ collapsed: [...collapsed] });
  }, [collapsed]);

  // Restore the saved tree scroll once the list has rendered (one-shot).
  useEffect(() => {
    if (ready && treeRef.current && !scrollRestored.current) {
      treeRef.current.scrollTop = loadPickerState().scrollTop;
      scrollRestored.current = true;
    }
  }, [ready]);

  const sections = useMemo(() => buildSections(visibleObjects, matches), [visibleObjects, matches]);
  const visibleCount = useMemo(() => sections.reduce((a, s) => a + s.total, 0), [sections]);

  // While searching, force every group open so matches are always visible; otherwise
  // honor the user's collapse choices. (A group is open unless explicitly collapsed.)
  // A host-forced collapse (--record only) wins over BOTH — so a clip can keep a
  // section closed even under a search filter. `forceCollapsed` holds the same
  // prefixed keys as `collapsed` (e.g. "sec:Heroes"). Empty for normal users.
  const searching = ql !== "";
  const isOpen = (key: string) => !forceCollapsed.has(key) && (searching || !collapsed.has(key));
  const toggle = (key: string) =>
    setCollapsed((prev) => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  const setCollapsedKey = (key: string, c: boolean) =>
    setCollapsed((prev) => {
      if (c === prev.has(key)) return prev;
      const next = new Set(prev);
      if (c) next.add(key); else next.delete(key);
      return next;
    });

  // The persisted/active selection may be absent from the visible list (mod object,
  // async load, or filtered by the search); surface it as a standalone row so the tree
  // doesn't silently lose it.
  const known =
    name === NONE ||
    (visibleObjects.some((o) => o.name === name) && matches(name));

  // The flat list of VISIBLE tree rows in DOM order — drives roving-tabindex
  // arrow navigation. Each row carries enough to move (kind/expanded/parent).
  type Row =
    | { key: string; kind: "leaf"; name: string; parentKey?: string }
    | { key: string; kind: "exp"; collapseKey: string; expanded: boolean; parentKey?: string };
  const rows = useMemo<Row[]>(() => {
    const out: Row[] = [{ key: "none", kind: "leaf", name: NONE }];
    if (name !== NONE && !known) out.push({ key: `leaf:${name}`, kind: "leaf", name });
    for (const sec of sections) {
      const secKey = `sec:${sec.key}`;
      const openSec = isOpen(secKey);
      out.push({ key: secKey, kind: "exp", collapseKey: secKey, expanded: openSec });
      if (!openSec) continue;
      if (sec.buckets.length === 0) {
        // Flat section (Heroes / Props / Templates): leaves directly under the section.
        for (const n of sec.flat) out.push({ key: `leaf:${n}`, kind: "leaf", name: n, parentKey: secKey });
      } else {
        for (const b of sec.buckets) {
          const bk = `buc:${sec.key}/${b.key}`;
          const openBuc = isOpen(bk);
          out.push({ key: bk, kind: "exp", collapseKey: bk, expanded: openBuc, parentKey: secKey });
          if (openBuc) for (const n of b.items) out.push({ key: `leaf:${n}`, kind: "leaf", name: n, parentKey: bk });
        }
      }
    }
    return out;
  }, [sections, collapsed, forceCollapsed, searching, name, known]);

  // Keep the active (tabbable) row valid as the visible set changes (search / collapse).
  useEffect(() => {
    if (rows.some((r) => r.key === activeKey)) return;
    const selKey = name !== NONE ? `leaf:${name}` : "none";
    setActiveKey(rows.some((r) => r.key === selKey) ? selKey : (rows[0]?.key ?? "none"));
  }, [rows, activeKey, name]);

  // Force-open the ancestor groups of the active selection so a
  // selection persisted from a prior session (into a group the user had collapsed)
  // is always visible. Runs on selection / list change, never re-collapsing on its own.
  useEffect(() => {
    if (name === NONE || !ready) return;
    const obj = objects.find((o) => o.name === name);
    if (!obj) return;
    const domainKey = obj.domain === "Space" ? "Space" : "Ground";
    const keys = obj.role === "Hero"
      ? ["sec:Heroes"]
      : obj.role === "Prop"
        ? ["sec:Props"]
        : obj.role === "Template"
          ? ["sec:Templates"]
          : [`sec:${domainKey}`, `buc:${domainKey}/${obj.bucket === "None" ? "Other" : obj.bucket}`];
    setCollapsed((prev) => {
      if (!keys.some((k) => prev.has(k))) return prev;
      const next = new Set(prev);
      keys.forEach((k) => next.delete(k));
      return next;
    });
  }, [name, objects, ready]);

  const setRowRef = (key: string) => (el: HTMLDivElement | null) => {
    if (el) itemRefs.current.set(key, el);
    else itemRefs.current.delete(key);
  };
  const focusRow = (key: string) => {
    setActiveKey(key);
    itemRefs.current.get(key)?.focus();
  };
  // Arrow-key navigation per the WAI-ARIA tree pattern (↑/↓ move, →/← dive/collapse,
  // Home/End, Enter/Space activate). Type-ahead is covered by the search box above.
  const onTreeKeyDown = (e: ReactKeyboardEvent, key: string) => {
    const NAV = ["ArrowDown", "ArrowUp", "ArrowLeft", "ArrowRight", "Home", "End", "Enter", " "];
    if (!NAV.includes(e.key)) return;
    e.stopPropagation(); // a focused child must not let its ancestor treeitem re-handle the key
    const idx = rows.findIndex((r) => r.key === key);
    if (idx < 0) return;
    const row = rows[idx];
    switch (e.key) {
      case "ArrowDown": e.preventDefault(); if (idx < rows.length - 1) focusRow(rows[idx + 1].key); break;
      case "ArrowUp":   e.preventDefault(); if (idx > 0) focusRow(rows[idx - 1].key); break;
      case "Home":      e.preventDefault(); focusRow(rows[0].key); break;
      case "End":       e.preventDefault(); focusRow(rows[rows.length - 1].key); break;
      case "ArrowRight":
        if (row.kind === "exp") {
          e.preventDefault();
          if (!row.expanded) setCollapsedKey(row.collapseKey, false);
          else if (idx < rows.length - 1) focusRow(rows[idx + 1].key);
        }
        break;
      case "ArrowLeft":
        e.preventDefault();
        if (row.kind === "exp" && row.expanded) setCollapsedKey(row.collapseKey, true);
        else if (row.parentKey) focusRow(row.parentKey);
        break;
      case "Enter":
      case " ":
        e.preventDefault(); e.stopPropagation();
        if (row.kind === "leaf") selectObject(row.name);
        else toggle(row.collapseKey);
        break;
    }
  };

  const selectObject = (n: string) =>
    void bridge.request({ kind: "engine/set/reference-object", params: { name: n } });

  const setVisible = (v: boolean) =>
    void bridge.request({ kind: "engine/set/reference-object-visible", params: { visible: v } });

  const setLocked = (v: boolean) =>
    void bridge.request({ kind: "engine/set/reference-object-lock", params: { locked: v } });

  // Persistent gizmo snap toggle. Ticking it persists/round-trips now; the
  // drag-time apply (reads the engine's snap state) lands in a separate task.
  const setSnapEnabled = (v: boolean) =>
    void bridge.request({ kind: "engine/set/snap-enabled", params: { enabled: v } });

  const setTransform = (position: Vec3, rotation: Vec3) =>
    void bridge.request({
      kind: "engine/set/reference-object-transform",
      params: { position, rotation },
    });

  const setPosAxis = (i: number, v: number) => {
    const next: [number, number, number] = [pos[0], pos[1], pos[2]];
    next[i] = v;
    setTransform(next, rot);
  };
  const setRotAxis = (i: number, v: number) => {
    const next: [number, number, number] = [rot[0], rot[1], rot[2]];
    next[i] = v;
    setTransform(pos, next);
  };

  const statusNote =
    status === "skinned"
      ? "Skinned unit — not yet supported (rigid parts only)."
      : status === "model-missing"
        ? "Model file not found in the current mod/base game."
        : status === "load-failed"
          ? "Couldn't load this object's model."
          : null;

  // ── tree (role="tree") renderers ──────────────────────────────────
  // A selectable leaf (an object name).
  const itemRow = (n: string) => {
    const selected = n === name;
    const rowKey = `leaf:${n}`;
    return (
      <div
        key={n}
        role="treeitem"
        aria-selected={selected}
        data-testid={`ref-unit:${n}`}
        ref={setRowRef(rowKey)}
        tabIndex={activeKey === rowKey ? 0 : -1}
        onClick={(e) => { e.stopPropagation(); setActiveKey(rowKey); selectObject(n); }}
        onKeyDown={(e) => onTreeKeyDown(e, rowKey)}
        className={
          "cursor-pointer truncate rounded px-2 py-0.5 text-sm transition motion-reduce:transition-none focus-ring " +
          (selected ? "bg-accent/20 text-text" : "text-text hover:bg-panel-2")
        }
      >
        {n}
      </div>
    );
  };

  // The visual label row of an expandable treeitem (chevron + label + count). The
  // click/key handling lives on the treeitem wrapper, so this is presentational.
  const labelRow = (label: string, count: number, open: boolean, indent: boolean) => (
    <div
      className={
        "flex cursor-pointer items-center gap-1 rounded px-1 py-0.5 transition motion-reduce:transition-none hover:bg-panel-2 " +
        (indent ? "pl-3 text-xs text-text-2" : "text-xs font-medium uppercase tracking-wide text-text-3")
      }
    >
      <span aria-hidden className={"inline-block w-3 text-text-3 transition-transform motion-reduce:transition-none " + (open ? "rotate-90" : "")}>
        ▸
      </span>
      <span className="flex-1">{label}</span>
      <span className="text-text-3">{count}</span>
    </div>
  );

  return (
    <div className="flex flex-col gap-4">
      {/* ── Object selection ─────────────────────────────────────────── */}
      <section className="flex flex-col gap-2">
        <span className="text-xs font-medium uppercase tracking-wide text-text-3">
          Reference object
        </span>

        <input
          type="search"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search units & structures…"
          aria-label="Search reference objects"
          disabled={loading}
          className="rounded-md border border-border bg-bg-2 px-2 py-1 text-sm text-text"
        />

        {loading ? (
          <p role="status" className="text-xs text-text-2">Loading objects…</p>
        ) : (
          <>
            {/* Category filter: "Props only" chip (only when the content has props).
                Filters everything that isn't a prop; ANDs with faction + search. */}
            {hasProps && (
              <div className="flex flex-wrap gap-1" role="group" aria-label="Filter by category">
                <button
                  type="button"
                  aria-pressed={propsOnly}
                  onClick={() => setPropsOnly((v) => !v)}
                  className={
                    "rounded-full border px-2 py-0.5 text-xs transition motion-reduce:transition-none focus-ring " +
                    (propsOnly
                      ? "border-accent bg-accent/20 text-text"
                      : "border-border text-text-2 hover:bg-panel-2")
                  }
                >
                  Props only
                </button>
              </div>
            )}
            {/* Faction filter chips (only when the active content has affiliations). */}
            {factions.length > 0 && (
              <div
                className="flex max-h-20 flex-wrap gap-1 overflow-y-auto scrollbar-stable"
                role="group"
                aria-label="Filter by faction"
              >
                {[null, ...factions].map((f) => {
                  const active = faction === f;
                  return (
                    <button
                      key={f ?? "__all__"}
                      type="button"
                      aria-pressed={active}
                      onClick={() => setFaction(f)}
                      className={
                        "rounded-full border px-2 py-0.5 text-xs transition motion-reduce:transition-none focus-ring " +
                        (active
                          ? "border-accent bg-accent/20 text-text"
                          : "border-border text-text-2 hover:bg-panel-2")
                      }
                    >
                      {f === null ? "All" : f.replace(/_/g, " ")}
                    </button>
                  );
                })}
              </div>
            )}
            <div
              ref={treeRef}
              role="tree"
              aria-label="Reference object"
              onScroll={(e) => savePickerState({ scrollTop: e.currentTarget.scrollTop })}
              className="flex max-h-72 flex-col gap-0.5 overflow-auto rounded-md border border-border bg-bg-2 p-1"
            >
              <div
                role="treeitem"
                aria-selected={name === NONE}
                ref={setRowRef("none")}
                tabIndex={activeKey === "none" ? 0 : -1}
                onClick={() => { setActiveKey("none"); selectObject(NONE); }}
                onKeyDown={(e) => onTreeKeyDown(e, "none")}
                className={
                  "cursor-pointer rounded px-2 py-0.5 text-sm transition motion-reduce:transition-none focus-ring " +
                  (name === NONE ? "bg-accent/20 text-text" : "text-text-2 hover:bg-panel-2")
                }
              >
                None
              </div>

              {/* A persisted selection filtered out of the visible tree. */}
              {name !== NONE && !known && itemRow(name)}

              {sections.map((sec) => {
                const secKey = `sec:${sec.key}`;
                const openSec = isOpen(secKey);
                return (
                  <div
                    key={sec.key}
                    role="treeitem"
                    aria-label={sec.label}
                    aria-expanded={openSec}
                    ref={setRowRef(secKey)}
                    tabIndex={activeKey === secKey ? 0 : -1}
                    onClick={(e) => { e.stopPropagation(); setActiveKey(secKey); toggle(secKey); }}
                    onKeyDown={(e) => onTreeKeyDown(e, secKey)}
                    className="flex flex-col rounded focus-ring"
                  >
                    {labelRow(sec.label, sec.total, openSec, false)}
                    {openSec && sec.buckets.length === 0 && (
                      <div role="group" className="flex flex-col pl-3">{sec.flat.map(itemRow)}</div>
                    )}
                    {openSec && sec.buckets.length > 0 && (
                      <div role="group" className="flex flex-col">
                        {sec.buckets.map((b) => {
                          const bk = `buc:${sec.key}/${b.key}`;
                          const openBuc = isOpen(bk);
                          return (
                            <div
                              key={bk}
                              role="treeitem"
                              aria-label={b.label}
                              aria-expanded={openBuc}
                              ref={setRowRef(bk)}
                              tabIndex={activeKey === bk ? 0 : -1}
                              onClick={(e) => { e.stopPropagation(); setActiveKey(bk); toggle(bk); }}
                              onKeyDown={(e) => onTreeKeyDown(e, bk)}
                              className="flex flex-col rounded focus-ring"
                            >
                              {labelRow(b.label, b.items.length, openBuc, true)}
                              {openBuc && (
                                <div role="group" className="flex flex-col pl-6">{b.items.map(itemRow)}</div>
                              )}
                            </div>
                          );
                        })}
                      </div>
                    )}
                  </div>
                );
              })}
            </div>
            {visibleCount === 0 && (searching || faction !== null || propsOnly) && (
              <p className="text-xs text-text-3">
                {searching
                  ? <>No {propsOnly ? "props" : "objects"} match “{query}”{faction !== null ? <> in {faction.replace(/_/g, " ")}</> : null}.</>
                  : propsOnly
                    ? <>No props{faction !== null ? <> in {faction.replace(/_/g, " ")}</> : null}.</>
                    : <>No {faction!.replace(/_/g, " ")} objects.</>}
              </p>
            )}
          </>
        )}

        {statusNote && (
          <p role="alert" className="text-xs text-warning-fg">{statusNote}</p>
        )}

        <label className="flex items-center gap-2 text-xs text-text-2">
          <input
            type="checkbox"
            checked={visible}
            onChange={(e) => setVisible(e.target.checked)}
            disabled={name === NONE}
            aria-label="Reference object visible"
            className="disabled:opacity-40 disabled:cursor-not-allowed"
          />
          Visible
        </label>
      </section>

      {/* ── Numeric transform (position + rotation) ──────────────────── */}
      <section className="flex flex-col gap-2">
        <div className="flex items-center justify-between">
          <span className="text-xs font-medium uppercase tracking-wide text-text-3">
            Transform
          </span>
          {/* Reset the object back to where it loads (origin, no rotation). */}
          <button
            type="button"
            onClick={() => setTransform([0, 0, 0], [0, 0, 0])}
            disabled={name === NONE || locked}
            className="rounded px-2 py-0.5 text-xs text-text-2 transition motion-reduce:transition-none hover:bg-panel-2 hover:text-text disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent focus-ring"
            aria-label="Reset transform to origin"
            title="Reset position + rotation to 0"
          >
            Reset
          </button>
        </div>
        <label
          className="flex items-center gap-2 text-xs text-text-2"
          title="Freeze the object's placement — it can't be selected, dragged, or nudged until unlocked."
        >
          <input
            type="checkbox"
            checked={locked}
            onChange={(e) => setLocked(e.target.checked)}
            disabled={name === NONE}
            aria-label="Lock object"
            className="disabled:opacity-40 disabled:cursor-not-allowed"
          />
          Lock object
        </label>
        <div className="grid grid-cols-3 gap-2">
          {(["X", "Y", "Z"] as const).map((axis, i) => (
            <label key={axis} className="flex flex-col gap-1 text-xs text-text-2">
              {axis}
              <Spinner
                value={pos[i]}
                onChange={(v) => setPosAxis(i, v)}
                step={1}
                decimals={1}
                disabled={locked}
                aria-label={`Position ${axis}`}
              />
            </label>
          ))}
        </div>
        <div className="grid grid-cols-3 gap-2">
          {(["Yaw", "Pitch", "Roll"] as const).map((axis, i) => (
            <label key={axis} className="flex flex-col gap-1 text-xs text-text-2">
              {axis}
              <Spinner
                value={rot[i]}
                onChange={(v) => setRotAxis(i, v)}
                step={5}
                decimals={0}
                unit="°"
                disabled={locked}
                aria-label={`Rotation ${axis}`}
              />
            </label>
          ))}
        </div>

        {/* Persistent snap toggle. */}
        <label
          className="flex items-center gap-2 text-xs text-text-2"
          title="Snaps X/Y to the grid spacing (set in the Ground popup, default 20u) and rotation to 15°. Hold Shift while dragging for a finer ×0.2 step."
        >
          <input
            type="checkbox"
            checked={snapEnabled}
            onChange={(e) => setSnapEnabled(e.target.checked)}
            aria-label="Snap to grid"
          />
          Snap to grid
        </label>
      </section>
    </div>
  );
}
