// Vitest unit tests for ReferenceObjectPicker. Driven by the real
// MockBridge so the canned reference-object-list + the set handlers (including
// the skinned-status path) are exercised end-to-end against the schema.
//
// Covered: object enumeration into the collapsible Heroes / Ground / Space tree,
// selection dispatch (click a tree item), section + bucket collapse/expand, the
// "skinned — not supported" + "model-missing" status notes, the visibility
// toggle, a numeric transform commit, the "Loading objects…" building
// state, and the search filter (which force-expands so matches stay visible).

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, act, within } from "@testing-library/react";
import { ReferenceObjectPickerBody } from "../ReferenceObjectPicker";
import { MockBridge } from "@/bridge/mock";
import { useMockEngineState, makeDefaultEngineState } from "@/bridge/mock-state";
import { __resetPickerStateCacheForTests } from "@/lib/picker-state";
import type { Bridge } from "@particle-editor/bridge-schema";

// The MockBridge reads/writes the module-level Zustand engine store, which is
// shared across every MockBridge instance in this file. Reset it to defaults
// before each test so state (selected object, lock flag) can't leak between
// tests — mirrors BackgroundPicker.test.tsx. Without this, the lock tests that
// assert on the pristine "no object selected" / "unlocked" defaults pick up the
// previous test's selection + lock state and fail.
beforeEach(() => {
  useMockEngineState.setState(makeDefaultEngineState());
});

describe("ReferenceObjectPicker — selection + status", () => {
  it("enumerates objects into Heroes/Ground/Space sections and dispatches on click-select", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);

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

  it("tags each tree leaf with a ref-unit:<name> testid for clip-cursor targeting", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument(),
    );
    // The leaf row (the click/select target) carries a positional-by-name testid so a
    // --record cursor can land on a specific unit.
    const leaf = screen.getByRole("treeitem", { name: "AT_AT_Walker" });
    expect(leaf.getAttribute("data-testid")).toBe("ref-unit:AT_AT_Walker");
    expect(document.querySelector('[data-testid="ref-unit:AT_AT_Walker"]')).toBe(leaf);
  });

  it("filters the tree when a ui/set-picker-search message arrives (clip-driven search)", async () => {
    // The synthetic --record cursor can't type, so the host drives the search box via a
    // ui/set-picker-search webview push. Mock window.chrome.webview to deliver it.
    const listeners: Record<string, Array<(e: { data: unknown }) => void>> = {};
    (window as unknown as { chrome?: unknown }).chrome = {
      webview: {
        addEventListener: (e: string, h: (ev: { data: unknown }) => void) => { (listeners[e] ||= []).push(h); },
        removeEventListener: (e: string, h: (ev: { data: unknown }) => void) => {
          listeners[e] = (listeners[e] ?? []).filter((x) => x !== h);
        },
      },
    };
    try {
      const bridge = new MockBridge();
      render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
      await screen.findByRole("tree", { name: "Reference object" });
      await waitFor(() => expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument());
      expect(screen.getByRole("treeitem", { name: "AT_ST_Walker" })).toBeInTheDocument();

      act(() => {
        const msg = { data: JSON.stringify({ type: "ui/set-picker-search", text: "AT_AT" }) };
        (listeners.message ?? []).forEach((h) => h(msg));
      });

      await waitFor(() =>
        expect(screen.queryByRole("treeitem", { name: "AT_ST_Walker" })).not.toBeInTheDocument(),
      );
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
    } finally {
      delete (window as unknown as { chrome?: unknown }).chrome;
    }
  });

  it("collapses a section to hide its items, and expands it again", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
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

  it("force-collapses a section under search via ui/picker-collapse (clip driver)", async () => {
    // The whole point of ui/picker-collapse: a clip can keep a section closed even
    // though a search force-expands every section. (Drives the Faith clip's Heroes
    // collapse; here we use Space since the mock has it.)
    const listeners: Record<string, Array<(e: { data: unknown }) => void>> = {};
    (window as unknown as { chrome?: unknown }).chrome = {
      webview: {
        addEventListener: (e: string, h: (ev: { data: unknown }) => void) => { (listeners[e] ||= []).push(h); },
        removeEventListener: (e: string, h: (ev: { data: unknown }) => void) => {
          listeners[e] = (listeners[e] ?? []).filter((x) => x !== h);
        },
      },
    };
    const send = (payload: unknown) =>
      act(() => {
        (listeners.message ?? []).forEach((h) => h({ data: JSON.stringify(payload) }));
      });
    try {
      const bridge = new MockBridge();
      render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
      await screen.findByRole("tree", { name: "Reference object" });
      await waitFor(() => expect(screen.getByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument());

      // A search force-expands every section, so a plain collapse would be ignored.
      send({ type: "ui/set-picker-search", text: "_" }); // matches every name (all have "_")
      await waitFor(() => expect(screen.getByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument());

      // The host force-collapses Space (bare section name) -> its items hide DESPITE the search.
      send({ type: "ui/picker-collapse", keys: ["Space"] });
      await waitFor(() =>
        expect(screen.queryByRole("treeitem", { name: "Star_Destroyer" })).not.toBeInTheDocument(),
      );
      expect(screen.getByRole("treeitem", { name: /Space/ })).toHaveAttribute("aria-expanded", "false");
      // A non-collapsed section's items stay visible.
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
    } finally {
      delete (window as unknown as { chrome?: unknown }).chrome;
    }
  });

  it("lists an unrecognised-tag unit under Ground ▸ Other (the missing-units fix)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    // bucket "Other" header + the item both render (Imperial_Bunker_Capturable is Ground/Unit/Other).
    expect(await screen.findByRole("treeitem", { name: /Other/ })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: "Imperial_Bunker_Capturable" })).toBeInTheDocument();
  });

  it("shows the skinned-unsupported note for a skinned object", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
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
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
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
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);

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
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "Mod_Only_Object" } });
    // The picker shows it as a standalone selected row, not snapping back to None.
    const opt = await screen.findByRole("treeitem", { name: "Mod_Only_Object" });
    expect(opt).toHaveAttribute("aria-selected", "true");
  });

  it("reflects snapEnabled and dispatches engine/set/snap-enabled on toggle", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);

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
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
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

  const ENTRY = { name: "AT_AT_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle", affiliation: "Empire" };

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
    render(<ReferenceObjectPickerBody bridge={bridge} />);

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
    render(<ReferenceObjectPickerBody bridge={bridge} />);
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
    render(<ReferenceObjectPickerBody bridge={bridge} />);
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
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
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

