// Vitest tests for MenuBar.
//
// Coverage:
//   1. File → New on a dirty system opens the SaveChangesPrompt
//      (assert prompt presence in the DOM after the click).
//   2. Recent Files submenu renders entries from the file-state atom
//      (recentFiles array of paths).
//
// Radix-in-jsdom caveat: hovering / sub-menu opening is brittle
// in jsdom because Radix relies on pointer events. The recent-files
// spec verifies the basename-formatting helper on the data path —
// asserting on the rendered submenu DOM nodes after clicking the File
// trigger + SubTrigger.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { MenuBar } from "../MenuBar";
import { useFileStateStore } from "@/lib/file-state";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { useTreeActionStore } from "@/lib/tree-action";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeStubBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  return {
    request: vi.fn().mockResolvedValue({}),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  useFileStateStore.setState({
    currentFilePath: null,
    dirty: false,
    recentFiles: [],
    pendingAction: null,
  });
});

function renderMenuBar(
  bridge: Bridge,
  overrides: Partial<{ onResetPanelLayout: () => void }> = {},
) {
  return render(
    <MenuBar
      bridge={bridge}
      onOpenImportEmittersDialog={() => {}}
      onOpenAboutDialog={() => {}}
      onOpenRescaleDialog={() => {}}
      onResetPanelLayout={overrides.onResetPanelLayout ?? (() => {})}
    />,
  );
}

describe("MenuBar — File menu", () => {
  it("File → New on a dirty system stores the pending action (opens SaveChangesPrompt)", async () => {
    useFileStateStore.getState().setDirty(true);
    const bridge = makeStubBridge();
    renderMenuBar(bridge);

    // Open the File menu via the trigger button.
    const fileTrigger = screen.getByRole("menuitem", { name: "File" });
    fireEvent.pointerDown(fileTrigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(fileTrigger);

    // The New item is now in the portal. Click it.
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /New/ })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole("menuitem", { name: /New/ }));

    // promptSaveChanges should have stored the pending action because
    // the atom was dirty. file/new is NOT dispatched directly — the
    // prompt is now responsible for that.
    await waitFor(() => {
      expect(useFileStateStore.getState().pendingAction).not.toBeNull();
    });
    expect(bridge.request).not.toHaveBeenCalledWith({
      kind: "file/new",
      params: {},
    });
  });

  it("File → New on a clean system dispatches file/new without a prompt", async () => {
    // Clean by default.
    const bridge = makeStubBridge();
    renderMenuBar(bridge);

    const fileTrigger = screen.getByRole("menuitem", { name: "File" });
    fireEvent.pointerDown(fileTrigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(fileTrigger);

    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /New/ })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole("menuitem", { name: /New/ }));

    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "file/new",
        params: {},
      });
    });
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });
});

// ─── Top-level menu restructure ──────────

