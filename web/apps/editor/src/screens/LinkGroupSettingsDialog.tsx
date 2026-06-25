// LinkGroupSettingsDialog.
//
// Link-group settings surface in the new UI. On open, fetches the link-group's
// current exempt-field set via `linkGroups/list-exempt-fields` and
// renders a checkbox-per-field list. OK commits the toggles via
// `linkGroups/set-exempt-fields`; Cancel discards. Reset All sets all
// checkboxes off in local state (caller still has to OK to commit —
// matches the legacy "edit-locally, OK-to-confirm" pattern).
//
// Checkbox semantics: checked = SHARED across the group (propagates
// on edit). Unchecked = EXEMPT (per-emitter, no propagation). This is
// the opposite of the underlying `LinkExemptFlags` data model — the
// flag is named "exempt" so true=per-emitter. The wire / dialog
// inverts before crossing the data/UI boundary, matching legacy
// `LinkGroupSettings_PopulateChecks` at [src/UI/EmitterList.cpp:2466].
//
// The full legacy table (kLinkSettingsFields at
// [src/UI/EmitterList.cpp:2381]) covers ~50 fields grouped by
// category. The new-UI dialog uses the same wire-name set as the C++
// host's `kLinkFieldTable` in BridgeDispatcher.cpp; the list of names
// is rendered in fetch order so the host owns the canonical column
// list (a future field addition lands on both sides with one host
// change).
//
// Driven by the `tree-context` atom with `targetLinkGroupId` carrying
// the group ID.

import { useEffect, useRef, useState } from "react";
import { ChevronDown, ChevronRight } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { useTreeContextStore } from "@/lib/tree-context";

// Display labels for the wire-name field set. Names not in this map
// fall back to the wire name itself so a future field addition still
// renders (just without a friendly label). Mirrors the labels in the
// legacy `kLinkSettingsFields` table at [src/UI/EmitterList.cpp:2381].
const FIELD_LABELS: Readonly<Record<string, string>> = {
  colorTexture:            "Color texture",
  normalTexture:           "Normal texture",
  trackIndex:              "Atlas index curve",
  trackRed:                "Red curve",
  trackGreen:              "Green curve",
  trackBlue:               "Blue curve",
  trackAlpha:              "Alpha curve",
  trackScale:              "Scale curve",
  trackRotationSpeed:      "Rotation speed curve",
  lifetime:                "Lifetime",
  initialDelay:            "Initial delay",
  burstDelay:              "Burst delay",
  nBursts:                 "Number of bursts",
  nParticlesPerBurst:      "Particles per burst",
  nParticlesPerSecond:     "Particles per second",
  useBursts:               "Use bursts",
  gravity:                 "Gravity",
  acceleration:            "Acceleration",
  inwardSpeed:             "Inward speed",
  inwardAcceleration:      "Inward acceleration",
  bounciness:              "Bounciness",
  groundBehavior:          "Ground behavior",
  objectSpaceAcceleration: "Object-space acceleration",
  affectedByWind:          "Affected by wind",
  blendMode:               "Blend mode",
  textureSize:             "Texture size",
  nTriangles:              "Triangles per particle",
  randomScalePerc:         "Random scale %",
  randomLifetimePerc:      "Random lifetime %",
  hasTail:                 "Has tail",
  tailSize:                "Tail size",
  noDepthTest:             "No depth test",
  randomColors:            "Random colors",
  isWeatherParticle:       "Weather particle",
  weatherCubeSize:         "Weather cube size",
  weatherCubeDistance:     "Weather cube distance",
  weatherFadeoutDistance:  "Weather fadeout distance",
  randomRotation:          "Random rotation",
  randomRotationDirection: "Random rotation direction",
  randomRotationAverage:   "Random rotation average",
  randomRotationVariance:  "Random rotation variance",
  linkToSystem:            "Link to system",
  parentLinkStrength:      "Parent link strength",
  doColorAddGrayscale:     "Color-add grayscale",
  isHeatParticle:          "Heat particle",
  isWorldOriented:         "World-oriented",
  freezeTime:              "Freeze time",
  skipTime:                "Skip time",
  emitFromMesh:            "Emit from mesh",
  emitFromMeshOffset:      "Emit from mesh offset",
  groupSpeed:              "Speed params (random box)",
  groupLifetime:           "Lifetime params (random box)",
  groupPosition:           "Position params (random box)",
};

