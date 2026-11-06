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
      isRunning: false,
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
              params: { editorExePath: "C:\\editor\\editor.exe", packageManifestPath: "C:\\package\\package.json" },
            })
          : await bridge.request({ kind: "engineHost/setup/remove", params: {} });
      expect(response).toEqual({ ok: true });
      expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
        managedPath: null,
        isRunning: true,
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
        isRunning: false,
      });
    },
  );
});
