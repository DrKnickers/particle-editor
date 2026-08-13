import type { Bridge } from "@particle-editor/bridge-schema";
import { vi } from "vitest";

export type BridgeStub = Bridge & { request: ReturnType<typeof vi.fn> };

export function makeBridgeStub(): BridgeStub {
  return {
    request: vi.fn().mockResolvedValue({}),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as BridgeStub;
}
