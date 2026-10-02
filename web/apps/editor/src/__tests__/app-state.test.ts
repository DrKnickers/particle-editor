// resetAppState() must return every web-side session store to its boot
// state. Each store is dirtied the way a test (or a user) would leave it,
// then reset — any store the helper misses keeps its dirty value here.

import { describe, it, expect } from "vitest";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";
import { resetAppState } from "@/test/app-state";
import { getAtlasContext, publishAtlasContext } from "@/lib/atlas/atlas-context";
import { atlasPanelCache } from "@/lib/atlas/atlas-panel-cache";
import { getCurveKeysClipboard, setCurveKeysClipboard } from "@/lib/curve/curve-key-clipboard";
import { computeDeleteImpact, useDeleteConfirmStore } from "@/lib/tree/delete-emitters";
import { useDockAnim } from "@/lib/dock-anim";
import { useEmitterClipboardStore } from "@/lib/tree/emitter-clipboard";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { useEmitterTreeStore } from "@/lib/tree/emitter-tree";
import { useFileOpErrorStore } from "@/lib/file-op";
import { useFileStateStore } from "@/lib/file-state";
import { useModalOpen } from "@/lib/modal-open";
import { useModStack } from "@/lib/mod-stack";
import { isRecording, markRecording } from "@/lib/record/record-mode";
import { setDock, useRightDockStoreForTests } from "@/lib/right-dock";
import { useStatusFeedback } from "@/lib/status-feedback";
import { useTreeActionStore } from "@/lib/tree/tree-action";
import { useTreeContextStore } from "@/lib/tree/tree-context";

describe("resetAppState", () => {
  it("returns every web-side session store to its boot state", () => {
    localStorage.clear();
    const tree = { root: { id: -1, name: "", role: "root", visible: true, children: [] } } as unknown as EmitterTreeDto;
    useEmitterTreeStore.getState().setTree(tree, {} as Bridge);
    useEmitterSelectionStore.getState().setIds([4, 5], 5);
    useTreeContextStore.getState().openDialog("set-link-group", 5, 2);
    useTreeActionStore.getState().requestRename(5);
    useEmitterClipboardStore.getState().markCopied();
    setCurveKeysClipboard([{ time: 0, value: 1 }]);
    useDeleteConfirmStore.getState().open([5], computeDeleteImpact([5], tree), tree);
    useFileOpErrorStore.getState().show("boom");
    useFileStateStore.setState({ currentFilePath: "C:/old.alo", dirty: true, recentFiles: ["C:/old.alo"] });
    useModalOpen.getState().open();
    useDockAnim.getState().setAnimating(true);
    publishAtlasContext({ ...getAtlasContext(), emitterId: 5 });
    atlasPanelCache.gridW = 320;
    useModStack.setState({ stack: ["ModA"] });
    markRecording();
    setDock("lighting");
    localStorage.clear(); // the dock persists; its reset re-reads storage
    useStatusFeedback.getState().announce("Copied");

    resetAppState();

    expect(useEmitterTreeStore.getState()).toMatchObject({ tree: null, bridge: null });
    expect(useEmitterSelectionStore.getState()).toMatchObject({ ids: [], primary: null, anchor: null });
    expect(useTreeContextStore.getState()).toMatchObject({ open: null, targetEmitterId: null, targetLinkGroupId: null });
    expect(useTreeActionStore.getState().renameRequest).toBeNull();
    expect(useEmitterClipboardStore.getState().hasContent).toBe(false);
    expect(getCurveKeysClipboard()).toEqual([]);
    expect(useDeleteConfirmStore.getState().pending).toBeNull();
    expect(useFileOpErrorStore.getState().message).toBeNull();
    expect(useFileStateStore.getState()).toMatchObject({
      currentFilePath: null, dirty: false, recentFiles: [], pendingAction: null,
    });
    expect(useModalOpen.getState().count).toBe(0);
    expect(useDockAnim.getState().animating).toBe(false);
    expect(getAtlasContext().emitterId).toBeNull();
    expect(atlasPanelCache.gridW).toBeNull();
    expect(useModStack.getState().stack).toEqual([]);
    expect(isRecording()).toBe(false);
    expect(useRightDockStoreForTests().getState().dock).toBe("spawner");
    expect(useStatusFeedback.getState().message).toBeNull();
  });
});
