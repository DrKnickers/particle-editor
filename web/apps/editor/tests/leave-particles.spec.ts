// leave-particles document-mutation contract. Drives the production bridge,
// UndoStack, and snapshot paths against the live native host.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as unknown as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

type BridgeReq = { kind: string; params: unknown };
type State = {
  leaveParticles: boolean;
  paused: boolean;
  dirty: boolean;
  canUndo: boolean;
  canRedo: boolean;
};

async function req<T = unknown>(kind: string, params: unknown = {}): Promise<T> {
  return page.evaluate(
    ({ kind, params }: BridgeReq) =>
      (window as unknown as {
        bridge: { request: (request: BridgeReq) => Promise<unknown> };
      }).bridge.request({ kind, params }),
    { kind, params } as BridgeReq,
  ) as Promise<T>;
}

const state = () => req<State>("engine/state/snapshot");
const setLeaveParticles = (enabled: boolean) =>
  req("engine/set/leave-particles", { enabled });
const undo = () =>
  req<{ applied: boolean }>("undo/perform", { direction: "undo" });
const redo = () =>
  req<{ applied: boolean }>("undo/perform", { direction: "redo" });

test("leave-particles undo/redo restores exact values while paused stays view-only", async () => {
  const pausedBefore = (await state()).paused;
  try {
    await req("file/new");
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
    await req("engine/set/paused", { paused: !pausedBefore });
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
    await req("engine/set/paused", { paused: pausedBefore });
    await req("file/new");
  }
});

test("a same-value leave-particles request creates no undo entry or dirty state", async () => {
  try {
    // A fresh document is both the saved-state baseline and an empty undo
    // stack, which makes capture-before-compare overreach observable.
    await req("file/new");
    const initial = await state();
    await setLeaveParticles(initial.leaveParticles);

    const afterNoOp = await state();
    expect.soft(afterNoOp.leaveParticles).toBe(initial.leaveParticles);
    expect.soft(afterNoOp.dirty).toBe(false);
    expect.soft(afterNoOp.canUndo).toBe(false);
    expect.soft(await undo()).toEqual({ applied: false });
  } finally {
    await req("file/new");
  }
});
