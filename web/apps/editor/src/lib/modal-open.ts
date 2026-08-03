// modal-open.ts — a tiny global "is any blocking modal open" depth counter.
//
// The viewport forwards global keydown/keyup to the engine (ViewportSlot) and
// only skips text-input targets — so a key pressed while a modal's OK/Cancel
// button is focused would still drive the viewport behind the modal
// (release-audit #12). Every shared <Modal> increments this on open and
// decrements on close/unmount; ViewportSlot suppresses viewport keys whenever
// `count > 0`. A depth counter (not a boolean) keeps nested/stacked modals
// correct.
import { create } from "zustand";

type ModalOpenStore = {
  count: number;
  open: () => void;
  close: () => void;
};

export const useModalOpen = create<ModalOpenStore>((set) => ({
  count: 0,
  open: () => set((s) => ({ count: s.count + 1 })),
  close: () => set((s) => ({ count: Math.max(0, s.count - 1) })),
}));
