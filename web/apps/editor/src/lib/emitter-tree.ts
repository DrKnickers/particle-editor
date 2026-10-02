// emitter-tree.ts — the latest EmitterTreeDto, lifted out of EmitterTree's
// local state so MenuBar and the delete helper can read it (non-reactively,
// via getState()) to compute subtree impact for the delete confirmation.
// EmitterTree reads/writes it as its tree state; nothing else's render
// behaviour changes.
//
// The store is a module singleton, so it outlives any one <App/> mount, but
// a tree's ids (positional and stableId) only mean something to the bridge
// session that served it. It therefore records that bridge, and readers go
// through useEmitterTree / getEmitterTree, which treat another bridge's tree
// as "no tree yet": a remount on a new bridge never paints the old session's
// rows as current before its own emitters/list lands, while a same-bridge
// remount (View > Reset Panel Layout) keeps painting the live tree.
import { create } from "zustand";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";

type EmitterTreeStore = {
  tree: EmitterTreeDto | null;
  bridge: Bridge | null;
  setTree: (tree: EmitterTreeDto | null, bridge: Bridge) => void;
};
export const useEmitterTreeStore = create<EmitterTreeStore>((set) => ({
  tree: null,
  bridge: null,
  setTree: (tree, bridge) => set({ tree, bridge }),
}));

/** The tree `bridge` served, or null when the store holds none or another bridge's. */
export function useEmitterTree(bridge: Bridge): EmitterTreeDto | null {
  return useEmitterTreeStore((s) => (s.bridge === bridge ? s.tree : null));
}

/** Non-reactive form of useEmitterTree. */
export function getEmitterTree(bridge: Bridge): EmitterTreeDto | null {
  const s = useEmitterTreeStore.getState();
  return s.bridge === bridge ? s.tree : null;
}
