// leave-particles document-mutation contract. Drives the production bridge,
// UndoStack, and snapshot paths against the live native host.

import { bridgeRequest } from "./helpers/bridge-request";
import { test, expect, type Page } from "./helpers/cdp";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

const state = () => bridgeRequest(page, { kind: "engine/state/snapshot", params: {} });
const setLeaveParticles = (enabled: boolean) =>
  bridgeRequest(page, { kind: "engine/set/leave-particles", params: { enabled } });
const undo = () =>
  bridgeRequest(page, { kind: "undo/perform", params: { direction: "undo" } });
const redo = () =>
  bridgeRequest(page, { kind: "undo/perform", params: { direction: "redo" } });

test("leave-particles undo/redo restores exact values while paused stays view-only", async () => {
  const pausedBefore = (await state()).paused;
  try {
    await bridgeRequest(page, { kind: "file/new", params: {} });
    const initial = await state();
    expect(initial.dirty).toBe(false);
    expect(initial.canUndo).toBe(false);

    const changedValue = !initial.leaveParticles;
    await setLeaveParticles(changedValue);
    const changed = await state();
    expect.soft(changed.leaveParticles).toBe(changedValue);
    expect.soft(changed.dirty).toBe(true);
    expect.soft(changed.canUndo).toBe(true);

    // Paused is a toolbar/view-only toggle. If undo capture is broadened to
    // engine/set/* instead of staying on this serialized field, this undo lands
    // on a duplicate changedValue snapshot instead of the exact initial value.
    await bridgeRequest(page, { kind: "engine/set/paused", params: { paused: !pausedBefore } });
    expect.soft(await undo()).toEqual({ applied: true });
    const undone = await state();
    expect.soft(undone.leaveParticles).toBe(initial.leaveParticles);
    expect.soft(undone.dirty).toBe(false);
    expect.soft(undone.paused).toBe(!pausedBefore);
    expect.soft(undone.canRedo).toBe(true);
    expect.soft(undone.canUndo).toBe(false);

    // A no-op while sitting on the redo branch must not truncate it.
    await setLeaveParticles(initial.leaveParticles);
    const afterUndoNoOp = await state();
    expect.soft(afterUndoNoOp.canUndo).toBe(false);
    expect.soft(afterUndoNoOp.canRedo).toBe(true);
    expect.soft(await redo()).toEqual({ applied: true });
    const redone = await state();
    expect.soft(redone.leaveParticles).toBe(changedValue);
    expect.soft(redone.dirty).toBe(true);
    expect.soft(redone.paused).toBe(!pausedBefore);
  } finally {
    await bridgeRequest(page, { kind: "engine/set/paused", params: { paused: pausedBefore } });
    await bridgeRequest(page, { kind: "file/new", params: {} });
  }
});

test("a same-value leave-particles request creates no undo entry or dirty state", async () => {
  try {
    // A fresh document is both the saved-state baseline and an empty undo
    // stack, which makes capture-before-compare overreach observable.
    await bridgeRequest(page, { kind: "file/new", params: {} });
    const initial = await state();
    await setLeaveParticles(initial.leaveParticles);

    const afterNoOp = await state();
    expect.soft(afterNoOp.leaveParticles).toBe(initial.leaveParticles);
    expect.soft(afterNoOp.dirty).toBe(false);
    expect.soft(afterNoOp.canUndo).toBe(false);
    expect.soft(await undo()).toEqual({ applied: false });
  } finally {
    await bridgeRequest(page, { kind: "file/new", params: {} });
  }
});
