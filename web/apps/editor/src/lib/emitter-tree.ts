// emitter-tree.ts — the latest EmitterTreeDto, lifted out of EmitterTree's
// local state so MenuBar and the delete helper can read it (non-reactively,
// via getState()) to compute subtree impact for the delete confirmation.
// EmitterTree reads/writes it as its tree state; nothing else's render
// behaviour changes.
import { create } from "zustand";
import type { EmitterTreeDto } from "@particle-editor/bridge-schema";

type EmitterTreeStore = {
  tree: EmitterTreeDto | null;
  setTree: (tree: EmitterTreeDto | null) => void;
};
export const useEmitterTreeStore = create<EmitterTreeStore>((set) => ({
  tree: null,
  setTree: (tree) => set({ tree }),
}));
