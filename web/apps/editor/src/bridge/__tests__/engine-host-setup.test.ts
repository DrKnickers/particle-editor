// Verifies browser setup progress and persisted mock paths without touching the filesystem.
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { MockBridge, MOCK_MANAGED_EDITOR_PATH } from "../mock";
import {
  ENGINE_HOST_SETUP_FINISHED_EVENT,
  ENGINE_HOST_SETUP_PROGRESSED_EVENT,
  type EngineHostSetupFinishedDto,
  type EngineHostSetupAcceptedDto,
  type EventOf,
} from "@particle-editor/bridge-schema";

describe("engine host setup bridge", (): void => {
  beforeEach((): void => {
    vi.useFakeTimers();
  });
  afterEach((): void => {
    vi.useRealTimers();
  });
  it("reports no managed Windows installation in browser mode", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
      managedPath: null,
      setupAction: null,
      isHostRunning: false,
      isSetupComplete: false,
      packageError: null,
      isHostOutOfDate: false,
      bundledHostVersion: "1.0.0",
    });
  });

  it.each(["setup", "remove"] as const)(
    "accepts %s then emits progress and completion",
    async (action: "setup" | "remove"): Promise<void> => {
      const bridge: MockBridge = new MockBridge();
      const finished: EngineHostSetupFinishedDto[] = [];
      const progressed: ReturnType<typeof vi.fn> = vi.fn();
      bridge.on(ENGINE_HOST_SETUP_PROGRESSED_EVENT, progressed);
      bridge.on(ENGINE_HOST_SETUP_FINISHED_EVENT, (event: EventOf<typeof ENGINE_HOST_SETUP_FINISHED_EVENT>): void => {
        finished.push(event.payload);
      });
      const response: EngineHostSetupAcceptedDto =
        action === "setup"
          ? await bridge.request({
              kind: "engineHost/setup/start",
              params: { editorExePath: "C:\\editor\\editor.exe" },
            })
          : await bridge.request({ kind: "engineHost/setup/remove", params: {} });
      expect(response).toEqual({ ok: true });
      expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
        managedPath: null,
        setupAction: action,
        isHostRunning: false,
        isSetupComplete: false,
        packageError: null,
        isHostOutOfDate: false,
        bundledHostVersion: "1.0.0",
      });
      expect(await bridge.request({ kind: "engineHost/setup/remove", params: {} })).toEqual({
        ok: false,
        error: "A setup operation is already running.",
      });
      await vi.runAllTimersAsync();
      expect(progressed).toHaveBeenCalledTimes(3);
      const exePath: string | null = action === "setup" ? MOCK_MANAGED_EDITOR_PATH : null;
      expect(finished).toEqual([action === "setup" ? { ok: true, action, exePath } : { ok: true, action }]);
      expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath });
      expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
        managedPath: exePath,
        setupAction: null,
        isHostRunning: false,
        isSetupComplete: action === "setup",
        packageError: null,
        isHostOutOfDate: false,
        bundledHostVersion: "1.0.0",
      });
    },
  );

  it("refreshes an out-of-date setup and preserves the configured editor path", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    await bridge.request({ kind: "engineHost/setup/start", params: { editorExePath: "C:\\editor\\editor.exe" } });
    await vi.runAllTimersAsync();
    const customEditorPath = "C:\\other\\editor.exe";
    await bridge.request({ kind: "engineHost/settings/set", params: { exePath: customEditorPath } });
    bridge.isEngineHostOutOfDate = true;
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toMatchObject({
      isHostOutOfDate: true,
      bundledHostVersion: "1.0.0",
    });
    const finished: ReturnType<typeof vi.fn> = vi.fn();
    const progressed: ReturnType<typeof vi.fn> = vi.fn();
    bridge.on(ENGINE_HOST_SETUP_FINISHED_EVENT, finished);
    bridge.on(ENGINE_HOST_SETUP_PROGRESSED_EVENT, progressed);
    expect(await bridge.request({ kind: "engineHost/setup/refresh", params: {} })).toEqual({ ok: true });
    expect(await bridge.request({ kind: "engineHost/setup/refresh", params: {} })).toEqual({
      ok: false,
      error: "A setup operation is already running.",
    });
    await vi.runAllTimersAsync();
    expect(progressed).toHaveBeenCalled();
    expect(finished).toHaveBeenCalledWith({
      kind: ENGINE_HOST_SETUP_FINISHED_EVENT,
      payload: { ok: true, action: "refresh" },
    });
    expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: customEditorPath });
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
      managedPath: MOCK_MANAGED_EDITOR_PATH,
      setupAction: null,
      isHostRunning: false,
      isSetupComplete: true,
      packageError: null,
      isHostOutOfDate: false,
      bundledHostVersion: "1.0.0",
    });
  });

  it("cancels setup without publishing a managed copy and permits a retry", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    const finished: ReturnType<typeof vi.fn> = vi.fn();
    const progressed: ReturnType<typeof vi.fn> = vi.fn();
    bridge.on(ENGINE_HOST_SETUP_FINISHED_EVENT, finished);
    bridge.on(ENGINE_HOST_SETUP_PROGRESSED_EVENT, progressed);
    await bridge.request({ kind: "engineHost/setup/start", params: { editorExePath: "C:\\editor\\editor.exe" } });
    expect(await bridge.request({ kind: "engineHost/setup/cancel", params: {} })).toEqual({ ok: true });
    await vi.runAllTimersAsync();
    expect(progressed).not.toHaveBeenCalled();
    expect(finished).toHaveBeenCalledWith({
      kind: ENGINE_HOST_SETUP_FINISHED_EVENT,
      payload: { ok: false, action: "setup", error: "Setup was cancelled." },
    });
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toMatchObject({
      managedPath: null,
      setupAction: null,
      isSetupComplete: false,
    });
    expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: null });
    expect(
      await bridge.request({ kind: "engineHost/setup/start", params: { editorExePath: "C:\\editor\\editor.exe" } }),
    ).toEqual({ ok: true });
    await vi.runAllTimersAsync();
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toMatchObject({
      isSetupComplete: true,
    });
  });

  it.each(["refresh", "remove"] as const)(
    "ignores cancel during %s",
    async (action: "refresh" | "remove"): Promise<void> => {
      const bridge: MockBridge = new MockBridge();
      await bridge.request({ kind: "engineHost/setup/start", params: { editorExePath: "C:\\editor\\editor.exe" } });
      await vi.runAllTimersAsync();
      const finished: ReturnType<typeof vi.fn> = vi.fn();
      bridge.on(ENGINE_HOST_SETUP_FINISHED_EVENT, finished);
      await bridge.request({
        kind: action === "refresh" ? "engineHost/setup/refresh" : "engineHost/setup/remove",
        params: {},
      });
      await bridge.request({ kind: "engineHost/setup/cancel", params: {} });
      await vi.runAllTimersAsync();
      expect(finished).toHaveBeenCalledWith({ kind: ENGINE_HOST_SETUP_FINISHED_EVENT, payload: { ok: true, action } });
    },
  );
});