describe("MenuBar — top-level structure", () => {
  beforeEach(() => {
    useEmitterSelectionStore.setState({ ids: [], primary: null });
    useTreeActionStore.setState({ renameRequest: null });
  });

  it("renders top-level triggers in the order [File, Edit, Emitters, Mods, View, Help]", () => {
    const bridge = makeStubBridge();
    const { container } = renderMenuBar(bridge);
    // Radix renders each Menubar.Trigger as a direct child <button>
    // of the [role="menubar"] root.
    const root = container.querySelector('[role="menubar"]');
    expect(root).not.toBeNull();
    const triggers = Array.from(
      root!.querySelectorAll(':scope > button'),
    ).map((b) => b.textContent?.trim());
    expect(triggers).toEqual(["File", "Edit", "Emitters", "Mods", "View", "Help"]);
  });

  it("does NOT render a `Tools` top-level trigger", () => {
    const bridge = makeStubBridge();
    const { container } = renderMenuBar(bridge);
    const root = container.querySelector('[role="menubar"]');
    const triggers = Array.from(
      root!.querySelectorAll(':scope > button'),
    ).map((b) => b.textContent?.trim());
    expect(triggers).not.toContain("Tools");
  });

  it("Emitters menu exposes New Emitter / Rename Emitter / Rescale Emitter… / Spawner…", async () => {
    const bridge = makeStubBridge();
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "Emitters" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /New Emitter/ })).toBeTruthy();
    });
    expect(screen.getByRole("menuitem", { name: /Rename Emitter/ })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: /Rescale Emitter/ })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: /Spawner/ })).toBeTruthy();
    // Toggle Visibility / Show All / Hide All rendered but disabled.
    const toggleVis = screen.getByRole("menuitem", { name: /Toggle Visibility/ });
    expect(toggleVis.getAttribute("data-disabled")).not.toBeNull();
  });

  it("View menu exposes Lighting… and no longer has any Bloom entries (folded into Lighting + toolbar)", async () => {
    const bridge = makeStubBridge();
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "View" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /Lighting/ })).toBeTruthy();
    });
    // Both View-menu Bloom entries were retired: "Bloom Settings…" moved
    // into the Lighting pane's Bloom section, and the on/off "Bloom" toggle
    // is the toolbar's "Toggle bloom" button. No Bloom menuitem remains.
    expect(screen.queryByRole("menuitem", { name: /Bloom/ })).toBeNull();
  });

  it("View > Reset panel layout fires the onResetPanelLayout callback", async () => {
    const bridge = makeStubBridge();
    const onResetPanelLayout = vi.fn();
    renderMenuBar(bridge, { onResetPanelLayout });
    const trigger = screen.getByRole("menuitem", { name: "View" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(
        screen.getByRole("menuitem", { name: /Reset panel layout/ }),
      ).toBeTruthy();
    });
    fireEvent.click(
      screen.getByRole("menuitem", { name: /Reset panel layout/ }),
    );
    await waitFor(() => {
      expect(onResetPanelLayout).toHaveBeenCalledTimes(1);
    });
  });

  it("Emitters > Rename Emitter writes the primary id into the tree-action atom", async () => {
    useEmitterSelectionStore.setState({ ids: [7], primary: 7 });
    const bridge = makeStubBridge();
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "Emitters" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /Rename Emitter/ })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole("menuitem", { name: /Rename Emitter/ }));
    await waitFor(() => {
      expect(useTreeActionStore.getState().renameRequest).toBe(7);
    });
  });

  it("Emitters > New Emitter > Root Emitter dispatches emitters/add-root", async () => {
    const bridge = makeStubBridge();
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "Emitters" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    // Open the New Emitter submenu.
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: /New Emitter/ })).toBeTruthy();
    });
    const newEmitter = screen.getByRole("menuitem", { name: /New Emitter/ });
    fireEvent.pointerDown(newEmitter, { button: 0, pointerType: "mouse" });
    fireEvent.click(newEmitter);
    // Wait for the Root Emitter item to appear in the submenu portal.
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: "Root Emitter" })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole("menuitem", { name: "Root Emitter" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "emitters/add-root",
        params: {},
      });
    });
  });
});

