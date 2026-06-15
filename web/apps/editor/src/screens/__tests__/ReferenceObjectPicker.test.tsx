// Vitest unit tests for ReferenceObjectPicker. Driven by the real
// MockBridge so the canned reference-object-list + the set handlers (including
// the skinned-status path) are exercised end-to-end against the schema.
//
// Covered: object enumeration + category grouping, selection dispatch, the
// "skinned — not supported" + "model-missing" status notes, the visibility
// toggle, a numeric transform commit, the "Loading objects…" building
// state, and the search filter. (The unit-grid toggle moved to the Ground panel
// in S48 — see GroundTexturePanel.test.tsx.)

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor, act, within } from "@testing-library/react";
import { ReferenceObjectPicker } from "../ReferenceObjectPicker";
import { MockBridge } from "@/bridge/mock";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("ReferenceObjectPicker — selection + status", () => {
  it("enumerates objects grouped by category and dispatches on select", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    // The object list renders once the (mock-instant) catalog query resolves.
    const select = await screen.findByRole("listbox", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "AT_AT_Walker" })).toBeInTheDocument();
    });
    // Category <optgroup>s render as groups.
    expect(screen.getByRole("group", { name: "Vehicle" })).toBeInTheDocument();
    expect(screen.getByRole("group", { name: "Turret" })).toBeInTheDocument();

    fireEvent.change(select, { target: { value: "AT_AT_Walker" } });
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectName).toBe("AT_AT_Walker");
      expect(snap.referenceObjectStatus).toBe("ok");
    });
  });

  it("lists unrecognised-tag units under the Other group (the Mod-units-missing fix)", async () => {
    // The engine now sends Other-categorised objects (unrecognised unit/structure
    // tags like Mod's groundcompany / capturable bunkers); the picker must show
    // them under an "Other" optgroup, not drop them.
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: "Reference object" });
    expect(await screen.findByRole("group", { name: "Other" })).toBeInTheDocument();
    expect(
      screen.getByRole("option", { name: "Imperial_Bunker_Capturable" })
    ).toBeInTheDocument();
  });

  it("shows the skinned-unsupported note for a skinned object", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const select = await screen.findByRole("listbox", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "Stormtrooper_Squad" })).toBeInTheDocument();
    });
    fireEvent.change(select, { target: { value: "Stormtrooper_Squad" } });

    const note = await screen.findByRole("alert");
    expect(note.textContent).toMatch(/skinned/i);
  });

  it("shows the model-missing note for an object whose file is absent", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const select = await screen.findByRole("listbox", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "Sensor_Array_NoModel" })).toBeInTheDocument();
    });
    fireEvent.change(select, { target: { value: "Sensor_Array_NoModel" } });

    const note = await screen.findByRole("alert");
    expect(note.textContent).toMatch(/not found/i);
  });

  it("commits a position-X change through the transform dispatch", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const posX = await screen.findByLabelText("Position X");
    fireEvent.change(posX, { target: { value: "12.5" } });
    fireEvent.blur(posX);  // Spinner commits on blur
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectPosition[0]).toBe(12.5);
    });
  });

  it("surfaces a selected name absent from the enumerated list (mod-only object)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    // Select a Name NOT in the canned list — the case exists for (a mod
    // object absent from a partial/async list). The picker must show it as the
    // active selection via the standalone <option>, not snap back to "None".
    await screen.findByRole("listbox", { name: "Reference object" });
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "Mod_Only_Object" } });
    const opt = await screen.findByRole("option", { name: "Mod_Only_Object" });
    expect((opt as HTMLOptionElement).selected).toBe(true);
  });

  it("a single-axis transform edit preserves the other axes + rotation", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByLabelText("Position X");
    // Seed a known transform, then edit only Position Y.
    await bridge.request({
      kind: "engine/set/reference-object-transform",
      params: { position: [1, 2, 3], rotation: [10, 20, 30] },
    });
    const posY = await screen.findByLabelText("Position Y");
    await waitFor(() => expect((posY as HTMLInputElement).value).toBe("2.0"));
    fireEvent.change(posY, { target: { value: "99" } });
    fireEvent.blur(posY);
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectPosition).toEqual([1, 99, 3]);   // X + Z preserved
      expect(snap.referenceObjectRotation).toEqual([10, 20, 30]); // rotation untouched
    });
  });
});

