// ReferenceObjectPicker — picks a real game/mod object to place in the
// preview as a scale reference, with a numeric transform + a unit grid toggle.
//
// The object list is enumerated live from the active mod/base's
// GameObjectFiles.xml via `engine/query/reference-object-list` (Name + category);
// selection drives `engine/set/reference-object`. The engine probes the chosen
// `.alo` lazily on select and reports `referenceObjectStatus` so the picker can
// warn when an object is skinned (a v1 deferral) or fails to load — we can't
// pre-grey the whole list without decoding thousands of meshes at open time.
//
// The list shows UNITS + STRUCTURES only (props/projectiles are filtered
// engine-side), and is built off the UI thread; while it builds the query returns
// `building:true` and the picker shows "Loading objects…", re-querying when the
// catalog-ready engine/state/changed event fires. A search box narrows the
// (now-much-smaller) list; the scrollable <select> is sized as a list box.
//
// The transform is six `Spinner`s (position X/Y/Z + rotation yaw/pitch/roll in
// degrees) → `engine/set/reference-object-transform`. This numeric entry is the
// precise / fallback path; the in-viewport drag manipulator is.
//
// Browser/mock mode: the list query returns a small canned set and the transform
// round-trips through the mock store — enough to validate the dispatch surface
// and grouping against the schema without a real install.

import { useEffect, useRef, useState } from "react";
import type {
  Bridge,
  EngineStateDto,
  ReferenceObjectCategory,
  ReferenceObjectEntry,
  Vec3,
} from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { ToolPanel } from "@/components/ToolPanel";

type Props = {
  bridge: Bridge;
  onClose: () => void;
};

type BodyProps = {
  bridge: Bridge;
};

const NONE = ""; // empty Name = no object selected

// Display order for the category <optgroup>s. Props / Projectiles / the
// model-bearing non-units ("Excluded") are filtered out engine-side
// (Engine::EnumerateReferenceObjects), so they never reach here. "Other" IS shown
// last: it's the catch-all for unrecognised unit/structure tags (e.g. Mod's
// groundcompany / flagship variants / capturable bunkers) -- the filter fails toward
// showing, so a unit type the engine didn't recognise isn't silently dropped.
const CATEGORY_ORDER: readonly ReferenceObjectCategory[] = [
  "Vehicle", "Infantry", "Structure", "Turret", "Hero", "Space", "Other",
];

/**
 * ReferenceObjectPickerBody — the picker content (object list + status + numeric
 * transform + grid controls). Extracted so both the ToolPanel wrapper and the
 * toolbar dropdown mount the same markup (mirrors BackgroundPickerBody).
 */
