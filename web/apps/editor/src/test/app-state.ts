// One reset for the web app's own session state — the app-side twin of
// resetMockState().
//
// These Zustand stores and module caches are singletons. In production the
// page is the session (an error-boundary Reload or a WebView2 crash recovery
// reloads the page), so they never outlive their <App/>. In a test file they
// do: a dialog left open, a copied-emitter flag or an error message from one
// render shows up in the next. Startup seeds re-read some of them from the
// host on mount (selection, file state, mod stack, the copied-emitter flag);
// the rest would just keep what the last test left. Call `resetAppState()` in a beforeEach of any
// suite that mounts <App/>, alongside resetMockState().
//
// Persisted preferences (palette slots, panel layout, picker state) are read
// from localStorage and are left to the suites that exercise them.

import { __resetAtlasContext } from "@/lib/atlas/atlas-context";
import { __resetAtlasPanelCacheForTests } from "@/lib/atlas/atlas-panel-cache";
import { setCurveKeysClipboard } from "@/lib/curve/curve-key-clipboard";
import { useDeleteConfirmStore } from "@/lib/tree/delete-emitters";
import { useDockAnim } from "@/lib/dock-anim";
import { useEmitterClipboardStore } from "@/lib/tree/emitter-clipboard";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { useEmitterTreeStore } from "@/lib/tree/emitter-tree";
import { useFileOpErrorStore } from "@/lib/file-op";
import { useFileStateStore } from "@/lib/file-state";
import { useModalOpen } from "@/lib/modal-open";
import { __resetModStackForTests } from "@/lib/mod-stack";
import { __resetRecordModeForTests } from "@/lib/record/record-mode";
import { __resetRightDockForTests } from "@/lib/right-dock";
import { __resetStatusFeedbackForTests } from "@/lib/status-feedback";
import { useTreeActionStore } from "@/lib/tree/tree-action";
import { useTreeContextStore } from "@/lib/tree/tree-context";

export function resetAppState(): void {
  useEmitterTreeStore.setState({ tree: null, bridge: null });
  useEmitterSelectionStore.setState({ ids: [], primary: null, anchor: null });
  useTreeContextStore.getState().close();
  useTreeActionStore.setState({ renameRequest: null });
  useEmitterClipboardStore.setState({ hasContent: false });
  setCurveKeysClipboard([]);
  useDeleteConfirmStore.getState().clear();
  useFileOpErrorStore.getState().clear();
  useFileStateStore.setState({
    currentFilePath: null,
    dirty: false,
    recentFiles: [],
    pendingAction: null,
  });
  useModalOpen.setState({ count: 0 });
  useDockAnim.setState({ animating: false, atlasTerminalFirstPaint: false, atlasGridMounted: false });
  __resetAtlasContext();
  __resetAtlasPanelCacheForTests();
  __resetModStackForTests();
  __resetRecordModeForTests();
  __resetRightDockForTests();
  __resetStatusFeedbackForTests();
}
