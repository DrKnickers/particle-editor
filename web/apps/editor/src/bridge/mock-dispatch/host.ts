// Shared instance state for the mock counterparts of the native bridge handlers.

import type {
  Event,
  EngineStateDto,
  LightingSettingsDto,
} from "@particle-editor/bridge-schema";

export interface MockDispatchHost {
  emit(e: Event): void;
  patchAndBroadcast(patch: Partial<EngineStateDto>): void;
  markClean(): void;
  commitFilePath(path: string): void;
  layerStack: string[];
  spawnerActiveCount: number;
  lightingOverride: LightingSettingsDto | null;
  lightingForceAlign: boolean;
  lastOverloadGuard: { enabled: boolean; maxParticles: number } | null;
}