// Freeze/lock: the Lock checkbox reflects + dispatches referenceObjectLocked,
// and a locked object disables the Transform inputs (Reset + spinners).
describe("ReferenceObjectPicker — lock", () => {
  it("reflects referenceObjectLocked and dispatches engine/set/reference-object-lock on toggle", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument()
    );
    fireEvent.click(screen.getByRole("treeitem", { name: "AT_AT_Walker" }));

    const lock = await screen.findByRole("checkbox", { name: "Lock object" });
    expect((lock as HTMLInputElement).checked).toBe(false); // mock default
    fireEvent.click(lock);
    await waitFor(async () => {
      const s = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(s.referenceObjectLocked).toBe(true);
    });
    await waitFor(() => expect((lock as HTMLInputElement).checked).toBe(true));
  });

  it("disables the Transform spinners + Reset when locked", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument()
    );
    fireEvent.click(screen.getByRole("treeitem", { name: "AT_AT_Walker" }));

    // Enabled before locking.
    expect(((await screen.findByLabelText("Position X")) as HTMLInputElement).disabled).toBe(false);
    // Lock -> round-trips -> inputs disable.
    fireEvent.click(await screen.findByRole("checkbox", { name: "Lock object" }));
    await waitFor(() =>
      expect((screen.getByLabelText("Position X") as HTMLInputElement).disabled).toBe(true)
    );
    expect((screen.getByLabelText("Position Y") as HTMLInputElement).disabled).toBe(true);
    expect((screen.getByLabelText("Position Z") as HTMLInputElement).disabled).toBe(true);
    expect((screen.getByLabelText("Rotation Yaw") as HTMLInputElement).disabled).toBe(true);
    expect(
      (screen.getByRole("button", { name: "Reset transform to origin" }) as HTMLButtonElement).disabled
    ).toBe(true);
  });

  it("disables the Lock checkbox when no object is selected", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    // Default mock has no reference object selected (name === NONE) -> Lock disabled.
    const lock = await screen.findByRole("checkbox", { name: "Lock object" });
    expect((lock as HTMLInputElement).disabled).toBe(true);
  });

  // Unlocking re-enables the inputs — the symmetric return path (a stuck
  // disabled={locked} would regress this without the lock->unlock round-trip).
  it("re-enables the Transform inputs after unlock", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument()
    );
    fireEvent.click(screen.getByRole("treeitem", { name: "AT_AT_Walker" }));

    const lock = await screen.findByRole("checkbox", { name: "Lock object" });
    fireEvent.click(lock); // lock
    await waitFor(() =>
      expect((screen.getByLabelText("Position X") as HTMLInputElement).disabled).toBe(true)
    );
    fireEvent.click(lock); // unlock
    await waitFor(() =>
      expect((screen.getByLabelText("Position X") as HTMLInputElement).disabled).toBe(false)
    );
    expect((screen.getByLabelText("Rotation Yaw") as HTMLInputElement).disabled).toBe(false);
    expect(
      (screen.getByRole("button", { name: "Reset transform to origin" }) as HTMLButtonElement).disabled
    ).toBe(false);
  });

  // The freeze holds at the (mock) bridge: a transform request against a locked
  // object is dropped, mirroring the native BridgeDispatcher no-op. Guards the
  // mock from silently encoding the wrong contract.
  it("drops a transform request while locked (bridge-level freeze)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByLabelText("Position X");
    // Seed a known transform, lock, then attempt to move it via the bridge.
    await bridge.request({
      kind: "engine/set/reference-object-transform",
      params: { position: [5, 6, 7], rotation: [0, 0, 0] },
    });
    await bridge.request({ kind: "engine/set/reference-object-lock", params: { locked: true } });
    await bridge.request({
      kind: "engine/set/reference-object-transform",
      params: { position: [99, 99, 99], rotation: [45, 0, 0] },
    });
    const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
    expect(snap.referenceObjectPosition).toEqual([5, 6, 7]);   // unchanged — frozen
    expect(snap.referenceObjectRotation).toEqual([0, 0, 0]);
  });
});

