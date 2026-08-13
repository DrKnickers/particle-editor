import type { SpawnerParamsDto } from "@particle-editor/bridge-schema";

/** Defaults mirror `SpawnerConfig()` at [src/SpawnerDriver.h:18]:
 *  Auto mode + disabled + burst 1 + 0 s spacing + 10 s interval + origin
 *  + 5 s lifetime + zero jitter. */
export function makeDefaultSpawnerParams(): SpawnerParamsDto {
  return {
    mode: "auto",
    enabled: false,
    burstSize: 1,
    spacingSec: 0,
    intervalSec: 10,
    position: [0, 0, 0],
    velocity: [0, 0, 0],
    maxLifetimeSec: 5,
    jitterPosition: [0, 0, 0],
    acceleration: [0, 0, 0],
    squiggleAmplitude: [0, 0, 0],
    squiggleFrequency: 1,
  };
}
