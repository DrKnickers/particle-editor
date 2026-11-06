// Tests Preferences controls through bridge replies; no native picker or registry is used.
import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";
import { PreferencesDialog } from "../PreferencesDialog";
import { readConfirmDelete } from "@/lib/tree/delete-emitters";
import {
  ENGINE_HOST_SETUP_PROGRESSED_EVENT,
  ENGINE_HOST_SETUP_FINISHED_EVENT,
  type EngineHostSettingsDto,
  type EngineHostSetupStatusDto,
  type Request,
  type EngineHostSetupAcceptedDto,
} from "@particle-editor/bridge-schema";

const SETUP_EDITOR_PATH = "C:\\MapEditor\\EAW Terrain Editor.exe";
const MANAGED_EDITOR_PATH = "C:\\Managed\\EAW Terrain Editor.exe";

function makeSetupBridge(): { bridge: BridgeStub; settings: EngineHostSettingsDto; status: EngineHostSetupStatusDto } {
  const settings: EngineHostSettingsDto = { exePath: null };
  const status: EngineHostSetupStatusDto = { managedPath: null, isRunning: false };
  const bridge: BridgeStub = makeBridgeStub({
    responses: {
      "engineHost/settings": (): EngineHostSettingsDto => ({ ...settings }),
      "engineHost/setup/status": (): EngineHostSetupStatusDto => ({ ...status }),
      "engineHost/setup/start": { ok: true },
      "engineHost/setup/remove": { ok: true },
      "file/pick-open": (): { ok: true; path: string } => ({
        ok: true,
        path: SETUP_EDITOR_PATH,
      }),
    },
  });
  return { bridge, settings, status };
}

async function openSetupDialog(): Promise<HTMLElement> {
  const openButton: HTMLElement = screen.getByRole("button", { name: "Set up" });
  await waitFor((): void => {
    expect(openButton).toBeEnabled();
  });
  await userEvent.click(openButton);
  return screen.findByRole("dialog", { name: "Set up engine preview" });
}

async function chooseSetupEditor(dialog: HTMLElement): Promise<void> {
  await userEvent.click(within(dialog).getByRole("button", { name: "Browse map editor" }));
}

function makePrefsBridge(msaaLevels: number[] = [0, 2, 4]) {
  const bridge = makeBridgeStub({
    responses: { "engine/query/msaa-levels": { levels: msaaLevels, current: 4 } },
  });
  return { bridge, request: bridge.request };
}