describe("ReferenceObjectPicker — keyboard navigation + persisted-selection expand", () => {
  const ready = async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument());
    return bridge;
  };
  const tabbable = () =>
    screen.getAllByRole("treeitem").filter((el) => el.getAttribute("tabindex") === "0");

  it("keeps exactly one tree row tabbable (roving tabindex), starting at None", async () => {
    await ready();
    const t = tabbable();
    expect(t).toHaveLength(1);
    expect(t[0]).toHaveAttribute("aria-selected", "true"); // None is selected by default
    expect(t[0].textContent).toBe("None");
  });

  it("ArrowDown moves the active row to the next visible row", async () => {
    await ready();
    const none = screen.getByRole("treeitem", { name: "None" });
    none.focus();
    fireEvent.keyDown(none, { key: "ArrowDown" });
    // First section after None is Heroes.
    await waitFor(() => expect(screen.getByRole("treeitem", { name: "Heroes" })).toHaveAttribute("tabindex", "0"));
    expect(none).toHaveAttribute("tabindex", "-1");
    expect(tabbable()).toHaveLength(1);
  });

  it("ArrowLeft collapses a focused open section; ArrowRight re-expands it", async () => {
    await ready();
    const space = screen.getByRole("treeitem", { name: "Space" });
    space.focus();
    expect(space).toHaveAttribute("aria-expanded", "true");
    fireEvent.keyDown(space, { key: "ArrowLeft" });
    await waitFor(() =>
      expect(screen.getByRole("treeitem", { name: "Space" })).toHaveAttribute("aria-expanded", "false")
    );
    expect(screen.queryByRole("treeitem", { name: "Star_Destroyer" })).not.toBeInTheDocument();
    fireEvent.keyDown(screen.getByRole("treeitem", { name: "Space" }), { key: "ArrowRight" });
    expect(await screen.findByRole("treeitem", { name: "Star_Destroyer" })).toBeInTheDocument();
  });

  it("Enter on a focused leaf selects it", async () => {
    const bridge = await ready();
    const item = screen.getByRole("treeitem", { name: "AT_AT_Walker" });
    fireEvent.keyDown(item, { key: "Enter" });
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectName).toBe("AT_AT_Walker");
    });
  });

  it("auto-expands the ancestor groups of a selection that lands in a collapsed group", async () => {
    const bridge = await ready();
    // User collapses Ground (hiding AT_AT_Walker under Ground ▸ Vehicles).
    fireEvent.click(screen.getByRole("treeitem", { name: "Ground" }));
    await waitFor(() => expect(screen.queryByRole("treeitem", { name: "AT_AT_Walker" })).not.toBeInTheDocument());
    // A selection lands inside the collapsed group (e.g. persisted from a prior session).
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "AT_AT_Walker" } });
    // Its ancestors re-open so it's visible + selected.
    const item = await screen.findByRole("treeitem", { name: "AT_AT_Walker" });
    expect(item).toHaveAttribute("aria-selected", "true");
  });
});

