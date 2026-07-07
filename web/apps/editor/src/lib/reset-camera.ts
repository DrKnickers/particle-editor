// reset-camera.ts — single source of truth for the Reset-Camera default
// vectors, shared by the View → Reset Camera menu item (MenuBar.tsx) and the
// Ctrl+Home accelerator (use-app-accelerators.ts). Both dispatch
// `engine/set/camera` with these exact values, so keeping ONE constant means
// the menu item and the shortcut can never silently drift apart.
//
// The values mirror the legacy editor exactly:
//   - ID_VIEW_RESETCAMERA handler            — the legacy main.cpp
//   - Engine constructor default (m_eye)     — src/engine.cpp:2190-2192
//   eye (0,-250,125), target origin, up +Z.
// The host's `engine/set/camera` handler (src/host/BridgeDispatch_Engine.cpp)
// maps this DTO 1:1 into Engine::Camera and calls the SAME Engine::SetCamera()
// the legacy command invokes, so parity is exact end-to-end.

import type { CameraDto } from "@particle-editor/bridge-schema";

export const RESET_CAMERA: CameraDto = {
  position: [0, -250, 125],
  target: [0, 0, 0],
  up: [0, 0, 1],
};