export function ReferenceObjectPickerBody({ bridge }: BodyProps) {
  const [snapshot, setSnapshot] = useState<EngineStateDto | null>(null);
  const [objects, setObjects] = useState<ReferenceObjectEntry[]>([]);
  const [ready, setReady] = useState(false); // got a non-building object list for the active catalog
  const [query, setQuery] = useState("");
  // LEVEL-triggered (not edge-triggered): re-query the list whenever we don't yet have a
  // ready list (readyRef avoids the stale closure). Once ready we STOP re-querying on
  // engine/state/changed, so a ~30 Hz gizmo drag doesn't thrash the UI thread. A mod /
  // submod switch flips referenceCatalogBuilding true again -> we reset ready and reload.
  // (Edge-triggering on building true->false is fragile: on first open the snapshot is
  // fetched before the list query sets the engine's "wanted" flag, so the rising edge is
  // never observed and the picker would hang on "Loading objects…".)
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

  const loading = !ready;

  const name = snapshot?.referenceObjectName ?? NONE;
  const visible = snapshot?.referenceObjectVisible ?? true;
  const status = snapshot?.referenceObjectStatus ?? "none";
  const pos: Vec3 = snapshot?.referenceObjectPosition ?? [0, 0, 0];
  const rot: Vec3 = snapshot?.referenceObjectRotation ?? [0, 0, 0];
  const snapEnabled = snapshot?.snapEnabled ?? false;

  // Live search: case-insensitive substring on Name. The category filter
  // already shrank the list (props gone); this narrows it the rest of the way.
  const ql = query.trim().toLowerCase();
  const matches = (n: string) => ql === "" || n.toLowerCase().includes(ql);

  // Group the (search-filtered) objects by category for the <optgroup>s.
  const byCategory = new Map<ReferenceObjectCategory, string[]>();
  for (const o of objects) {
    if (!matches(o.name)) continue;
    const arr = byCategory.get(o.category) ?? [];
    arr.push(o.name);
    byCategory.set(o.category, arr);
  }
  let visibleCount = 0;
  for (const arr of byCategory.values()) visibleCount += arr.length;

  // The persisted/active selection may be absent from the visible list (mod
  // object, async load, or filtered by the search); surface it as a standalone
  // option so the <select> doesn't silently fall back to "None".
  const known =
    name === NONE ||
    (objects.some((o) => o.name === name) && matches(name));

  const selectObject = (n: string) =>
    void bridge.request({ kind: "engine/set/reference-object", params: { name: n } });

  const setVisible = (v: boolean) =>
    void bridge.request({ kind: "engine/set/reference-object-visible", params: { visible: v } });

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
            <label className="flex flex-col gap-1 text-xs text-text-2">
              Object
              <select
                value={name}
                onChange={(e) => selectObject(e.target.value)}
                aria-label="Reference object"
                size={8}
                className="rounded-md border border-border bg-bg-2 px-2 py-1 text-sm text-text"
              >
                <option value={NONE}>None</option>
                {name !== NONE && !known && <option value={name}>{name}</option>}
                {CATEGORY_ORDER.filter((c) => byCategory.has(c)).map((cat) => (
                  <optgroup key={cat} label={cat}>
                    {byCategory.get(cat)!.map((n) => (
                      <option key={n} value={n}>{n}</option>
                    ))}
                  </optgroup>
                ))}
              </select>
            </label>
            {ql !== "" && visibleCount === 0 && (
              <p className="text-xs text-text-3">No objects match “{query}”.</p>
            )}
          </>
        )}

        {statusNote && (
          <p role="alert" className="text-xs text-warning">{statusNote}</p>
        )}

        <label className="flex items-center gap-2 text-xs text-text-2">
          <input
            type="checkbox"
            checked={visible}
            onChange={(e) => setVisible(e.target.checked)}
            disabled={name === NONE}
            aria-label="Reference object visible"
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
            disabled={name === NONE}
            className="rounded px-2 py-0.5 text-xs text-text-2 transition hover:bg-panel-2 hover:text-text disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent outline-none"
            aria-label="Reset transform to origin"
            title="Reset position + rotation to 0"
          >
            Reset
          </button>
        </div>
        <div className="grid grid-cols-3 gap-2">
          {(["X", "Y", "Z"] as const).map((axis, i) => (
            <label key={axis} className="flex flex-col gap-1 text-xs text-text-2">
              {axis}
              <Spinner
                value={pos[i]}
                onChange={(v) => setPosAxis(i, v)}
                step={1}
                decimals={1}
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
                aria-label={`Rotation ${axis}`}
              />
            </label>
          ))}
        </div>

        {/* Persistent snap toggle. State + plumbing only — the drag-time
            snap apply (reads the engine snap state) lands in a separate task. */}
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

/**
 * ReferenceObjectPicker — thin <ToolPanel> wrapper around the body, kept as the
 * default slide-in form (parallels BackgroundPicker).
 */
export function ReferenceObjectPicker({ bridge, onClose }: Props) {
  return (
    <ToolPanel
      title="Reference object"
      onClose={onClose}
      bridge={bridge}
      occlusionId="tool-panel:reference-object"
    >
      <ReferenceObjectPickerBody bridge={bridge} />
    </ToolPanel>
  );
}
