// EmitterPropertyTabs — lower-left quadrant of the four-quadrant layout.
// Three tabs (Basic / Appearance / Physics) driven by Radix Tabs. Their form
// fields commit through `emitters/set-properties { id, patch: { ... } }`.
//
// Replaces the legacy `src/UI/Emitter.cpp` modal (873 LOC, ~150 control
// IDs). Mirrors the legacy tab structure 1:1: Basic / Appearance /
// Physics.
//
// Bridge surface:
//   - On selection change + on `emitters/tree/changed`: fetch via
//     `emitters/get-properties { id }`.
//   - Each field commit: `emitters/set-properties { id, patch: { ... } }`.
//
// Optimistic local update: each commit also applies the patch to local
// `properties` state immediately so the form doesn't flash on
// round-trip. A late-arriving `tree/changed` re-fetch is authoritative.
//
// `useBursts` mutex enabling (mirrors legacy):
//   - `useBursts === true` enables nBursts / burstDelay / nParticlesPerBurst
//     and disables nParticlesPerSecond.
//   - `useBursts === false` enables nParticlesPerSecond and disables
//     nBursts / burstDelay / nParticlesPerBurst.
//
// `randomRotation` enabling: when false, randomRotationDirection /
// Average / Variance disable.
//
// Text input (name) commits on blur — avoids per-keystroke bridge spam.
// Spinners commit per their existing semantics (Enter / blur / arrow /
// wheel / drag-release). Checkboxes commit on change.

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import * as Tabs from "@radix-ui/react-tabs";
import type { Bridge, EmitterPropertiesDto } from "@particle-editor/bridge-schema";
import { requestTreeRefetch } from "@/lib/tree-refetch";
import { AppearanceTab } from "./property-tabs/AppearanceTab";
import { BasicTab } from "./property-tabs/BasicTab";
import { TabTrigger } from "./property-tabs/fields";
import { PhysicsTab } from "./property-tabs/PhysicsTab";

export { BasicTab } from "./property-tabs/BasicTab";
export { AppearanceTab } from "./property-tabs/AppearanceTab";
export { PhysicsTab } from "./property-tabs/PhysicsTab";
export { FieldSpinner, TexturePickerField } from "./property-tabs/fields";

type Props = {
  bridge: Bridge;
};

export function EmitterPropertyTabs({ bridge }: Props) {
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [properties, setProperties] = useState<EmitterPropertiesDto | null>(null);
  // Discard stale responses if selection changes mid-flight.
  const inFlightFor = useRef<number | null>(null);

  // Seed selection from the engine snapshot.
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((snap) => {
        if (cancelled) return;
        setSelectedId(snap.selectedEmitterId);
      })
      .catch(() => { /* placeholder branch handles null */ });
    return () => { cancelled = true; };
  }, [bridge]);

  // Track live selection.
  useEffect(() => {
    const off = bridge.on("emitters/selected", (e) => {
      setSelectedId(e.payload.id);
    });
    return off;
  }, [bridge]);

  // Fetch helper. Discards responses for stale selection.
  const fetchProps = useCallback(
    (id: number | null, coalesceTreeRefetch = false) => {
      if (id === null) {
        setProperties(null);
        inFlightFor.current = null;
        return;
      }
      inFlightFor.current = id;
      const req = { kind: "emitters/get-properties", params: { id } } as const;
      const request = coalesceTreeRefetch ? requestTreeRefetch(bridge, req) : bridge.request(req);
      request
        .then((res) => {
          if (inFlightFor.current !== id) return;
          setProperties(res.properties);
        })
        .catch(() => {
          if (inFlightFor.current !== id) return;
          setProperties(null);
        });
    },
    [bridge],
  );

  // Re-fetch on selection change.
  useEffect(() => {
    fetchProps(selectedId);
  }, [fetchProps, selectedId]);

  // Re-fetch on tree mutations.
  useEffect(() => {
    const off = bridge.on("emitters/tree/changed", () => {
      fetchProps(selectedId, true);
    });
    return off;
  }, [bridge, fetchProps, selectedId]);

  // Commit helper — fires the bridge patch + optimistic local update.
  const commit = useCallback(
    (patch: Partial<EmitterPropertiesDto>) => {
      if (selectedId === null) return;
      // Optimistic local update so the spinner doesn't flash back to
      // the old value before the engine re-emits.
      setProperties((p) => (p === null ? p : { ...p, ...patch }));
      void bridge
        .request({
          kind: "emitters/set-properties",
          params: { id: selectedId, patch },
        })
        .catch(() => {
          // On failure, re-fetch the authoritative value so we don't
          // leave the form stuck on a value the engine refused.
          fetchProps(selectedId);
        });
    },
    [bridge, selectedId, fetchProps],
  );

  // Browse helper — opens the host-side native
  // texture dialog and resolves to the picked basename ("" if cancelled
  // or in browser/mock mode). TexturePickerField commits a non-empty
  // result through `commit`, same as the text input.
  const browseTexture = useCallback(
    async (slot: "color" | "bump"): Promise<string> => {
      try {
        const res = await bridge.request({
          kind: "textures/browse",
          params: { slot },
        });
        return res.filename ?? "";
      } catch {
        return "";
      }
    },
    [bridge],
  );

  // The tab strip is always mounted so the user can see the
  // Basic/Appearance/Physics structure (and pre-click a tab) before any
  // emitter is selected. The per-Content `renderBody` helper swaps in a
  // placeholder when no selection / loading, so only the active tab's
  // body shows the placeholder — three call sites, never duplicated.
  const renderBody = (content: (p: EmitterPropertiesDto) => ReactNode): ReactNode => {
    if (selectedId === null) {
      return (
        <div
          data-testid="emitter-property-tabs-placeholder"
          className="flex h-full items-center justify-center p-4 text-center text-xs text-text-3"
        >
          Select an emitter to edit its properties
        </div>
      );
    }
    if (properties === null) {
      return (
        <div className="flex h-full items-center justify-center p-4 text-xs text-text-3">
          Loading…
        </div>
      );
    }
    return content(properties);
  };

  return (
    <Tabs.Root
      data-testid="emitter-property-tabs"
      defaultValue="basic"
      className="flex h-full flex-col"
    >
      <Tabs.List
        className="flex shrink-0 border-b border-border bg-bg"
        aria-label="Emitter property tabs"
      >
        <TabTrigger value="basic" label="Basic" />
        <TabTrigger value="appearance" label="Appearance" />
        <TabTrigger value="physics" label="Physics" />
      </Tabs.List>
      {/* All three tabs render <div className="inspector"> inside, which
          owns the padding — so the Tabs.Content wrappers omit Tailwind
          padding to avoid doubling. */}
      <Tabs.Content
        value="basic"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-basic-content"
      >
        {renderBody((p) => <BasicTab properties={p} onCommit={commit} />)}
      </Tabs.Content>
      <Tabs.Content
        value="appearance"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-appearance-content"
      >
        {renderBody((p) => (
          <AppearanceTab
            properties={p}
            onCommit={commit}
            onBrowseTexture={browseTexture}
            bridge={bridge}
          />
        ))}
      </Tabs.Content>
      <Tabs.Content
        value="physics"
        className="inspector-tab-scroll flex-1 min-h-0 overflow-y-auto outline-none focus-ring-inset scrollbar-stable fade-in-fast"
        data-testid="tab-physics-content"
      >
        {renderBody((p) => <PhysicsTab properties={p} onCommit={commit} />)}
      </Tabs.Content>
    </Tabs.Root>
  );
}