describe("ReferenceObjectPicker — faction filter chips", () => {
  const ready = async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument());
    return bridge;
  };

  it("renders All + faction chips and narrows the tree to the chosen faction", async () => {
    await ready();
    expect(screen.getByRole("button", { name: "All" })).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByRole("button", { name: "Empire" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Rebel" })).toBeInTheDocument();
    // Default (All): both an Empire-only and a Rebel object are visible.
    expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();   // Empire
    expect(screen.getByRole("treeitem", { name: "Rebel_Barracks" })).toBeInTheDocument(); // Rebel

    fireEvent.click(screen.getByRole("button", { name: "Rebel" }));
    await waitFor(() =>
      expect(screen.queryByRole("treeitem", { name: "AT_AT_Walker" })).not.toBeInTheDocument()
    );
    expect(screen.getByRole("button", { name: "Rebel" })).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByRole("treeitem", { name: "Rebel_Barracks" })).toBeInTheDocument();
    // A multi-faction object ("Rebel, Empire") shows under Rebel too.
    expect(screen.getByRole("treeitem", { name: "Nebulon_B_Frigate" })).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "All" }));
    expect(await screen.findByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
  });

  it("shows a multi-faction object under each of its factions", async () => {
    await ready();
    fireEvent.click(screen.getByRole("button", { name: "Empire" }));
    // Nebulon_B_Frigate is "Rebel, Empire" -> visible under Empire as well.
    expect(await screen.findByRole("treeitem", { name: "Nebulon_B_Frigate" })).toBeInTheDocument();
    // A Rebel-only object is hidden under the Empire filter.
    expect(screen.queryByRole("treeitem", { name: "Rebel_Barracks" })).not.toBeInTheDocument();
  });
});

describe("ReferenceObjectPicker — props + templates", () => {
  const ready = async () => {
    // The faction-filter describe above persists a faction into picker-state
    // (localStorage + in-memory cache); clear both so these props tests start at
    // the "All" filter, else props with no / other affiliation are filtered out.
    localStorage.removeItem("alo:refpicker:v1");
    __resetPickerStateCacheForTests();
    const bridge = new MockBridge();
    render(<ReferenceObjectPickerBody bridge={bridge as unknown as Bridge} />);
    await screen.findByRole("tree", { name: "Reference object" });
    await waitFor(() => expect(screen.getByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument());
    return bridge;
  };

  it("renders Props + Templates as their own flat top-level sections", async () => {
    const bridge = await ready();
    const tree = screen.getByRole("tree", { name: "Reference object" });
    // Sections present.
    expect(screen.getByRole("treeitem", { name: /Props/ })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: /Templates/ })).toBeInTheDocument();
    // Prop + template leaves land directly under their section (no bucket sub-header),
    // and select like any other object.
    const prop = within(tree).getByRole("treeitem", { name: "Asteroid_Field_Prop" });
    expect(prop).toBeInTheDocument();
    expect(within(tree).getByRole("treeitem", { name: "Generic_Frigate_Template" })).toBeInTheDocument();

    fireEvent.click(prop);
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectName).toBe("Asteroid_Field_Prop");
    });
  });

  it("the 'Props only' chip filters the tree to props (hides units, heroes, templates)", async () => {
    await ready();
    const chip = screen.getByRole("button", { name: "Props only" });
    expect(chip).toHaveAttribute("aria-pressed", "false");

    fireEvent.click(chip);
    await waitFor(() => expect(chip).toHaveAttribute("aria-pressed", "true"));
    // Only props remain.
    expect(screen.getByRole("treeitem", { name: "Asteroid_Field_Prop" })).toBeInTheDocument();
    expect(screen.getByRole("treeitem", { name: "Rebel_Crate_Prop" })).toBeInTheDocument();
    expect(screen.queryByRole("treeitem", { name: "AT_AT_Walker" })).not.toBeInTheDocument();          // unit
    expect(screen.queryByRole("treeitem", { name: "Darth_Vader" })).not.toBeInTheDocument();           // hero
    expect(screen.queryByRole("treeitem", { name: "Generic_Frigate_Template" })).not.toBeInTheDocument(); // template

    // Toggling off restores the full list.
    fireEvent.click(chip);
    expect(await screen.findByRole("treeitem", { name: "AT_AT_Walker" })).toBeInTheDocument();
  });

  it("ANDs 'Props only' with the faction filter", async () => {
    await ready();
    fireEvent.click(screen.getByRole("button", { name: "Props only" }));
    fireEvent.click(screen.getByRole("button", { name: "Rebel" }));
    // Rebel_Crate_Prop is the only Rebel-affiliated prop; the no-affiliation
    // Asteroid_Field_Prop drops out under a faction filter.
    expect(await screen.findByRole("treeitem", { name: "Rebel_Crate_Prop" })).toBeInTheDocument();
    expect(screen.queryByRole("treeitem", { name: "Asteroid_Field_Prop" })).not.toBeInTheDocument();
  });
});