// Field organization — groups the flag set into categories that mirror
// the editor's own structure (the Curves editor + the Basic / Appearance
// / Physics inspector tabs) so a user finds a param where they'd edit it.
// Weather is folded into Appearance. The union of these lists is the full
// known field universe; any host field not listed here drops into an
// "Other" section at render time (forward-compat with field additions).
type FieldCategory = { id: string; label: string; fields: readonly string[] };
const FIELD_CATEGORIES: readonly FieldCategory[] = [
  {
    id: "curves",
    label: "Curves",
    fields: [
      "trackRed", "trackGreen", "trackBlue", "trackAlpha",
      "trackScale", "trackRotationSpeed", "trackIndex",
    ],
  },
  {
    id: "basic",
    label: "Basic",
    fields: [
      "lifetime", "initialDelay", "randomLifetimePerc",
      "useBursts", "nBursts", "burstDelay",
      "nParticlesPerBurst", "nParticlesPerSecond",
      "emitFromMesh", "emitFromMeshOffset", "groupLifetime",
      "linkToSystem", "parentLinkStrength",
    ],
  },
  {
    id: "appearance",
    label: "Appearance",
    fields: [
      "colorTexture", "normalTexture",
      "blendMode", "textureSize", "nTriangles", "randomScalePerc",
      "noDepthTest", "isWorldOriented", "isHeatParticle",
      "hasTail", "tailSize",
      "randomColors", "doColorAddGrayscale",
      "randomRotation", "randomRotationDirection",
      "randomRotationAverage", "randomRotationVariance",
      "isWeatherParticle", "weatherCubeSize",
      "weatherCubeDistance", "weatherFadeoutDistance",
    ],
  },
  {
    id: "physics",
    label: "Physics",
    fields: [
      "gravity", "acceleration", "inwardSpeed", "inwardAcceleration",
      "objectSpaceAcceleration", "affectedByWind",
      "bounciness", "groundBehavior",
      "groupSpeed", "groupPosition",
      "freezeTime", "skipTime",
    ],
  },
];

// One collapsible category: a header band (collapse chevron + label +
// tri-state "share all" checkbox) over the per-field checkboxes. The
// header checkbox reads indeterminate when the category is mixed.
function CategorySection({
  label,
  fields,
  exempt,
  collapsed,
  onToggleCollapse,
  onToggleField,
  onToggleCategory,
}: {
  label: string;
  fields: readonly string[];
  exempt: Set<string>;
  collapsed: boolean;
  onToggleCollapse: () => void;
  onToggleField: (field: string, sharedNext: boolean) => void;
  onToggleCategory: (fields: readonly string[], sharedNext: boolean) => void;
}) {
  const sharedCount = fields.reduce((n, f) => n + (exempt.has(f) ? 0 : 1), 0);
  const allShared = sharedCount === fields.length;
  const noneShared = sharedCount === 0;
  const catRef = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (catRef.current) catRef.current.indeterminate = !allShared && !noneShared;
  }, [allShared, noneShared]);
  const Chevron = collapsed ? ChevronRight : ChevronDown;
  return (
    <div className="mb-1">
      <div className="flex items-center gap-2 rounded bg-bg-2 px-2 py-1">
        <button
          type="button"
          onClick={onToggleCollapse}
          aria-expanded={!collapsed}
          className="flex flex-1 items-center gap-1 text-left focus-ring"
        >
          <Chevron className="size-3.5 text-text-3" aria-hidden="true" />
          <span className="text-xs font-medium text-text">{label}</span>
          <span className="text-[10px] text-text-3">
            {sharedCount}/{fields.length} shared
          </span>
        </button>
        <input
          ref={catRef}
          type="checkbox"
          checked={allShared}
          aria-label={`Share all ${label}`}
          onChange={(e) => onToggleCategory(fields, e.target.checked)}
        />
      </div>
      <div className="collapse-anim" data-open={!collapsed}>
        <div className="pl-4">
          {fields.map((field) => (
            <label
              key={field}
              className="flex cursor-pointer items-center gap-2 rounded px-2 py-1 hover:bg-panel-2"
            >
              <input
                type="checkbox"
                data-field={field}
                checked={!exempt.has(field)}
                onChange={(e) => onToggleField(field, e.target.checked)}
              />
              <span className="text-xs text-text">
                {FIELD_LABELS[field] ?? field}
              </span>
            </label>
          ))}
        </div>
      </div>
    </div>
  );
}