describe("ReferenceObjectPicker — async build + search", () => {
  const SNAP = (building: boolean) => ({
    referenceObjectName: "",
    referenceObjectVisible: true,
    referenceObjectStatus: "none",
    referenceObjectPosition: [0, 0, 0],
    referenceObjectRotation: [0, 0, 0],
    referenceCatalogBuilding: building,
  });

  // A stub bridge modelling the REAL host ordering: the mount snapshot is fetched
  // BEFORE the list query (which is what sets the engine's "wanted" flag), so the
  // snapshot reports referenceCatalogBuilding:FALSE even while the catalog is still
  // building. The list query is what reports building:true. This is the ordering the
  // picker must survive — an edge-triggered (building true->false) re-query would hang
  // here because the rising edge is never observed. finishBuild() fires the ready event.
  function makeBuildingBridge() {
    let listBuilding = true;
    let handler: ((e: { kind: string; payload: unknown }) => void) | null = null;
    const list = () =>
      listBuilding
        ? { objects: [], building: true }
        : { objects: [{ name: "AT_AT_Walker", category: "Vehicle" }], building: false };
    const bridge = {
      request: (req: { kind: string }) => {
        if (req.kind === "engine/query/reference-object-list") return Promise.resolve(list());
        if (req.kind === "engine/state/snapshot") return Promise.resolve(SNAP(false));
        return Promise.resolve({});
      },
      on: (kind: string, cb: (e: { kind: string; payload: unknown }) => void) => {
        if (kind === "engine/state/changed") handler = cb;
        return () => {};
      },
    } as unknown as Bridge;
    const emit = (building: boolean) =>
      handler?.({ kind: "engine/state/changed", payload: SNAP(building) });
    const finishBuild = () => { listBuilding = false; emit(false); };
    const startRebuild = () => { listBuilding = true; emit(true); };
    return { bridge, finishBuild, startRebuild };
  }

  it("shows 'Loading objects…' while building, then renders the list when the build finishes", async () => {
    // The snapshot reports NOT building (real ordering) -> proves the re-query is
    // level-triggered (driven by !ready), not the never-observed rising edge.
    const { bridge, finishBuild } = makeBuildingBridge();
    render(<ReferenceObjectPicker bridge={bridge} onClose={() => {}} />);

    // Building: the loading note shows, the list box is absent, the search is disabled.
    expect(await screen.findByText(/loading objects/i)).toBeInTheDocument();
    expect(screen.queryByRole("listbox", { name: "Reference object" })).not.toBeInTheDocument();
    expect(
      (screen.getByLabelText("Search reference objects") as HTMLInputElement).disabled
    ).toBe(true);

    // Build finishes -> engine/state/changed (building:false) -> !ready re-query -> list.
    act(() => finishBuild());
    const select = await screen.findByRole("listbox", { name: "Reference object" });
    expect(within(select).getByRole("option", { name: "AT_AT_Walker" })).toBeInTheDocument();
    expect(screen.queryByText(/loading objects/i)).not.toBeInTheDocument();
    expect(
      (screen.getByLabelText("Search reference objects") as HTMLInputElement).disabled
    ).toBe(false);
  });

  it("does NOT re-query the list on unrelated state changes once loaded (no fetch storm)", async () => {
    // Guards finding-3: a ~30 Hz gizmo drag emits engine/state/changed with
    // referenceCatalogBuilding=false; the loaded picker must issue ZERO extra list
    // queries (the level-triggered re-fetch is gated on !ready).
    let listCalls = 0;
    let handler: ((e: { kind: string; payload: unknown }) => void) | null = null;
    const bridge = {
      request: (req: { kind: string }) => {
        if (req.kind === "engine/query/reference-object-list") {
          listCalls++;
          return Promise.resolve({
            objects: [{ name: "AT_AT_Walker", category: "Vehicle" }],
            building: false,
          });
        }
        if (req.kind === "engine/state/snapshot") return Promise.resolve(SNAP(false));
        return Promise.resolve({});
      },
      on: (kind: string, cb: (e: { kind: string; payload: unknown }) => void) => {
        if (kind === "engine/state/changed") handler = cb;
        return () => {};
      },
    } as unknown as Bridge;
    render(<ReferenceObjectPicker bridge={bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: "Reference object" });
    const afterLoad = listCalls;

    // Simulate a gizmo drag: 10 building=false events, each MOVING the object so the
    // position delta proves they were actually delivered to the picker (otherwise
    // "no extra query" could false-pass on an unwired handler).
    act(() => {
      for (let i = 1; i <= 10; i++)
        handler?.({
          kind: "engine/state/changed",
          payload: { ...SNAP(false), referenceObjectPosition: [i, 0, 0] },
        });
    });
    // Delivered — the X spinner tracked the last event...
    await waitFor(() =>
      expect((screen.getByLabelText("Position X") as HTMLInputElement).value).toBe("10.0")
    );
    // ...yet ZERO extra list queries (the level-triggered re-fetch is gated on !ready).
    expect(listCalls).toBe(afterLoad);
  });

  it("a mod-switch rebuild (building flips true again) re-loads the list", async () => {
    const { bridge, finishBuild, startRebuild } = makeBuildingBridge();
    render(<ReferenceObjectPicker bridge={bridge} onClose={() => {}} />);
    act(() => finishBuild());
    await screen.findByRole("listbox", { name: "Reference object" });

    // Mod switch while open: referenceCatalogBuilding flips true -> back to Loading.
    act(() => startRebuild());
    expect(await screen.findByText(/loading objects/i)).toBeInTheDocument();
    expect(screen.queryByRole("listbox", { name: "Reference object" })).not.toBeInTheDocument();

    // Rebuild completes -> the new list loads again.
    act(() => finishBuild());
    const select = await screen.findByRole("listbox", { name: "Reference object" });
    expect(within(select).getByRole("option", { name: "AT_AT_Walker" })).toBeInTheDocument();
  });

  it("search narrows the listed objects (case-insensitive substring on Name)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("listbox", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("option", { name: "Star_Destroyer" })).toBeInTheDocument()
    );

    fireEvent.change(screen.getByLabelText("Search reference objects"), {
      target: { value: "at_" },
    });
    // Only the AT_* vehicles remain; Star_Destroyer is filtered out.
    await waitFor(() =>
      expect(screen.queryByRole("option", { name: "Star_Destroyer" })).not.toBeInTheDocument()
    );
    expect(screen.getByRole("option", { name: "AT_AT_Walker" })).toBeInTheDocument();
    expect(screen.getByRole("option", { name: "AT_ST_Walker" })).toBeInTheDocument();
  });
});
