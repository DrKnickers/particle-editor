// Mock counterpart of src/host/BridgeDispatch_SpawnerLighting.cpp.

import type {
  Request,
  SpawnerParamsDto,
} from "@particle-editor/bridge-schema";
import {
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type SpawnerLightingKind =
  | "spawner/start"
  | "spawner/trigger"
  | "spawner/stop"
  | "settings/lighting"
  | "settings/lighting/set"
  | "settings/lighting-force-align/set";

type SpawnerLightingRequest = Extract<Request, { kind: SpawnerLightingKind }>;

// Mirrors native ClampSpawnerConfig (src/simulation/SpawnerDriver.cpp): burstSize
// 1..MAX_BURST_SIZE (10), spacingSec 0..MAX_SPACING_SEC (10), intervalSec
// 0..MAX_INTERVAL_SEC (60), maxLifetimeSec 0..MAX_LIFETIME_SEC (600), every
// position / velocity / jitterPosition / acceleration / squiggleAmplitude axis
// +/-JITTER_MAX (10000), and squiggleFrequency 0..SQUIGGLE_FREQ_MAX (20).
// Native code remains the source of truth.
function clampSpawnerConfig(params: SpawnerParamsDto): SpawnerParamsDto {
  const clamp = (value: number, min: number, max: number) =>
    Math.min(max, Math.max(min, value));
  const vec3 = (value: SpawnerParamsDto["position"]): SpawnerParamsDto["position"] =>
    [clamp(value[0], -10_000, 10_000), clamp(value[1], -10_000, 10_000), clamp(value[2], -10_000, 10_000)];
  return {
    ...params,
    mode: params.mode === "manual" ? "manual" : "auto",
    burstSize: clamp(params.burstSize, 1, 10),
    spacingSec: clamp(params.spacingSec, 0, 10),
    intervalSec: clamp(params.intervalSec, 0, 60),
    maxLifetimeSec: clamp(params.maxLifetimeSec, 0, 600),
    position: vec3(params.position),
    velocity: vec3(params.velocity),
    jitterPosition: vec3(params.jitterPosition),
    acceleration: vec3(params.acceleration),
    squiggleAmplitude: vec3(params.squiggleAmplitude),
    squiggleFrequency: clamp(params.squiggleFrequency, 0, 20),
  };
}

export function dispatchSpawnerLighting(this: MockDispatchHost, req: SpawnerLightingRequest): unknown {
  switch (req.kind) {

    // ---------------- spawner ----------------
    //
    // The native host treats spawner/start as a full-config replace
    // (mirrors `SpawnerDriver::SetConfig`). The mock matches: every
    // clamped incoming params overwrite the cached spawner block in
    // EngineStateDto, then emits engine/state/changed so any panel
    // subscribed to snapshots picks up the new config.
    //
    // The mock doesn't simulate physics: Manual trigger bumps the count by
    // burstSize, and stop disables the config plus zeroes the count. Real
    // instance tracking lives in the native SpawnerDriver.

    case "spawner/start":
      this.patchAndBroadcast({ spawner: clampSpawnerConfig(req.params) });
      return {};

    case "spawner/trigger": {
      const params = snapshotEngineState().spawner;
      if (params.mode !== "manual") return {};
      const next = this.spawnerActiveCount + params.burstSize;
      this.spawnerActiveCount = next;
      this.emit({
        kind: "spawner/active-count",
        payload: { count: next },
      });
      return {};
    }

    case "spawner/stop": {
      const spawner = snapshotEngineState().spawner;
      this.patchAndBroadcast({ spawner: { ...spawner, enabled: false } });
      this.spawnerActiveCount = 0;
      this.emit({
        kind: "spawner/active-count",
        payload: { count: 0 },
      });
      return {};
    }

    // ---------------- settings: cross-mode registry ----------------
    //
    // `settings/lighting` returns the raw lighting split (the native
    // host reads it from the registry; browser mode returns the
    // canonical defaults (matching the legacy Win32 dialog), with the live
    // in-memory `lightingForceAlign` flag). `…/set` writes just the
    // flag. No event is emitted — the constraint is enforced UI-side
    // in LightingPane, and lighting isn't part of EngineStateDto.
    case "settings/lighting": {
      if (this.lightingOverride) {
        // Last written snapshot wins; forceAlign tracks the live flag so a
        // standalone `…/force-align/set` after a full write is still seen.
        return { ...this.lightingOverride, forceAlign: this.lightingForceAlign };
      }
      const rgb = (r: number, g: number, b: number) => r | (g << 8) | (b << 16);
      return {
        sun:   { intensity: 0.5, az: 0,   alt: 45,  diffuse: rgb(180, 180, 190), specular: rgb(190, 190, 200) },
        fill1: { intensity: 0.5, az: 120, alt: -10, diffuse: rgb(60, 80, 160),   specular: 0 },
        fill2: { intensity: 0.5, az: 210, alt: -10, diffuse: rgb(60, 80, 160),   specular: 0 },
        ambient: rgb(40, 40, 50),
        shadow:  rgb(100, 100, 110),
        forceAlign: this.lightingForceAlign,
      };
    }

    case "settings/lighting/set":
      this.lightingOverride = req.params;
      this.lightingForceAlign = req.params.forceAlign;
      return {};

    case "settings/lighting-force-align/set":
      this.lightingForceAlign = req.params.enabled;
      return {};

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
