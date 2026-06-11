// TDD: useEstimatedLoadPush — tree-driven push of estimated alive-particles
// to the engine (engine/set/estimated-load) with epsilon gating.
//
// Spec: docs/superpowers/specs/2026-06-11-overload-hard-guard-design.md §2.1
// Plan: docs/superpowers/plans/2026-06-11-overload-hard-guard.md Task 2

import { describe, it, expect, vi, beforeEach } from "vitest";
import { renderHook } from "@testing-library/react";
import type { EmitterTreeNode, SpawnParamsDto } from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import { useEstimatedLoadPush } from "../use-estimated-load-push";
import { estimateSystemLoad } from "../chain-load";

// ---------------------------------------------------------------------------
// Fixture helpers — mirror chain-load.test.ts conventions
// ---------------------------------------------------------------------------

const spawn = (s: Partial<SpawnParamsDto>): SpawnParamsDto => ({ ...ZERO_SPAWN, ...s });

let nextId = 100;
const node = (
  name: string,
  s: Partial<SpawnParamsDto>,
  children: EmitterTreeNode[] = [],
): EmitterTreeNode => ({
  id: nextId, stableId: nextId++, name, role: "root",
  linkGroup: 0, visible: true, spawn: spawn(s), children,
});

const syntheticRoot = (children: EmitterTreeNode[]): EmitterTreeNode => ({
  id: -1, stableId: 0, name: "", role: "root",
  linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children,
});

// ---------------------------------------------------------------------------
// Stub bridge
// ---------------------------------------------------------------------------

function makeBridge() {
  return { request: vi.fn().mockResolvedValue({}) };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

beforeEach(() => { nextId = 100; });

describe("useEstimatedLoadPush", () => {
  it("pushes engine/set/estimated-load once on mount with the computed perInstance", () => {
    const bridge = makeBridge();
    const root = syntheticRoot([node("a", { nParticlesPerSecond: 10, lifetime: 2 })]);
    const tree = { root };
    const expected = estimateSystemLoad(root);

    renderHook(() => useEstimatedLoadPush(bridge as never, tree));

    expect(bridge.request).toHaveBeenCalledTimes(1);
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "engine/set/estimated-load",
      params: { perInstance: expected },
    });
  });

  it("does NOT push a second time when re-rendered with a tree whose estimate is within epsilon", () => {
    const bridge = makeBridge();
    // Build two trees with the exact same spawn params → same estimate
    const makeTree = () =>
      syntheticRoot([node("a", { nParticlesPerSecond: 10, lifetime: 2 })]);

    const tree1 = { root: makeTree() };
    const tree2 = { root: makeTree() };
    // Sanity: the two estimates are equal (same params, same formula)
    expect(estimateSystemLoad(tree1.root)).toBeCloseTo(estimateSystemLoad(tree2.root), 6);

    const { rerender } = renderHook(
      ({ tree }: { tree: { root: EmitterTreeNode } }) =>
        useEstimatedLoadPush(bridge as never, tree),
      { initialProps: { tree: tree1 } },
    );

    expect(bridge.request).toHaveBeenCalledTimes(1);

    // Re-render with a different tree object but same estimated value
    rerender({ tree: tree2 });

    // Still only ONE push — epsilon gate suppressed the duplicate
    expect(bridge.request).toHaveBeenCalledTimes(1);
  });

  it("pushes a second time when the tree changes and the estimate differs materially", () => {
    const bridge = makeBridge();
    const smallRoot = syntheticRoot([node("a", { nParticlesPerSecond: 10, lifetime: 1 })]);
    const largeRoot = syntheticRoot([node("b", { nParticlesPerSecond: 200, lifetime: 5 })]);

    const { rerender } = renderHook(
      ({ tree }: { tree: { root: EmitterTreeNode } }) =>
        useEstimatedLoadPush(bridge as never, tree),
      { initialProps: { tree: { root: smallRoot } } },
    );

    expect(bridge.request).toHaveBeenCalledTimes(1);
    const firstCall = bridge.request.mock.calls[0][0];
    expect(firstCall.kind).toBe("engine/set/estimated-load");
    expect(firstCall.params.perInstance).toBeCloseTo(estimateSystemLoad(smallRoot), 6);

    rerender({ tree: { root: largeRoot } });

    expect(bridge.request).toHaveBeenCalledTimes(2);
    const secondCall = bridge.request.mock.calls[1][0];
    expect(secondCall.kind).toBe("engine/set/estimated-load");
    expect(secondCall.params.perInstance).toBeCloseTo(estimateSystemLoad(largeRoot), 6);
  });

  it("does not push when tree is null", () => {
    const bridge = makeBridge();
    renderHook(() => useEstimatedLoadPush(bridge as never, null));
    expect(bridge.request).not.toHaveBeenCalled();
  });
});
