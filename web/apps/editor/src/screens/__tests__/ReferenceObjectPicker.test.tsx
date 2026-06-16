// / Vitest unit tests for ReferenceObjectPicker. Driven by the real
// MockBridge so the canned reference-object-list + the set handlers (including
// the skinned-status path) are exercised end-to-end against the schema.
//
// Covered: object enumeration into the collapsible Heroes / Ground / Space tree,
// selection dispatch (click a tree item), section + bucket collapse/expand, the
// "skinned — not supported" + "model-missing" status notes, the visibility
// toggle, a numeric transform commit, the "Loading objects…" building
// state, and the search filter (which force-expands so matches stay visible).

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor, act, within } from "@testing-library/react";
import { ReferenceObjectPicker } from "../ReferenceObjectPicker";
import { MockBridge } from "@/bridge/mock";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("ReferenceObjectPicker — selection + status", () => {
  it("enumerates objects into Heroes/Ground/Space sections and dispatches on click-select", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const tree = await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
    });
    // Top-level sections render as disclosure headers (default expanded).
    expect(screen.getByRole("treeitem", { name: /Ground/ })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: /Space/ })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: /Heroes/ })).toBeInTheDocument();
    // A space capital and a hero land in their sections.
    expect(within(tree).getByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument();
    expect(within(tree).getByRole("treeitem", { name: "Darth_Vader" })).toBeInTheDocument();

    fireEvent.click(screen.getByRole("treeitem", { name: "AT_AT_Walker" }));
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectName).toBe("AT_AT_Walker");
      expect(snap.referenceObjectStatus).toBe("ok");
    });
  });

  it("collapses a section to hide its items, and expands it again", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => expect(screen.getByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument());

    const spaceHeader = screen.getByRole("treeitem", { name: /Space/ });
    expect(spaceHeader).toHaveAttribute("aria-expanded", "true");
    fireEvent.click(spaceHeader); // collapse
    await waitFor(() =>
      expect(screen.queryByRole("treeitem", { name: "Star_Destroyer" })).not.toBeInTheDocument()
    );
    expect(screen.getByRole("treeitem", { name: /Space/ })).toHaveAttribute("aria-expanded", "false");
    // Ground items are unaffected.
    expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();

    fireEvent.click(screen.getByRole("treeitem", { name: /Space/ })); // expand again
    expect(await screen.findByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument();
  });

  it("lists an unrecognised-tag unit under Ground ▸ Other (the Mod-units-missing fix)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("tree", { name: "Reference object" });
    // bucket "Other" header + the item both render (Imperial_Bunker_Capturable is Ground/Unit/Other).
    expect(await screen.findByRole("treeitem", { name: /Other/ })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: "Imperial_Bunker_Capturable" })).toBeInTheDocument();
  });

  it("shows the skinned-unsupported note for a skinned object", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("treeitem", { name: "Stormtrooper_Squad" })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("treeitem", { name: "Stormtrooper_Squad" }));

    const note = await screen.findByRole("alert");
    expect(note.textContent).toMatch(/skinned/i);
  });

  it("shows the model-missing note for an object whose file is absent", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("treeitem", { name: "Sensor_Array_NoModel" })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("treeitem", { name: "Sensor_Array_NoModel" }));

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
    await screen.findByRole("tree", { name: "Reference object" });
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "Mod_Only_Object" } });
    // The picker shows it as a standalone selected row, not snapping back to None.
    const opt = await screen.findByRole("treeitem", { name: "Mod_Only_Object" });
    expect(opt).toHaveAttribute("aria-selected", "true");
  });

  it("reflects snapEnabled and dispatches engine/set/snap-enabled on toggle", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const snap = await screen.findByRole("checkbox", { name: "Snap to grid" });
    expect((snap as HTMLInputElement).checked).toBe(false); // mock default

    fireEvent.click(snap);
    await waitFor(async () => {
      const s = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(s.snapEnabled).toBe(true);
    });
    await waitFor(() => expect((snap as HTMLInputElement).checked).toBe(true));
  });

  it("a single-axis transform edit preserves the other axes + rotation", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByLabelText("Position X");
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

  const ENTRY = { name: "AT_AT_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle" };

  function makeBuildingBridge() {
    let listBuilding = true;
    let handler: ((e: { kind: string; payload: unknown }) => void) | null = null;
    const list = () =>
      listBuilding ? { objects: [], building: true } : { objects: [ENTRY], building: false };
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
    const { bridge, finishBuild } = makeBuildingBridge();
    render(<ReferenceObjectPicker bridge={bridge} onClose={() => {}} />);

    expect(await screen.findByText(/loading objects/i)).toBeInTheDocument();
    expect(screen.queryByRole("tree", { name: "Reference object" })).not.toBeInTheDocument();
    expect(
      (screen.getByLabelText("Search reference objects") as HTMLInputElement).disabled
    ).toBe(true);

    act(() => finishBuild());
    const tree = await screen.findByRole("tree", { name: "Reference object" });
    expect(within(tree).getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
    expect(screen.queryByText(/loading objects/i)).not.toBeInTheDocument();
    expect(
      (screen.getByLabelText("Search reference objects") as HTMLInputElement).disabled
    ).toBe(false);
  });

  it("does NOT re-query the list on unrelated state changes once loaded (no fetch storm)", async () => {
    let listCalls = 0;
    let handler: ((e: { kind: string; payload: unknown }) => void) | null = null;
    const bridge = {
      request: (req: { kind: string }) => {
        if (req.kind === "engine/query/reference-object-list") {
          listCalls++;
          return Promise.resolve({ objects: [ENTRY], building: false });
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
    await screen.findByRole("tree", { name: "Reference object" });
    const afterLoad = listCalls;

    act(() => {
      for (let i = 1; i <= 10; i++)
        handler?.({
          kind: "engine/state/changed",
          payload: { ...SNAP(false), referenceObjectPosition: [i, 0, 0] },
        });
    });
    await waitFor(() =>
      expect((screen.getByLabelText("Position X") as HTMLInputElement).value).toBe("10.0")
    );
    expect(listCalls).toBe(afterLoad);
  });

  it("a mod-switch rebuild (building flips true again) re-loads the list", async () => {
    const { bridge, finishBuild, startRebuild } = makeBuildingBridge();
    render(<ReferenceObjectPicker bridge={bridge} onClose={() => {}} />);
    act(() => finishBuild());
    await screen.findByRole("tree", { name: "Reference object" });

    act(() => startRebuild());
    expect(await screen.findByText(/loading objects/i)).toBeInTheDocument();
    expect(screen.queryByRole("tree", { name: "Reference object" })).not.toBeInTheDocument();

    act(() => finishBuild());
    const tree = await screen.findByRole("tree", { name: "Reference object" });
    expect(within(tree).getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
  });

  it("search narrows the listed objects (case-insensitive substring on Name)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument()
    );

    fireEvent.change(screen.getByLabelText("Search reference objects"), {
      target: { value: "at_" },
    });
    await waitFor(() =>
      expect(screen.queryByRole("treeitem", { name: "Star_Destroyer" })).not.toBeInTheDocument()
    );
    expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: "AT_ST_Walker" })).toBeInTheDocument();
  });
});
