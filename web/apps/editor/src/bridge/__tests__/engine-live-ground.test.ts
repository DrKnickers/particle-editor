// Checks the mock's persistent live-ground limitation through the bridge; no host rendering.
import { beforeEach, describe, expect, it } from "vitest";
import type { EngineLiveStatsDto } from "@particle-editor/bridge-schema";
import { MockBridge } from "../mock";
import { resetMockState } from "@/test/mock-state";

const GROUND_NOTE = "Engine view keeps its ground visible at height 0.";
const RAISED_GROUND_UNITS = 10;

beforeEach(resetMockState);

describe("live engine ground note", (): void => {
  it("explains hidden or raised ground and clears when defaults return", async (): Promise<void> => {
    const bridge: MockBridge = new MockBridge();
    bridge.enginePreview.live = "live";
    await bridge.request({ kind: "engine/set/ground", params: { enabled: true } });
    await bridge.request({ kind: "engine/set/ground-z", params: { z: 0 } });
    const defaults: EngineLiveStatsDto = await bridge.request({ kind: "engineLive/stats", params: {} });
    expect(defaults.groundNote).toBe("");
    await bridge.request({ kind: "engine/set/ground", params: { enabled: false } });
    expect((await bridge.request({ kind: "engineLive/stats", params: {} })).groundNote).toBe(GROUND_NOTE);
    await bridge.request({ kind: "engine/set/ground-z", params: { z: RAISED_GROUND_UNITS } });
    await bridge.request({ kind: "engine/set/ground", params: { enabled: true } });
    expect((await bridge.request({ kind: "engineLive/stats", params: {} })).groundNote).toBe(GROUND_NOTE);
    await bridge.request({ kind: "engineLive/stop", params: {} });
    expect((await bridge.request({ kind: "engineLive/stats", params: {} })).groundNote).toBe("");
    bridge.enginePreview.live = "live";
    await bridge.request({ kind: "engine/set/ground-z", params: { z: 0 } });
    expect((await bridge.request({ kind: "engineLive/stats", params: {} })).groundNote).toBe("");
  });
});
