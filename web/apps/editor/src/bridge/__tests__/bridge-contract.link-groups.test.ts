// Contract tests for MockBridge — link-group membership enforcement and exempt fields.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import {
  useMockLinkGroupConflicts,
} from "../mock-state";
import type {
  EmitterTreeDto,
} from "@particle-editor/bridge-schema";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — linkGroups/*", () => {
  it("linkGroups/list-exempt-fields returns the v1 default exempt set for an unknown group", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    // Defaults: colorTexture + normalTexture + trackIndex.
    expect(r.fields).toEqual(expect.arrayContaining([
      "colorTexture", "normalTexture", "trackIndex",
    ]));
  });

  it("linkGroups/set-exempt-fields replaces the per-group set", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId: 1, fields: ["lifetime", "gravity"] },
    });
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    expect(r.fields).toEqual(["lifetime", "gravity"]);
  });

  it("linkGroups/set-membership applies groupId to each id; -1 + single id auto-demotes to 0 (path 3); null clears", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1.
    // Flash (id=5) is unlinked. Add Flash to a new group via the
    // -1 sentinel — the resolution rule picks group 2 (smallest
    // unused positive), then `enforceSingleMemberLinkGroups`
    // fires immediately after the mutation and demotes Flash back to
    // 0 because group 2 has exactly one member. Net result: a -1 +
    // single-id dispatch is effectively a no-op (data layer matches
    // render layer, which already filters single-member groups).
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [5], groupId: -1 },
    });
    expect(lastTree).not.toBeNull();
    // Find Flash by walking the synthetic root.
    const flashAfter = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(flashAfter.linkGroup).toBe(0);

    // Clear Smoke + Sparks via null — both should drop to 0.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: null },
    });
    const smokeAfter  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparksAfter = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(smokeAfter.linkGroup).toBe(0);
    expect(sparksAfter.linkGroup).toBe(0);

    // Assign Flash + Smoke to an explicit group id — should land
    // exactly there with no scan/rewrite. Group 7 has 2 members so
    // the single-member enforcement doesn't fire.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 5], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 5)!.linkGroup).toBe(7);
    off();
  });

  // ─── Engine-side single-member link-group enforcement ─────
  //
  // Five tests covering the three mutation paths
  // that can leave a group with exactly one member, plus a regression
  // guard against over-eager demotion and an undo round-trip.

  it("path 1: leaving a 2-member group demotes BOTH members to 0", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1. Leave
    // Smoke via groupId=null — Sparks is now alone in group 1, so
    // the enforcement demotes Sparks too.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0], groupId: null },
    });
    expect(lastTree).not.toBeNull();
    const smoke  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(smoke.linkGroup).toBe(0);
    expect(sparks.linkGroup).toBe(0);
    off();
  });

  it("path 1b: joining a different group that shrinks the previous group demotes the lone survivor", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1. Flash
    // (id=5) is unlinked. Move Smoke into a 2-member-with-Flash group
    // (say groupId=7) via a single dispatch — leaves Sparks alone in
    // group 1, which the enforcement demotes.
    //
    // Use two dispatches to set this up clearly: first add Flash to
    // group 7 (creates singleton — gets demoted), then bulk-assign
    // [Smoke, Flash] to 7 (creates valid 2-member group AND shrinks
    // group 1 to {Sparks} → Sparks demoted).
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 5], groupId: 7 },
    });
    expect(lastTree).not.toBeNull();
    const smoke  = lastTree!.root.children.find((c) => c.id === 0)!;
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flash  = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(smoke.linkGroup).toBe(7);
    expect(flash.linkGroup).toBe(7);
    expect(sparks.linkGroup).toBe(0);  // demoted — was alone in group 1
    off();
  });

  it("path 2: deleting one member of a 2-member group demotes the survivor", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1.
    // Delete Smoke; Sparks is now alone in group 1 → demoted.
    await b.request({
      kind: "emitters/delete",
      params: { id: 0 },
    });
    expect(lastTree).not.toBeNull();
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(sparks.linkGroup).toBe(0);
    off();
  });

  it("regression guard: deleting from a 3-member group keeps the remaining 2 in their group", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // Build a 3-member group first: Smoke + Sparks + Flash all in
    // group 9. (Smoke + Sparks were already in group 1 — overwrite.)
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3, 5], groupId: 9 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(9);
    // Delete Smoke; Sparks + Flash still in group 9 (count = 2) — no
    // demotion should fire.
    await b.request({
      kind: "emitters/delete",
      params: { id: 0 },
    });
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flash  = lastTree!.root.children.find((c) => c.id === 5)!;
    expect(sparks.linkGroup).toBe(9);
    expect(flash.linkGroup).toBe(9);
    off();
  });

  it("idempotence: a no-op dispatch on an already-enforced tree changes nothing", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    // First dispatch: assign [Smoke, Sparks] to group 7 (already
    // sharing group 1, but explicit reassignment is fine — both end
    // at 7, group 7 has 2 members, no demotion). Confirms the
    // fixture's group 1 → group 7 reassignment doesn't accidentally
    // demote anyone.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 3)!.linkGroup).toBe(7);

    // Second dispatch: re-assign the same ids to the same group.
    // Enforcement runs again but the tree state is already correct →
    // no change.
    await b.request({
      kind: "linkGroups/set-membership",
      params: { ids: [0, 3], groupId: 7 },
    });
    expect(lastTree!.root.children.find((c) => c.id === 0)!.linkGroup).toBe(7);
    expect(lastTree!.root.children.find((c) => c.id === 3)!.linkGroup).toBe(7);
    off();
  });

  it("linkGroups/reset-exempt-fields drops the per-group entry back to defaults", async () => {
    const b = new MockBridge();
    await b.request({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId: 1, fields: ["lifetime"] },
    });
    await b.request({
      kind: "linkGroups/reset-exempt-fields",
      params: { groupId: 1 },
    });
    const r = await b.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    expect(r.fields).toEqual(expect.arrayContaining([
      "colorTexture", "normalTexture", "trackIndex",
    ]));
  });

  // ─── linkGroups/diff-membership (join-conflict preview) ──

  it("linkGroups/diff-membership returns the seeded conflicts for a real-group join", async () => {
    const b = new MockBridge();
    useMockLinkGroupConflicts.getState().setConflicts([
      { id: 4, fields: ["lifetime", "gravity"] },
    ]);
    const r = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: 1 },
    });
    expect(r.conflicts).toEqual([{ id: 4, fields: ["lifetime", "gravity"] }]);
  });

  it("linkGroups/diff-membership reports no conflicts when leaving (groupId null/0)", async () => {
    const b = new MockBridge();
    useMockLinkGroupConflicts.getState().setConflicts([
      { id: 4, fields: ["lifetime"] },
    ]);
    const left = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: null },
    });
    expect(left.conflicts).toEqual([]);
    const zero = await b.request({
      kind: "linkGroups/diff-membership",
      params: { ids: [4], groupId: 0 },
    });
    expect(zero.conflicts).toEqual([]);
  });
});