describe("PreferencesDialog", () => {
  beforeEach(() => localStorage.clear());
  it("requires only the map editor path and sends it with the exe picker filter", async (): Promise<void> => {
    const { bridge }: { bridge: BridgeStub } = makeSetupBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    const dialog: HTMLElement = await openSetupDialog();
    const startButton: HTMLElement = within(dialog).getByRole("button", { name: "Set up" });
    expect(startButton).toBeDisabled();
    expect(within(dialog).queryByRole("button", { name: "Browse host package" })).not.toBeInTheDocument();
    await userEvent.click(within(dialog).getByRole("button", { name: "Browse map editor" }));
    expect(within(dialog).getByLabelText("Map editor")).toHaveAttribute("title", SETUP_EDITOR_PATH);
    expect(startButton).toBeEnabled();
    expect(bridge.request).toHaveBeenCalledWith({ kind: "file/pick-open", params: { filter: "exe" } });
    await userEvent.click(startButton);
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "engineHost/setup/start",
      params: { editorExePath: SETUP_EDITOR_PATH },
    });
    expect(startButton).toBeDisabled();
    expect(within(dialog).getByRole("button", { name: "Browse map editor" })).toBeDisabled();
  });

  it("renders progress, keeps working after close, and refreshes successful settings", async (): Promise<void> => {
    const { bridge, settings, status }: ReturnType<typeof makeSetupBridge> = makeSetupBridge();
    const { unmount }: ReturnType<typeof render> = render(
      <PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />,
    );
    const dialog: HTMLElement = await openSetupDialog();
    await chooseSetupEditor(dialog);
    await userEvent.click(within(dialog).getByRole("button", { name: "Set up" }));
    bridge.emit(ENGINE_HOST_SETUP_PROGRESSED_EVENT, { stage: "Checking files", done: 0, total: 0 });
    expect(within(dialog).getByRole("progressbar")).not.toHaveAttribute("value");
    bridge.emit(ENGINE_HOST_SETUP_PROGRESSED_EVENT, { stage: "Copying game data", done: 1, total: 4 });
    expect(within(dialog).getByText("Setting up: Copying game data (1 of 4)")).toBeInTheDocument();
    expect(within(dialog).getByRole("progressbar")).toHaveAttribute("value", "1");
    await userEvent.click(within(dialog).getByText("Close"));
    expect(screen.getByText("Setting up: Copying game data (1 of 4)")).toBeInTheDocument();
    settings.exePath = MANAGED_EDITOR_PATH;
    status.managedPath = MANAGED_EDITOR_PATH;
    bridge.emit(ENGINE_HOST_SETUP_FINISHED_EVENT, { ok: true, action: "setup", exePath: MANAGED_EDITOR_PATH });
    await waitFor((): void => {
      expect(screen.getByLabelText("Managed setup path")).toHaveTextContent(MANAGED_EDITOR_PATH);
    });
    expect(screen.getByLabelText("Map editor executable")).toHaveTextContent(MANAGED_EDITOR_PATH);
    expect(screen.getByText(`Setup complete: ${MANAGED_EDITOR_PATH}`)).toBeInTheDocument();
    expect(
      bridge.request.mock.calls.filter(([request]: [Request]): boolean => request.kind === "engineHost/settings"),
    ).toHaveLength(2);
    expect(
      bridge.request.mock.calls.filter(([request]: [Request]): boolean => request.kind === "engineHost/setup/status"),
    ).toHaveLength(2);
    unmount();
    expect(bridge.listenerCount(ENGINE_HOST_SETUP_PROGRESSED_EVENT)).toBe(0);
    expect(bridge.listenerCount(ENGINE_HOST_SETUP_FINISHED_EVENT)).toBe(0);
  });

  it("shows setup failure and allows retry", async (): Promise<void> => {
    const { bridge }: { bridge: BridgeStub } = makeSetupBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    const dialog: HTMLElement = await openSetupDialog();
    await chooseSetupEditor(dialog);
    await userEvent.click(within(dialog).getByRole("button", { name: "Set up" }));
    bridge.emit(ENGINE_HOST_SETUP_FINISHED_EVENT, { ok: false, action: "setup", error: "Not enough disk space" });
    expect(within(dialog).getByRole("alert")).toHaveTextContent("Not enough disk space");
    await waitFor((): void => {
      expect(within(dialog).getByRole("button", { name: "Set up" })).toBeEnabled();
    });
    expect(within(dialog).getByRole("button", { name: "Browse map editor" })).toBeEnabled();
  });

  it("shows picker and immediate setup errors without waiting for completion events", async (): Promise<void> => {
    let hasPickerFailed = true;
    const bridge: BridgeStub = makeBridgeStub({
      responses: {
        "engineHost/settings": { exePath: null },
        "engineHost/setup/status": { managedPath: null, isRunning: false },
        "file/pick-open": (): { ok: false; error: string } | { ok: true; path: string } => {
          if (hasPickerFailed) return { ok: false, error: "Cannot open picker" };
          return { ok: true, path: SETUP_EDITOR_PATH };
        },
        "engineHost/setup/start": (): EngineHostSetupAcceptedDto => ({ ok: false, error: "Preview is still running" }),
      },
    });
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    const dialog: HTMLElement = await openSetupDialog();
    await userEvent.click(within(dialog).getByRole("button", { name: "Browse map editor" }));
    expect(within(dialog).getByRole("alert")).toHaveTextContent("Cannot open picker");
    hasPickerFailed = false;
    await chooseSetupEditor(dialog);
    await userEvent.click(within(dialog).getByRole("button", { name: "Set up" }));
    expect(within(dialog).getByRole("alert")).toHaveTextContent("Preview is still running");
    expect(within(dialog).getByRole("button", { name: "Set up" })).toBeEnabled();
  });

  it("loads an already running setup and disables conflicting actions", async (): Promise<void> => {
    const { bridge, status }: ReturnType<typeof makeSetupBridge> = makeSetupBridge();
    status.isRunning = true;
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    expect(await screen.findByText("Setting up")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Set up" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "Browse" })).toBeDisabled();
  });

  it("requires confirmation before removing the managed copy and refreshes after removal", async (): Promise<void> => {
    const { bridge, settings, status }: ReturnType<typeof makeSetupBridge> = makeSetupBridge();
    settings.exePath = MANAGED_EDITOR_PATH;
    status.managedPath = MANAGED_EDITOR_PATH;
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    await userEvent.click(await screen.findByRole("button", { name: "Remove setup" }));
    expect(bridge.request).not.toHaveBeenCalledWith({ kind: "engineHost/setup/remove", params: {} });
    await userEvent.click(screen.getByRole("button", { name: "Confirm remove" }));
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineHost/setup/remove", params: {} });
    bridge.emit(ENGINE_HOST_SETUP_PROGRESSED_EVENT, { stage: "Deleting files", done: 1, total: 4 });
    expect(screen.getByText("Removing setup: Deleting files (1 of 4)")).toBeInTheDocument();
    settings.exePath = null;
    status.managedPath = null;
    bridge.emit(ENGINE_HOST_SETUP_FINISHED_EVENT, { ok: true, action: "remove" });
    await waitFor((): void => {
      expect(screen.queryByRole("button", { name: "Remove setup" })).not.toBeInTheDocument();
    });
    expect(screen.getByLabelText("Map editor executable")).toHaveTextContent("Not set");
    expect(screen.getByText("Setup removed.")).toBeInTheDocument();
  });
  it("loads, browses and clears the map editor executable", async (): Promise<void> => {
    const path = "C:\\Preview\\EAW Terrain Editor.exe";
    const bridge: BridgeStub = makeBridgeStub({
      responses: {
        "engineHost/settings": { exePath: null },
        "file/pick-open": { ok: true, path },
        "engineHost/settings/set": (params: EngineHostSettingsDto): EngineHostSettingsDto => ({
          exePath: params.exePath,
        }),
      },
    });
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    expect(screen.getByText("Engine preview")).toBeInTheDocument();
    expect(screen.getByLabelText("Map editor executable")).toHaveTextContent("Not set");
    await waitFor((): void => {
      expect(screen.getByRole("button", { name: "Browse" })).toBeEnabled();
    });
    expect(screen.getByRole("button", { name: "Clear" })).toBeDisabled();
    await userEvent.click(screen.getByRole("button", { name: "Browse" }));
    expect(bridge.request).toHaveBeenCalledWith({ kind: "file/pick-open", params: { filter: "exe" } });
    await waitFor((): void => {
      expect(screen.getByLabelText("Map editor executable")).toHaveTextContent(path);
    });
    await userEvent.click(screen.getByRole("button", { name: "Clear" }));
    await waitFor((): void => {
      expect(screen.getByLabelText("Map editor executable")).toHaveTextContent("Not set");
    });
    expect(bridge.request).toHaveBeenCalledWith({ kind: "engineHost/settings/set", params: { exePath: null } });
  });

  it("preserves the path on picker cancellation and shows setting failures", async (): Promise<void> => {
    const path = "C:\\Preview\\editor.exe";
    const bridge: BridgeStub = makeBridgeStub({
      responses: {
        "engineHost/settings": { exePath: path },
        "file/pick-open": { ok: false, error: "cancelled" },
        "engineHost/settings/set": (): never => {
          throw new Error("Cannot save settings");
        },
      },
    });
    render(<PreferencesDialog bridge={bridge} open onOpenChange={(): void => {}} />);
    await waitFor((): void => {
      expect(screen.getByRole("button", { name: "Browse" })).toBeEnabled();
    });
    await userEvent.click(screen.getByRole("button", { name: "Browse" }));
    expect(bridge.request).not.toHaveBeenCalledWith(expect.objectContaining({ kind: "engineHost/settings/set" }));
    await userEvent.click(screen.getByRole("button", { name: "Clear" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("Cannot save settings");
    expect(screen.getByLabelText("Map editor executable")).toHaveTextContent(path);
  });
  it("renders a 3-way theme control", () => {
    render(<PreferencesDialog bridge={makePrefsBridge().bridge} open onOpenChange={() => {}} />);
    expect(screen.getByRole("radio", { name: /dark/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /light/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /system/i })).toBeInTheDocument();
  });
  it("selecting Light applies + persists the mode", () => {
    render(<PreferencesDialog bridge={makePrefsBridge().bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("radio", { name: /light/i }));
    expect(document.documentElement.dataset.theme).toBe("light");
    expect(localStorage.getItem("alo:theme")).toBe("light");
  });
  it("toggles and persists confirm-before-delete", async () => {
    localStorage.removeItem("alo:confirm-delete");
    render(<PreferencesDialog bridge={makePrefsBridge().bridge} open onOpenChange={() => {}} />);
    const box = screen.getByLabelText("Confirm before deleting emitters") as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    await userEvent.click(box);
    expect(box.checked).toBe(false);
    expect(readConfirmDelete()).toBe(false); // persisted
  });

  it("renders the preview guard controls (checkbox on, number enabled, no warning)", () => {
    const { bridge } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /limit preview particle count/i });
    expect(box).toBeChecked();
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    expect(num).toBeEnabled();
    expect((num as HTMLInputElement).value).toBe("10000");
    expect(screen.queryByText(/can crash the editor/i)).not.toBeInTheDocument();
  });

  it("unchecking sends enabled:false, persists, greys the number, shows the warning", () => {
    const { bridge, request } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("checkbox", { name: /limit preview particle count/i }));
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: false, maxParticles: 10_000 },
    });
    expect(JSON.parse(localStorage.getItem("alo:overload-guard")!)).toEqual({
      enabled: false,
      maxParticles: 10_000,
    });
    expect(screen.getByRole("spinbutton", { name: /max preview particles/i })).toBeDisabled();
    expect(screen.getByText(/can crash the editor/i)).toBeInTheDocument();
  });

  it("committing a new cap on blur clamps, persists, and sends", () => {
    const { bridge, request } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    fireEvent.change(num, { target: { value: "50" } });
    fireEvent.blur(num);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 1_000 },
    });
    expect((num as HTMLInputElement).value).toBe("1000");
  });

  it("Enter commits the cap too", () => {
    const { bridge, request } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    fireEvent.change(num, { target: { value: "60000" } });
    fireEvent.keyDown(num, { key: "Enter" });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 60_000 },
    });
  });

  it("renders the Antialiasing select disabled while query is in-flight", () => {
    // The stub resolves on the next microtask — before that the select should be disabled.
    const { bridge } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = screen.getByRole("combobox", { name: /antialiasing/i });
    // Initially disabled (query not yet resolved)
    expect(sel).toBeDisabled();
  });

  it("populates Antialiasing options from the query result and defaults to saved level", async () => {
    localStorage.setItem("alo:msaa-quality", "2");
    const { bridge } = makePrefsBridge([0, 2, 4]);
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    expect((sel as HTMLSelectElement).value).toBe("2");
    const options = Array.from((sel as HTMLSelectElement).options).map((o) => o.text);
    expect(options).toEqual(["Off", "2× MSAA", "4× MSAA"]);
  });

  it("selecting a different MSAA level persists and sends the bridge call", async () => {
    localStorage.setItem("alo:msaa-quality", "0");
    const { bridge, request } = makePrefsBridge([0, 2, 4]);
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    fireEvent.change(sel, { target: { value: "2" } });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: { level: 2 },
    });
    expect(localStorage.getItem("alo:msaa-quality")).toBe("2");
  });

  it("displays the engine's current level when saved level isn't offered, without writing localStorage or sending the bridge", async () => {
    // Saved 8× but the GPU only reports [0, 2, 4]; engine's current is 4.
    localStorage.setItem("alo:msaa-quality", "8");
    const { bridge, request } = makePrefsBridge([0, 2, 4]); // stub returns current: 4
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    // Should display the engine's authoritative current (4), not Off (0).
    expect((sel as HTMLSelectElement).value).toBe("4");
    // Must NOT have persisted or sent on mount — only onChange does that.
    expect(localStorage.getItem("alo:msaa-quality")).toBe("8"); // saved intent preserved
    expect(request).not.toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: expect.anything(),
    });
  });

  it("query failure leaves the control showing the saved level without persisting or sending", async () => {
    localStorage.setItem("alo:msaa-quality", "4");
    // Bridge rejects the query entirely.
    const bridge = makeBridgeStub({
      responses: { "engine/query/msaa-levels": () => Promise.reject(new Error("bridge down")) },
    });
    const request = bridge.request;
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    // The query will resolve (to the unknown fallback) on the next microtask.
    // Let it settle, then verify the control stayed as-is.
    await waitFor(() => {});
    const sel = screen.getByRole("combobox", { name: /antialiasing/i });
    // select stays disabled (msaaLevels never set) but shows saved value in the seed option.
    expect((sel as HTMLSelectElement).value).toBe("4");
    // No persist, no send.
    expect(localStorage.getItem("alo:msaa-quality")).toBe("4");
    expect(request).not.toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: expect.anything(),
    });
  });

  it("Model shadows defaults on; unchecking sends enabled:false and persists", () => {
    const { bridge, request } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /model shadows/i }) as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/model-shadows",
      params: { enabled: false },
    });
    expect(localStorage.getItem("alo:model-shadows")).toBe("0");
  });

  it("Soft shadows defaults on; unchecking sends enabled:false and persists", () => {
    const { bridge, request } = makePrefsBridge();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /soft shadows/i }) as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/soft-shadows",
      params: { enabled: false },
    });
    expect(localStorage.getItem("alo:soft-shadows")).toBe("0");
  });

  // The Max-particles ▲/▼ steppers must step from the value VISIBLE in the
  // field (the uncommitted draft), not the last-committed guard value — else a
  // typed-but-unblurred edit is silently discarded (a past stepper regression).
  it("stepper ▲/▼ step from the visible draft, not the committed value", () => {
    const { bridge, request } = makePrefsBridge(); // default cap 10000
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i }) as HTMLInputElement;
    // Type a new value but do NOT commit (no blur / Enter).
    fireEvent.change(num, { target: { value: "5000" } });
    // ▲ steps from the visible 5000 → 6000 (NOT from committed 10000 → 11000).
    fireEvent.click(screen.getByRole("button", { name: /increase max preview particles/i }));
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 6_000 },
    });
    expect(num.value).toBe("6000");
    // ▼ steps back from the now-committed 6000 → 5000.
    fireEvent.click(screen.getByRole("button", { name: /decrease max preview particles/i }));
    expect(num.value).toBe("5000");
  });

  it("Soft shadows depends on Model shadows: disabled when off, stored pref not clobbered", () => {
    const { bridge } = makePrefsBridge(); // both default on
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const soft = screen.getByRole("checkbox", { name: /soft shadows/i }) as HTMLInputElement;
    expect(soft).toBeEnabled();
    expect(soft.checked).toBe(true);
    // Turn Model shadows off — the Soft-shadows control disables…
    fireEvent.click(screen.getByRole("checkbox", { name: /model shadows/i }));
    expect(soft).toBeDisabled();
    // …but the parent toggle must never overwrite the stored soft preference.
    expect(localStorage.getItem("alo:soft-shadows")).not.toBe("0");
  });
});
