// EmitterPropertyTabs — lower-left quadrant of the four-quadrant layout.
// Three tabs (Basic / Appearance / Physics) driven by Radix Tabs. Their form
// fields commit through `emitters/set-properties { id, patch: { ... } }`.
//
// Replaces the legacy Win32 editor's emitter-properties modal (~150
// control IDs). Mirrors the legacy tab structure 1:1: Basic / Appearance /
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
import { fireAndReport, useStatusFeedback } from "@/lib/status-feedback";
import { Button } from "@/primitives/Button";
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
  const [selection, setSelection] = useState<{ bridge: Bridge; id: number | null }>({ bridge, id: null });
  const selectedId = selection.bridge === bridge ? selection.id : null;
  const [loaded, setLoaded] = useState<{ bridge: Bridge; id: number; properties: EmitterPropertiesDto } | null>(null);
  const properties = loaded?.bridge === bridge && loaded.id === selectedId ? loaded.properties : null;
  const [loading, setLoading] = useState(false);
  const [readError, setReadError] = useState<string | null>(null);
  const currentSelection = useRef(selection);
  const selectionGeneration = useRef(0);
  const readGeneration = useRef(0);

  // Live selection wins over the one-shot seed, even when it selects null.
  useEffect(() => {
    const select = (id: number | null) => {
      const previous = currentSelection.current;
      if (previous.bridge !== bridge || previous.id !== id) {
        readGeneration.current += 1;
        setLoaded(null);
        setReadError(null);
        setLoading(false);
      }
      currentSelection.current = { bridge, id };
      setSelection(currentSelection.current);
    };
    selectionGeneration.current += 1;
    select(null);
    const seedGeneration = selectionGeneration.current;
    const off = bridge.on("emitters/selected", (e) => {
      selectionGeneration.current += 1;
      select(e.payload.id);
    });
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((snap) => {
        if (seedGeneration !== selectionGeneration.current) return;
        select(snap.selectedEmitterId);
      })
      .catch(() => { /* placeholder branch handles null */ });
    return () => {
      selectionGeneration.current += 1;
      readGeneration.current += 1;
      currentSelection.current = { bridge, id: null };
      off();
    };
  }, [bridge]);

  // Display identity is separate from request identity: a same-emitter refresh
  // keeps the form mounted, while only the latest read can replace its values.
  const fetchProps = useCallback(
    (id: number | null, coalesceTreeRefetch = false) => {
      const generation = ++readGeneration.current;
      setReadError(null);
      if (id === null) {
        setLoaded(null);
        setLoading(false);
        return;
      }
      setLoading(true);
      const req = { kind: "emitters/get-properties", params: { id } } as const;
      const request = coalesceTreeRefetch ? requestTreeRefetch(bridge, req) : bridge.request(req);
      const isCurrent = () => generation === readGeneration.current
        && currentSelection.current.bridge === bridge && currentSelection.current.id === id;
      request
        .then((res) => {
          if (!isCurrent()) return;
          setLoaded({ bridge, id, properties: res.properties });
          setLoading(false);
        })
        .catch((err: unknown) => {
          if (!isCurrent()) return;
          setReadError(`Couldn't load emitter properties: ${err instanceof Error ? err.message : String(err)}`);
          setLoading(false);
        });
    },
    [bridge],
  );

  // Re-fetch on selection change.
  useEffect(() => {
    fetchProps(selectedId);
  }, [fetchProps, selectedId, selection]);

  // Re-fetch on tree mutations.
  useEffect(() => {
    const off = bridge.on("emitters/tree/changed", () => {
      if (currentSelection.current.bridge === bridge) {
        fetchProps(currentSelection.current.id, true);
      }
    });
    return off;
  }, [bridge, fetchProps]);

  // Commit helper — fires the bridge patch + optimistic local update.
  const commit = useCallback(
    (patch: Partial<EmitterPropertiesDto>) => {
      if (selectedId === null || properties === null
        || currentSelection.current !== selection) return;
      const selectedGeneration = selectionGeneration.current;
      const generation = ++readGeneration.current;
      setLoading(false);
      // Optimistic local update so the spinner doesn't flash back to
      // the old value before the engine re-emits.
      setLoaded((p) => p?.bridge === bridge && p.id === selectedId
        ? { ...p, properties: { ...p.properties, ...patch } } : p);
      void fireAndReport(bridge, {
        kind: "emitters/set-properties",
        params: { id: selectedId, patch },
      }, "Edit emitter properties")
        .then((res) => {
          if (selectedGeneration !== selectionGeneration.current
            || currentSelection.current.bridge !== bridge || currentSelection.current.id !== selectedId) return;
          if (res !== undefined && res.skipped.length === 0) return;
          if (res !== undefined) {
            useStatusFeedback.getState().announce(`Couldn't apply emitter properties: ${res.skipped.join(", ")}`);
          }
          if (generation !== readGeneration.current) return;
          // On failure, re-fetch the authoritative value so we don't
          // leave the form stuck on a value the engine refused.
          fetchProps(selectedId);
        });
    },
    [bridge, selection, selectedId, properties, fetchProps],
  );

  // Browse helper — opens the host-side native
  // texture dialog and resolves to the picked basename ("" if cancelled
  // or in browser/mock mode). TexturePickerField commits a non-empty
  // result through `commit`, same as the text input.
  const browseTexture = useCallback(
    async (slot: "color" | "bump"): Promise<string> => {
      const res = await fireAndReport(bridge, {
        kind: "textures/browse",
        params: { slot },
      }, "Browse texture");
      return res?.filename ?? "";
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
    const error = readError === null ? null : (
      <div className="flex flex-col items-center gap-2 p-4 text-center text-xs">
        <div role="alert" className="text-danger-fg">{readError}</div>
        <Button variant="secondary" onClick={() => fetchProps(selectedId)}>Retry</Button>
      </div>
    );
    if (properties === null) {
      if (error) return error;
      return (
        <div role="status" className="flex h-full items-center justify-center p-4 text-xs text-text-3">
          Loading…
        </div>
      );
    }
    return <>{error}{content(properties)}</>;
  };

  return (
    <Tabs.Root
      data-testid="emitter-property-tabs"
      aria-busy={loading}
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
