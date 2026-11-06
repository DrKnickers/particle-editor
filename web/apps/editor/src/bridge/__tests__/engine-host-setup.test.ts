// Verifies the setup bridge's asynchronous browser-mode refusal and read-only status.
import { describe, expect, it, vi } from "vitest";
import { MockBridge } from "../mock";
import {
  ENGINE_HOST_SETUP_FINISHED_EVENT,
  ENGINE_HOST_SETUP_PROGRESSED_EVENT,
  type EngineHostSetupFinishedDto,
  type EngineHostSetupAcceptedDto,
  type EventOf,
} from "@particle-editor/bridge-schema";

describe("engine host setup bridge", (): void => {
  it("reports no managed Windows installation in browser mode", async (): Promise<void> => {
    const bridge = new MockBridge();
    expect(await bridge.request({ kind: "engineHost/setup/status", params: {} })).toEqual({
      managedPath: null,
      isRunning: false,
    });
  });

  it.each(["setup", "remove"] as const)(
    "accepts %s then reports the native requirement",
    async (action: "setup" | "remove"): Promise<void> => {
      const bridge = new MockBridge();
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
      await Promise.resolve();
      expect(progressed).toHaveBeenCalledOnce();
      expect(finished).toEqual([{ ok: false, action, error: "Engine host setup requires the Windows editor." }]);
      expect(await bridge.request({ kind: "engineHost/settings", params: {} })).toEqual({ exePath: null });
    },
  );
});
