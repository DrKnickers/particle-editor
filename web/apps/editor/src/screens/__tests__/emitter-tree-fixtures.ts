import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";
import { vi } from "vitest";
import type { Rect } from "@/lib/marquee";

export function treeWithChildren(): EmitterTreeDto {
  return {
    root: {
      id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
      children: [
        {
          id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 1, stableId: 101, name: "Smoke embers", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
            { id: 2, stableId: 102, name: "Smoke puff",   role: "death",    linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 3, stableId: 103, name: "Sparks", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 4, stableId: 104, name: "Spark trail", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 5, stableId: 105, name: "Flash", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
      ],
    },
  };
}

export function flatRootsTree(): EmitterTreeDto {
  return {
    root: {
      id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
      children: [
        {
          id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
        {
          id: 3, stableId: 103, name: "Sparks", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
        {
          id: 5, stableId: 105, name: "Flash", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
      ],
    },
  };
}

export function marqueeFlatRootsTree(): EmitterTreeDto {
  return {
    root: {
      id: -1,
      stableId: 0,
      name: "",
      role: "root",
      linkGroup: 0,
      visible: true,
      spawn: ZERO_SPAWN,
      children: [
        { id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
        { id: 1, stableId: 101, name: "Sparks", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
        { id: 2, stableId: 102, name: "Flash", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
      ],
    },
  };
}

export function makeStubBridge(tree: EmitterTreeDto = treeWithChildren()) {
  const snapshot = { selectedEmitterId: null };
  return {
    request: vi.fn().mockImplementation((req: { kind: string; params?: unknown }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "emitters/select") return Promise.resolve({});
      if (req.kind === "emitters/drop") return Promise.resolve({ ok: true });
      if (req.kind === "emitters/reorder-many") {
        const ids = (req.params as { ids: number[] }).ids;
        return Promise.resolve({ ok: true, newIds: ids });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

export function stubRect(el: HTMLElement, rect: Rect): ReturnType<typeof vi.fn>;
export function stubRect(el: HTMLElement, top: number, height: number): ReturnType<typeof vi.fn>;
export function stubRect(el: HTMLElement, rectOrTop: Rect | number, height?: number) {
  const rect = typeof rectOrTop === "number"
    ? { left: 0, top: rectOrTop, right: 200, bottom: rectOrTop + height! }
    : rectOrTop;
  const spy = vi.fn(() => ({
    left: rect.left,
    top: rect.top,
    right: rect.right,
    bottom: rect.bottom,
    width: rect.right - rect.left,
    height: rect.bottom - rect.top,
    x: rect.left,
    y: rect.top,
    toJSON: () => "{}",
  }));
  Object.defineProperty(el, "getBoundingClientRect", {
    configurable: true,
    writable: true,
    value: spy,
  });
  return spy;
}