describe("MenuBar — Mods menu (layer stacking)", () => {
  const fixtureMods = [
    { path: "C:/test/corruption/Mods/Alpha", folderName: "Alpha", nickname: "", isFoC: true, rootHasArt: true },
    { path: "C:/test/GameData/Mods/Beta",    folderName: "Beta",  nickname: "Beta Mod", isFoC: false, rootHasArt: true },
  ];
  const fixtureLayers = [
    { path: "C:/test/corruption/Mods/Alpha",          label: "Alpha",    isFoC: true,  kind: "mod" as const },
    { path: "C:/test/corruption/Mods/Alpha/Mod",     label: "Mod",     parentLabel: "Alpha", parentPath: "C:/test/corruption/Mods/Alpha", isFoC: true, kind: "nested" as const },
    { path: "C:/test/corruption/Mods/Alpha/Core", label: "Core", parentLabel: "Alpha", parentPath: "C:/test/corruption/Mods/Alpha", isFoC: true, kind: "nested" as const },
    { path: "C:/test/GameData/Mods/Beta",             label: "Beta Mod", isFoC: false, kind: "mod" as const },
  ];
  function makeModsStubBridge(opts: { stack?: string[] } = {}): Bridge & { request: ReturnType<typeof vi.fn> } {
    const stack = opts.stack ?? [];
    const snapshot = { activeModPath: stack[0] ?? null, ground: false, bloom: false, paused: false };
    const modsListResp = { mods: fixtureMods, layers: fixtureLayers, stack, activePath: stack[0] ?? null };
    const request = vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "mods/list" || req.kind === "mods/refresh") return Promise.resolve(modsListResp);
      if (req.kind === "mods/set-layers") return Promise.resolve({ ok: true, stack: [] });
      return Promise.resolve({});
    });
    return { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  }
  async function openModsMenu() {
    const trigger = screen.getByRole("menuitem", { name: "Mods" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "Refresh Mod List" })).toBeTruthy(); });
  }

  // Open the Add mod… flyout (catalog + search lives there now).
  async function openAddMod() {
    const addMod = screen.getByRole("menuitem", { name: /Add mod/ });
    fireEvent.pointerDown(addMod, { button: 0, pointerType: "mouse" });
    fireEvent.click(addMod);
  }

  it("renders the active-stack block, Add mod…, Expand, Reset, Refresh", async () => {
    renderMenuBar(makeModsStubBridge());
    await openModsMenu();
    expect(screen.getByRole("menuitem", { name: /Add mod/ })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Reset" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Refresh Mod List" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Expand to full editor" })).toBeTruthy();
    expect(screen.getByText(/Unmodded — base game only/)).toBeTruthy(); // empty-stack copy
  });

  // The catalog lives in the Add mod… flyout now. Adding a no-nested mod from an
  // empty stack dispatches set-layers with that one path.
  it("Add mod… adds a no-nested mod to the stack", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "Beta Mod" })).toBeTruthy(); });
    fireEvent.click(screen.getByRole("menuitem", { name: "Beta Mod" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: ["C:/test/GameData/Mods/Beta"] } });
    });
  });

  // The stack composes — adding APPENDS, never replaces.
  it("Add mod… appends to an existing stack instead of replacing it", async () => {
    const bridge = makeModsStubBridge({ stack: ["C:/test/corruption/Mods/Alpha"] });
    renderMenuBar(bridge);
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "Beta Mod" })).toBeTruthy(); });
    fireEvent.click(screen.getByRole("menuitem", { name: "Beta Mod" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-layers",
        params: { paths: ["C:/test/corruption/Mods/Alpha", "C:/test/GameData/Mods/Beta"] },
      });
    });
  });

  // A nested mod lists its root-art layer + each nested layer as add items.
  it("Add mod… lists a nested mod's root-art + nested layers; adding a nested layer dispatches", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "Mod" })).toBeTruthy(); });
    expect(screen.getByRole("menuitem", { name: "Core" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Alpha" })).toBeTruthy(); // root-art layer (rootHasArt:true)
    fireEvent.click(screen.getByRole("menuitem", { name: "Mod" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: ["C:/test/corruption/Mods/Alpha/Mod"] } });
    });
  });

  // An in-stack layer reads "✓ in stack" (not an actionable add item).
  it("Add mod… shows in-stack layers as non-actionable '✓ in stack'", async () => {
    renderMenuBar(makeModsStubBridge({ stack: ["C:/test/GameData/Mods/Beta"] }));
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByText(/in stack/)).toBeTruthy(); });
    expect(screen.queryByRole("menuitem", { name: "Beta Mod" })).toBeNull(); // not an add item
  });

  it("Reset dispatches mods/set-layers with []", async () => {
    const bridge = makeModsStubBridge({ stack: ["C:/test/corruption/Mods/Alpha"] });
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Reset" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: [] } });
    });
  });

  // The active-stack block (role=list "Active load order") shows the ordered layers.
  it("renders the active load-order list with each layer", async () => {
    renderMenuBar(makeModsStubBridge({ stack: ["C:/test/corruption/Mods/Alpha/Mod", "C:/test/corruption/Mods/Alpha"] }));
    await openModsMenu();
    const list = screen.getByRole("list", { name: "Active load order" });
    expect(list.textContent).toContain("Mod");
    expect(list.textContent).toContain("Alpha");
  });

  // Each active-stack row has a × that removes that layer (dispatches without it).
  it("removing a layer from the active stack dispatches set-layers without it", async () => {
    const bridge = makeModsStubBridge({ stack: ["C:/test/corruption/Mods/Alpha/Mod", "C:/test/corruption/Mods/Alpha"] });
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("button", { name: /Remove Mod from stack/ }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: ["C:/test/corruption/Mods/Alpha"] } });
    });
  });

  it("Refresh dispatches mods/refresh", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Refresh Mod List" }));
    await waitFor(() => { expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/refresh", params: {} }); });
  });

  // Expand → opens the full Load Order modal (the demoted fallback + keyboard/AT path).
  it("Expand to full editor opens the Load Order dialog", async () => {
    renderMenuBar(makeModsStubBridge({ stack: ["C:/test/corruption/Mods/Alpha"] }));
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Expand to full editor" }));
    await waitFor(() => { expect(screen.getByRole("list", { name: "Load order" })).toBeTruthy(); });
  });

  // rootHasArt:false → no root-art add item (only the nested layers are addable).
  it("Add mod…: a rootHasArt:false mod shows only its nested layers (no root-art item)", async () => {
    const layers = [
      { path: "C:/test/corruption/Mods/Gamma/Skin", label: "Skin", parentLabel: "Gamma", parentPath: "C:/test/corruption/Mods/Gamma", isFoC: true, kind: "nested" as const },
    ];
    const snapshot = { activeModPath: null, ground: false, bloom: false, paused: false };
    const request = vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "mods/list" || req.kind === "mods/refresh") return Promise.resolve({ mods: [], layers, stack: [], activePath: null });
      return Promise.resolve({});
    });
    const bridge = { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge;
    renderMenuBar(bridge);
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "Skin" })).toBeTruthy(); });
    expect(screen.queryByRole("menuitem", { name: "Gamma" })).toBeNull(); // no root-art layer
  });

  // Duplicate-label mods keep their own nested layers (matched by parentPath) — both
  // appear in the flyout under their own parent group, no cross-pollination.
  it("Add mod…: duplicate-label mods keep their own nested layers (parentPath)", async () => {
    const layers = [
      { path: "C:/test/corruption/Mods/Dup/FocLayer",  label: "FocLayer",  parentLabel: "Dup", parentPath: "C:/test/corruption/Mods/Dup", isFoC: true,  kind: "nested" as const },
      { path: "C:/test/GameData/Mods/Dup/BaseLayer",   label: "BaseLayer", parentLabel: "Dup", parentPath: "C:/test/GameData/Mods/Dup",   isFoC: false, kind: "nested" as const },
    ];
    const snapshot = { activeModPath: null, ground: false, bloom: false, paused: false };
    const request = vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "mods/list" || req.kind === "mods/refresh") return Promise.resolve({ mods: [], layers, stack: [], activePath: null });
      return Promise.resolve({});
    });
    const bridge = { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
    renderMenuBar(bridge);
    await openModsMenu();
    await openAddMod();
    await waitFor(() => { expect(screen.getByRole("menuitem", { name: "FocLayer" })).toBeTruthy(); });
    expect(screen.getByRole("menuitem", { name: "BaseLayer" })).toBeTruthy(); // both present, separate parent groups
    fireEvent.click(screen.getByRole("menuitem", { name: "FocLayer" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: ["C:/test/corruption/Mods/Dup/FocLayer"] } });
    });
  });

  it("a partial mods/list (missing layers/stack) does not crash the menu", async () => {
    const bridge = { request: vi.fn().mockImplementation((r: { kind: string }) =>
      r.kind === "engine/state/snapshot" ? Promise.resolve({ activeModPath: null })
      : r.kind === "mods/list" || r.kind === "mods/refresh" ? Promise.resolve({ mods: fixtureMods })
      : Promise.resolve({})), on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge;
    renderMenuBar(bridge);
    await openModsMenu();
    expect(screen.getByRole("menuitem", { name: "Refresh Mod List" })).toBeTruthy();
  });
});

describe("MenuBar — View menu Grid item", () => {
  function makeViewStubBridge(
    gridVisible: boolean,
  ): Bridge & { request: ReturnType<typeof vi.fn> } {
    const snapshot = { ground: false, gridVisible, bloom: false, paused: false };
    const request = vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      return Promise.resolve({});
    });
    return {
      request,
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  }

  it("View menu shows a Grid item", async () => {
    const bridge = makeViewStubBridge(false);
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "View" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: "Grid" })).toBeTruthy();
    });
  });

  it("View > Grid dispatches engine/set/grid-visible with the inverted value", async () => {
    const bridge = makeViewStubBridge(false);
    renderMenuBar(bridge);
    const trigger = screen.getByRole("menuitem", { name: "View" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: "Grid" })).toBeTruthy();
    });
    fireEvent.click(screen.getByRole("menuitem", { name: "Grid" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "engine/set/grid-visible",
        params: { visible: true },
      });
    });
  });
});