type Props = {
  bridge: Bridge;
};

type LoadState =
  | { kind: "loading" }
  | { kind: "loaded"; exempt: Set<string> }
  | { kind: "error"; message: string };

export function LinkGroupSettingsDialog({ bridge }: Props) {
  const open = useTreeContextStore((s) => s.open === "link-group");
  const groupId = useTreeContextStore((s) => s.targetLinkGroupId);
  const close = useTreeContextStore((s) => s.close);

  const [state, setState] = useState<LoadState>({ kind: "loading" });
  // Collapsed category ids. Empty = all expanded (the default).
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set());
  // Members the proposed exempt set would
  // overwrite to the canonical value when a now-exempt field becomes
  // shared. Fetched reactively as the local exempt set changes; shown
  // INLINE before OK (which resolves it host-side). Read-only preview.
  const [conflicts, setConflicts] = useState<{ id: number; fields: string[] }[]>([]);

  // Fetch the current exempt set when the dialog opens. Each open is a
  // fresh fetch so external edits (e.g. another bridge client writing
  // through the link-group settings surface) are reflected immediately.
  useEffect(() => {
    if (!open || groupId === null) {
      setState({ kind: "loading" });
      return;
    }
    let cancelled = false;
    setState({ kind: "loading" });
    bridge
      .request({ kind: "linkGroups/list-exempt-fields", params: { groupId } })
      .then((r) => {
        if (cancelled) return;
        setState({ kind: "loaded", exempt: new Set(r.fields) });
      })
      .catch((err) => {
        if (cancelled) return;
        setState({
          kind: "error",
          message: err instanceof Error ? err.message : String(err),
        });
      });
    return () => {
      cancelled = true;
    };
  }, [bridge, open, groupId]);

  // Reactively preview which members the CURRENT (proposed) exempt set would
  // overwrite when a now-exempt field becomes shared, so the form can list
  // them BEFORE the user commits. Read-only; OK always proceeds (and the host
  // resolves the disagreement on commit). Re-runs whenever the local exempt
  // set changes — `state` is a fresh object on every toggle.
  useEffect(() => {
    if (!open || groupId === null || state.kind !== "loaded") {
      setConflicts([]);
      return;
    }
    let cancelled = false;
    bridge
      .request({
        kind: "linkGroups/diff-exempt-change",
        params: { groupId, exempt: Array.from(state.exempt) },
      })
      .then((r) => {
        if (!cancelled) setConflicts(r?.conflicts ?? []);
      })
      .catch(() => {
        if (!cancelled) setConflicts([]);
      });
    return () => {
      cancelled = true;
    };
  }, [bridge, open, groupId, state]);

  const toggleField = (field: string, sharedNext: boolean) => {
    setState((cur) => {
      if (cur.kind !== "loaded") return cur;
      const next = new Set(cur.exempt);
      // sharedNext === true means checked → SHARED → not exempt.
      if (sharedNext) next.delete(field);
      else next.add(field);
      return { kind: "loaded", exempt: next };
    });
  };

  // Flip every field in a category at once (the header tri-state toggle).
  const setCategoryShared = (fields: readonly string[], shared: boolean) => {
    setState((cur) => {
      if (cur.kind !== "loaded") return cur;
      const next = new Set(cur.exempt);
      for (const f of fields) {
        if (shared) next.delete(f);
        else next.add(f);
      }
      return { kind: "loaded", exempt: next };
    });
  };

  const toggleCollapse = (id: string) => {
    setCollapsed((cur) => {
      const next = new Set(cur);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  const resetAll = () => {
    setState((cur) => {
      if (cur.kind !== "loaded") return cur;
      // Reset = all SHARED (no fields exempt). The user must still
      // click OK to commit — matches legacy "edit-locally, OK to
      // confirm" pattern.
      return { kind: "loaded", exempt: new Set() };
    });
  };

  const handleOk = () => {
    if (groupId === null) return;
    if (state.kind !== "loaded") return;
    void bridge.request({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId, fields: Array.from(state.exempt) },
    });
    close();
  };

  // Host fields not covered by any category (forward-compat) → "Other".
  const otherFields: string[] =
    state.kind === "loaded"
      ? (() => {
          const categorized = new Set(
            FIELD_CATEGORIES.flatMap((c) => c.fields),
          );
          return Array.from(state.exempt).filter((f) => !categorized.has(f));
        })()
      : [];

  return (
    <Modal
      open={open}
      onOpenChange={(o) => {
        if (!o) close();
      }}
      title={`Link Group ${groupId ?? ""} Settings`}
      size="md"
    >
      <Modal.Body>
        {state.kind === "loading" && (
          <div className="text-sm text-text-2">Loading…</div>
        )}
        {state.kind === "error" && (
          <div data-testid="link-group-error" className="text-sm text-danger-fg">
            Could not load exempt fields: {state.message}
          </div>
        )}
        {state.kind === "loaded" && (
          <div className="flex flex-col gap-1 text-sm">
            <p className="mb-2 text-[11px] leading-relaxed text-text-3">
              Checked fields are <em>shared</em> across the link group —
              edits propagate to every member. Unchecked fields are{" "}
              <em>per-emitter</em>.
            </p>
            {(() => {
              const conflictFields = Array.from(
                new Set(conflicts.flatMap((c) => c.fields)),
              );
              if (conflictFields.length === 0) return null;
              const n = conflictFields.length;
              const m = conflicts.length;
              return (
                <div
                  data-testid="link-settings-conflict-inline"
                  className="mb-2 rounded border border-warning/60 bg-warning/15 px-2 py-1.5 text-[11px] leading-relaxed text-warning-fg"
                >
                  <p className="font-medium">
                    Sharing {n} {n === 1 ? "field" : "fields"} will overwrite{" "}
                    {m} {m === 1 ? "emitter" : "emitters"} with the canonical
                    (first-in-tree-order) value:
                  </p>
                  <p className="mt-0.5 font-medium text-warning-fg">
                    {conflictFields.map((f) => FIELD_LABELS[f] ?? f).join(", ")}
                  </p>
                </div>
              );
            })()}
            <div className="link-settings-scroll max-h-[40vh] overflow-y-auto pr-1">
              {FIELD_CATEGORIES.map((cat) => (
                <CategorySection
                  key={cat.id}
                  label={cat.label}
                  fields={cat.fields}
                  exempt={state.exempt}
                  collapsed={collapsed.has(cat.id)}
                  onToggleCollapse={() => toggleCollapse(cat.id)}
                  onToggleField={toggleField}
                  onToggleCategory={setCategoryShared}
                />
              ))}
              {otherFields.length > 0 && (
                <CategorySection
                  label="Other"
                  fields={otherFields}
                  exempt={state.exempt}
                  collapsed={collapsed.has("other")}
                  onToggleCollapse={() => toggleCollapse("other")}
                  onToggleField={toggleField}
                  onToggleCategory={setCategoryShared}
                />
              )}
            </div>
          </div>
        )}
      </Modal.Body>
      <Modal.Footer>
        <button
          type="button"
          onClick={resetAll}
          disabled={state.kind !== "loaded"}
          className="mr-auto rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 focus-ring disabled:cursor-not-allowed disabled:opacity-40"
        >
          Reset All
        </button>
        <Modal.CancelButton>Cancel</Modal.CancelButton>
        <Modal.OkButton
          onClick={handleOk}
          disabled={state.kind !== "loaded"}
        >
          OK
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
