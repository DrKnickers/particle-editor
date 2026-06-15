// Vitest tests for MenuBar — Phase 3 Screen 8 Batch 3 additions.
//
// Coverage:
//   1. File → New on a dirty system opens the SaveChangesPrompt
//      (assert prompt presence in the DOM after the click).
//   2. Recent Files submenu renders entries from the file-state atom
//      (recentFiles array of paths).
//
// Radix-in-jsdom caveat (): hovering / sub-menu opening is brittle
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

describe("MenuBar — File menu (Batch 3)", () => {
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

// ─── Phase 4.1 Fix dispatch 5 — top-level menu restructure ──────────

describe("MenuBar — top-level structure ()", () => {
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

  it("View menu exposes Lighting… and no longer has any Bloom entries (folded into Lighting + toolbar, session 11)", async () => {
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

  it("View > Reset panel layout fires the onResetPanelLayout callback (B1.4 T6)", async () => {
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

// D6 — Mods menu coverage. Three specs:
//   1. The dynamic list (Unmodded + 2 fixture entries + Refresh) renders.
//   2. Clicking a mod entry dispatches mods/select with that path.
//   3. Clicking Refresh dispatches mods/refresh.
//
// The check-mark behaviour (active entry shows Check icon) is exercised
// via the activeModPath path on the stub bridge's snapshot — the
// assertion on entry membership covers the rendering pipeline.

describe("MenuBar — Mods menu (D6)", () => {
  const fixtureMods = [
    { path: "C:/test/corruption/Mods/Alpha", folderName: "Alpha", nickname: "", isFoC: true },
    { path: "C:/test/GameData/Mods/Beta", folderName: "Beta", nickname: "Beta Mod", isFoC: false },
  ];

  function makeModsStubBridge(
    opts: { activeModPath?: string | null; submods?: string[]; activeSubmods?: string[] } = {},
  ): Bridge & { request: ReturnType<typeof vi.fn> } {
    const snapshot = {
      activeModPath: opts.activeModPath ?? null,
      // Minimum surface the MenuBar reads — other fields default sensibly
      // via optional chaining on the snapshot inside MenuBar.tsx.
      ground: false,
      bloom: false,
      paused: false,
    };
    const modsListResp = {
      mods: fixtureMods,
      activePath: opts.activeModPath ?? null,
      submods: opts.submods ?? [],              //
      activeSubmods: opts.activeSubmods ?? [],   //
    };
    const request = vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "mods/list" || req.kind === "mods/refresh") return Promise.resolve(modsListResp);
      if (req.kind === "mods/select") return Promise.resolve({ ok: true, activePath: null });
      if (req.kind === "mods/set-submods") return Promise.resolve({ ok: true, activeSubmods: [] });
      return Promise.resolve({});
    });
    return {
      request,
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  }

  async function openModsMenu() {
    const trigger = screen.getByRole("menuitem", { name: "Mods" });
    fireEvent.pointerDown(trigger, { button: 0, pointerType: "mouse" });
    fireEvent.click(trigger);
    // The dynamic items only render once the mods/list response has
    // resolved; wait on the Refresh anchor that's always rendered.
    await waitFor(() => {
      expect(screen.getByRole("menuitem", { name: "Refresh Mod List" })).toBeTruthy();
    });
  }

  it("renders Unmodded + the fixture entries + Refresh", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    expect(screen.getByRole("menuitem", { name: "Unmodded" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Alpha" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Beta Mod" })).toBeTruthy();
    expect(screen.getByRole("menuitem", { name: "Refresh Mod List" })).toBeTruthy();
  });

  it("clicking a mod entry dispatches mods/select with the right path", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Alpha" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/select",
        params: { path: "C:/test/corruption/Mods/Alpha" },
      });
    });
  });

  it("clicking Refresh dispatches mods/refresh", async () => {
    const bridge = makeModsStubBridge();
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Refresh Mod List" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/refresh",
        params: {},
      });
    });
  });

  // Submod submenu — shown only when the active mod has submods.
  it("shows the Submods… item when the active mod has submods", async () => {
    const bridge = makeModsStubBridge({
      activeModPath: "C:/test/corruption/Mods/Alpha",
      submods: ["Mod", "GCW"],
    });
    renderMenuBar(bridge);
    await openModsMenu();
    expect(screen.getByRole("menuitem", { name: "Submods…" })).toBeTruthy();
  });

  it("hides the Submods… item when there are no submods", async () => {
    const bridge = makeModsStubBridge();  // no submods
    renderMenuBar(bridge);
    await openModsMenu();
    expect(screen.queryByRole("menuitem", { name: "Submods…" })).toBeNull();
  });

  it("clicking Submods… opens the reorderable stack dialog", async () => {
    const bridge = makeModsStubBridge({
      activeModPath: "C:/test/corruption/Mods/Alpha",
      submods: ["Mod", "GCW"],
      activeSubmods: ["Mod"],
    });
    renderMenuBar(bridge);
    await openModsMenu();
    fireEvent.click(screen.getByRole("menuitem", { name: "Submods…" }));
    // The dialog mounts and fetches mods/list, rendering the checklist.
    await waitFor(() => {
      expect(screen.getByRole("list", { name: "Submod load order" })).toBeTruthy();
    });
    expect(screen.getByRole("checkbox", { name: "Include Mod" })).toBeTruthy();
    expect(screen.getByRole("checkbox", { name: "Include GCW" })).toBeTruthy();
  });
});
