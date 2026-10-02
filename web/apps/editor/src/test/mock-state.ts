// One reset for every piece of MockBridge state.
//
// MockBridge reads and writes module-level Zustand stores (plus the palette
// seed in mock.ts) that every `new MockBridge()` in a test file shares. Call
// `resetMockState()` in a beforeEach of any suite that drives a MockBridge so
// a mutation in one test cannot leak into the next. The stableId counter is
// deliberately NOT reset: it is process-monotonic, like the host's.

import { seedMockPalette } from "@/bridge/mock";
import {
  makeDefaultEmitterTree,
  makeDefaultEngineState,
  useMockEmitterClipboard,
  useMockEmitterProperties,
  useMockEmitterTree,
  useMockEngineState,
  useMockLinkGroupConflicts,
  useMockLinkGroupExempt,
  useMockRecentFiles,
  useMockTrackOverlay,
} from "@/bridge/mock-state";

export function resetMockState(): void {
  useMockEngineState.setState(makeDefaultEngineState());
  useMockRecentFiles.getState().reset();
  useMockEmitterTree.setState({ tree: makeDefaultEmitterTree() });
  useMockLinkGroupExempt.getState().resetAll();
  useMockLinkGroupConflicts.getState().resetAll();
  useMockEmitterClipboard.getState().reset();
  useMockTrackOverlay.getState().reset();
  useMockEmitterProperties.getState().reset();
  seedMockPalette(null);
}
