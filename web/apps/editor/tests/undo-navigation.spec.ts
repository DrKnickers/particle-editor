// Undo navigation contract specs.
//
// Regression coverage for the head-of-history auto-capture in
// BridgeDispatcher's `undo/perform`. The auto-cap exists because the
// new-UI captures undo snapshots PRE-mutation (legacy captured POST),
// so the live state sits one step ahead of the stack tip after a fresh
// edit and must be snapshotted before the first Undo() steps back.
//
// BUG (this spec): the auto-cap condition was `Cursor() == Depth()`,
// which is ALSO true immediately after a Redo() (redo to the tip leaves
// cursor == size). After a redo the live state is already in sync with
// the tip, so the auto-cap was spurious — it captured a duplicate and
// the following Undo() returned that duplicate, silently swallowing the
// undo. User-visible: undo → redo → undo loses the second undo.
//
// Talks to the host's real ParticleSystem + UndoStack via window.bridge
// (no mocks) over the --test-host CDP endpoint.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  // Pick the page that actually has window.bridge (skip DevTools targets).
  let found: Page | null = null;
  for (const p of pages) {
    try {
      if (await p.evaluate(() => typeof (window as { bridge?: unknown }).bridge !== "undefined")) {
        found = p;
        break;
      }
    } catch {
      /* page not evaluable (e.g. devtools) — skip */
    }
  }
  page = found ?? pages[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

// Property-edit coalescing is time-windowed (UndoStack COALESCE_WINDOW_MS =
// 1500ms). Wait out the window before each test so the first edit always
// starts a fresh undo entry rather than folding into a prior test's
// same-emitter edit — makes the time-dependent behaviour deterministic.
test.beforeEach(async () => {
  await page.waitForTimeout(1600);
});

// Bridge helpers — all run inside the page against the real host.
type BridgeReq = { kind: string; params: unknown };
async function req<T = unknown>(kind: string, params: unknown = {}): Promise<T> {
  return page.evaluate(
    ({ kind, params }: BridgeReq) =>
      (window as unknown as { bridge: { request: (r: BridgeReq) => Promise<unknown> } }).bridge.request({ kind, params }),
    { kind, params } as BridgeReq,
  ) as Promise<T>;
}
async function firstEmitterId(): Promise<number> {
  const list = await req<{ root: { children: { id: number }[] } }>("emitters/list");
  const id = list.root.children[0]?.id;
  if (id === undefined) throw new Error("no emitters in tree");
  return id;
}
async function getProps(id: number): Promise<{ lifetime: number; gravity: number }> {
  const r = await req<{ properties: { lifetime: number; gravity: number } }>(
    "emitters/get-properties",
    { id },
  );
  return r.properties;
}
async function getLifetime(id: number): Promise<number> {
  return (await getProps(id)).lifetime;
}
const setLifetime = (id: number, v: number) =>
  req("emitters/set-properties", { id, patch: { lifetime: v } });
const setProp = (id: number, patch: Record<string, number>) =>
  req("emitters/set-properties", { id, patch });
const undo = () => req<{ applied: boolean }>("undo/perform", { direction: "undo" });
const redo = () => req<{ applied: boolean }>("undo/perform", { direction: "redo" });
type UndoBudgetState = {
  maxTotalBytes: number;
  totalBytes: number;
  depth: number;
  cursor: number;
};
const queryUndoBudget = () => req<UndoBudgetState>("undo/test/budget");
const setUndoBudget = (maxTotalBytes: number) =>
  req<UndoBudgetState>("undo/test/budget", { maxTotalBytes });

test("production auto-cap preserves the PRE + LIVE pair at the byte frontier", async () => {
  // Isolate the host-owned production stack, then query its ACTUAL configured
  // member before lowering it. This catches both a changed default constant and
  // HostWindow bypassing that default at construction.
  await req("file/new");
  const original = await queryUndoBudget();

  try {
    expect(original.maxTotalBytes).toBe(256 * 1024 * 1024);
    expect(original).toMatchObject({ totalBytes: 0, depth: 0, cursor: 0 });

    const limited = await setUndoBudget(1);
    expect(limited).toMatchObject({
      maxTotalBytes: 1,
      totalBytes: 0,
      depth: 0,
      cursor: 0,
    });

    const id = await firstEmitterId();
    await req("emitters/select", { id });
    const p0 = await getLifetime(id);
    const target = Number((p0 + 3).toFixed(3));

    // The normal PRE capture keeps one over-budget entry. Undo then takes the
    // real production missing-LIVE branch, whose scoped policy must retain PRE
    // while it captures LIVE.
    await setLifetime(id, target);
    expect(await getLifetime(id)).toBeCloseTo(target, 4);
    let state = await queryUndoBudget();
    expect(state.depth).toBe(1);
    expect(state.cursor).toBe(1);
    expect(state.totalBytes).toBeGreaterThan(state.maxTotalBytes);

    // The test override itself is guarded against altering a live history.
    await expect(setUndoBudget(2)).rejects.toThrow(
      "stack must be empty and synchronized",
    );

    expect(await undo()).toEqual({ applied: true });
    expect(await getLifetime(id)).toBeCloseTo(p0, 4);
    state = await queryUndoBudget();
    expect(state.depth).toBe(2);
    expect(state.cursor).toBe(1);
    expect(state.totalBytes).toBeGreaterThan(state.maxTotalBytes);

    expect(await redo()).toEqual({ applied: true });
    expect(await getLifetime(id)).toBeCloseTo(target, 4);
    state = await queryUndoBudget();
    expect(state.depth).toBe(2);
    expect(state.cursor).toBe(2);
  } finally {
    // file/new clears history before both reconfiguration and handoff. The
    // safe-integer guard keeps cleanup sane under the intentional unbounded-
    // default mutant, whose precondition assertion fails before configuration.
    await req("file/new");
    if (Number.isSafeInteger(original.maxTotalBytes)) {
      await setUndoBudget(original.maxTotalBytes);
    }
    await req("file/new");
  }
});

test("a single edit undoes and redoes (auto-cap round-trip)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);
  const target = Number((p0 + 3).toFixed(3));

  await setLifetime(id, target);
  expect(await getLifetime(id)).toBeCloseTo(target, 4);

  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);

  await redo();
  expect(await getLifetime(id)).toBeCloseTo(target, 4);

  // restore
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);
});

test("undo after a redo steps back to the pre-edit state (no spurious auto-cap)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);
  const target = Number((p0 + 7).toFixed(3));

  await setLifetime(id, target); // edit
  await undo();                  // -> p0
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);

  await redo();                  // -> target (cursor back at tip)
  expect(await getLifetime(id)).toBeCloseTo(target, 4);

  // THE REGRESSION: this undo must return to p0, not stay at target.
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);
});

test("a full undo/redo/undo cycle is stable across repeats", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);
  const target = Number((p0 + 2).toFixed(3));

  await setLifetime(id, target);
  for (let i = 0; i < 3; i++) {
    await undo();
    expect(await getLifetime(id)).toBeCloseTo(p0, 4);
    await redo();
    expect(await getLifetime(id)).toBeCloseTo(target, 4);
  }
  // leave at the pre-edit value
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);
});

test("a rapid burst of same-emitter edits coalesces into ONE undo step (wheel scroll)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);

  // Simulate a scroll-wheel gesture: 4 rapid edits to the same field. Each is
  // a separate emitters/set-properties (one per wheel notch), all landing
  // inside the coalesce window.
  for (let i = 1; i <= 4; i++) await setLifetime(id, Number((p0 + i).toFixed(3)));
  expect(await getLifetime(id)).toBeCloseTo(p0 + 4, 4);

  // ONE undo must revert the WHOLE burst — not just the last tick.
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);

  // ONE redo must reapply the whole burst.
  await redo();
  expect(await getLifetime(id)).toBeCloseTo(p0 + 4, 4);

  // restore
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);
});

test("rapid edits to DIFFERENT fields are SEPARATE undo steps (per-field coalescing)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const { lifetime: lt0, gravity: gv0 } = await getProps(id);
  const lt1 = Number((lt0 + 3).toFixed(3));
  const gv1 = Number((gv0 + 2).toFixed(3));

  // Two rapid edits to DIFFERENT fields on the same emitter, within the
  // coalesce window. Per-FIELD coalescing keeps these as separate steps;
  // per-emitter coalescing (the prior behaviour) would fold them into one.
  await setProp(id, { lifetime: lt1 });
  await setProp(id, { gravity: gv1 });
  expect((await getProps(id)).lifetime).toBeCloseTo(lt1, 4);
  expect((await getProps(id)).gravity).toBeCloseTo(gv1, 4);

  // ONE undo reverts only the LAST field (gravity); lifetime is untouched.
  await undo();
  let p = await getProps(id);
  expect(p.gravity).toBeCloseTo(gv0, 4);
  expect(p.lifetime).toBeCloseTo(lt1, 4);

  // A SECOND undo reverts the earlier field (lifetime).
  await undo();
  p = await getProps(id);
  expect(p.lifetime).toBeCloseTo(lt0, 4);
  expect(p.gravity).toBeCloseTo(gv0, 4);
});

test("a same-field burst still coalesces under per-field keying", async () => {
  // The per-field key must stay stable across ticks of one field.
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);
  for (let i = 1; i <= 3; i++) await setLifetime(id, Number((p0 + i).toFixed(3)));
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4); // one undo reverts all 3
});

// ── Follow-up: streaming track-key undo coalescing ──────────────
//
// The host's emitters/set-track-key folds rapid same-track/same-emitter
// edits (a wheel/hold-arrow/scrub Value or Time key spinner, plus a
// multi-key group shift's N per-key calls) into ONE undo step within the
// 1500ms window — mirroring the emitter-property per-field coalescing above.
// Per-TRACK keying (legacy's track<<16|emitterIdx). Fixture: --test-host
// boots a default system whose every track has border keys at t=0 and t=100
// (ParticleSystem.cpp:824); we move the distinct, non-aliased `scale` (idx 4,
// default 20) and `rotationSpeed` (idx 6, default 0) tracks — never the
// Green/Blue/Alpha aliases of Red.

type TrackKey = { time: number; value: number };
async function getTrackKeys(id: number, trackName: string): Promise<TrackKey[]> {
  const r = await req<{ tracks: { name: string; keys: TrackKey[] }[] }>(
    "emitters/get-tracks",
    { id },
  );
  return r.tracks.find((t) => t.name === trackName)?.keys ?? [];
}
async function getTrackKeyValue(id: number, trackName: string, time: number): Promise<number> {
  const k = (await getTrackKeys(id, trackName)).find((x) => Math.abs(x.time - time) < 1e-3);
  if (k === undefined) throw new Error(`no ${trackName} key near t=${time}`);
  return k.value;
}
// Value-only move (newTime == oldTime): mirrors handleValueSpinner, keeps
// oldTime stable across ticks. time defaults to the t=0 border key.
const setTrackKeyValue = (id: number, track: string, time: number, newValue: number) =>
  req("emitters/set-track-key", { id, track, oldTime: time, newTime: time, newValue });
const addTrackKey = (id: number, track: string, time: number, value: number) =>
  req("emitters/add-track-key", { id, track, time, value });

test("a rapid value-spinner burst on ONE track key coalesces into ONE undo step", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const v0 = await getTrackKeyValue(id, "scale", 0);

  // 4 rapid value moves (one per wheel notch), same track + emitter, in-window.
  for (let i = 1; i <= 4; i++) await setTrackKeyValue(id, "scale", 0, v0 + i);
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(v0 + 4, 3);

  // ONE undo must revert the WHOLE burst — not just the last tick.
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(v0, 3);

  // ONE redo reapplies the whole burst; leave the fixture at baseline.
  await redo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(v0 + 4, 3);
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(v0, 3);
});

test("value edits to DIFFERENT tracks are SEPARATE undo steps (per-track keying)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const s0 = await getTrackKeyValue(id, "scale", 0);
  const r0 = await getTrackKeyValue(id, "rotationSpeed", 0);

  await setTrackKeyValue(id, "scale", 0, s0 + 5);
  await setTrackKeyValue(id, "rotationSpeed", 0, r0 + 5);
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 5, 3);
  expect(await getTrackKeyValue(id, "rotationSpeed", 0)).toBeCloseTo(r0 + 5, 3);

  // ONE undo reverts only the LAST track (rotationSpeed); scale untouched.
  await undo();
  expect(await getTrackKeyValue(id, "rotationSpeed", 0)).toBeCloseTo(r0, 3);
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 5, 3);

  // SECOND undo reverts the earlier track (scale) — back to baseline.
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);
});

test("a structural add-track-key between two value edits breaks the fold", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const s0 = await getTrackKeyValue(id, "scale", 0);

  await setTrackKeyValue(id, "scale", 0, s0 + 2);  // edit 1
  await addTrackKey(id, "scale", 50, 30);          // structural (key=0, never folds)
  await setTrackKeyValue(id, "scale", 0, s0 + 4);  // edit 2
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 4, 3);

  // Undo edit 2 -> s0+2; the added key is still present.
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 2, 3);

  // Undo the structural add -> the t=50 key is gone.
  await undo();
  expect((await getTrackKeys(id, "scale")).find((k) => Math.abs(k.time - 50) < 1e-3))
    .toBeUndefined();

  // Undo edit 1 -> baseline.
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);
});

test("a same-track edit after an undo PUSHES (no mid-redo-branch coalesce)", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const s0 = await getTrackKeyValue(id, "scale", 0);

  await setTrackKeyValue(id, "scale", 0, s0 + 3);  // edit A
  await undo();                                     // -> s0 (cursor below tip)
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);

  await setTrackKeyValue(id, "scale", 0, s0 + 8);  // edit B must PUSH, truncating redo
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 8, 3);

  await redo();                                     // branch truncated -> no-op
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 8, 3);

  await undo();                                     // reverts B -> s0
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);
});

test("two same-track edits MORE than the window apart are SEPARATE undo steps", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const s0 = await getTrackKeyValue(id, "scale", 0);

  await setTrackKeyValue(id, "scale", 0, s0 + 3);  // edit 1
  await page.waitForTimeout(1600);                 // exceed COALESCE_WINDOW_MS (1500)
  await setTrackKeyValue(id, "scale", 0, s0 + 6);  // edit 2 — window expired -> new step

  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0 + 3, 3);
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);
});

// ── engine/action/rescale-system undo boundary ──────────────────────────────
//
// 2026-07 audit V-2. `rescale-system` mutated every emitter in the system
// without a pre-mutation captureUndo(), while its sibling `rescale-emitter`
// forty lines below has always called one. The handler carried a comment
// excusing the omission as "a no-op until the broader capture wiring lands" —
// wiring that HAD landed, so the comment read as an accepted limitation and
// every reader skipped past it.
//
// The failure is not "undo does nothing", which is why it stayed invisible:
// `undo/perform`'s head-of-history auto-cap still fires, so a single Ctrl+Z
// after a rescale DOES change something — it reverts straight past the rescale
// to whatever the last genuinely-captured entry was, silently discarding the
// user's previous edit as well. Hence the seeded edit below: it is what
// separates "the rescale reverted" from "the rescale AND everything before it
// reverted".
test("a whole-system rescale is its own undo step and does not swallow the prior edit", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const p0 = await getLifetime(id);

  // Seed a distinct, separately-captured undo entry.
  const seeded = Number((p0 + 5).toFixed(3));
  await setLifetime(id, seeded);
  expect(await getLifetime(id)).toBeCloseTo(seeded, 4);
  await page.waitForTimeout(1600);   // exceed COALESCE_WINDOW_MS so the rescale can't fold in

  // DoRescaleEmitter does `emitter->lifetime *= timeScale` (src/Rescale.cpp:23),
  // so a 200% duration scale doubles it. sizeScale stays 1.0 to keep the
  // assertion on a single axis.
  await req("engine/action/rescale-system", {
    durationScalePercent: 200,
    sizeScalePercent: 100,
  });
  expect(await getLifetime(id)).toBeCloseTo(seeded * 2, 3);

  // THE REGRESSION: without captureUndo() in rescale-system this lands on p0,
  // having thrown away the seeded edit along with the rescale.
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(seeded, 3);

  // Restore, so the shared native host isn't left rescaled for later specs.
  await undo();
  expect(await getLifetime(id)).toBeCloseTo(p0, 4);
});

// ── batched-gesture undo boundaries ─────────────────────────────────────────
//
// 2026-07 audit C-006 + C-007. Both were the same shape: the React layer fanned
// ONE user gesture out into N bridge requests, and each native handler captured
// its own undo entry, so a single Ctrl+Z undid a fraction of one gesture.
// `emitters/cut` has always been the correct pattern — one captureUndo() around
// its whole delete loop — and these two specs pin the same property onto the
// two handlers that now batch: `emitters/delete-many` and
// `emitters/add-track-keys`.
//
// Both assert the SPECIFIC surviving state, not merely "something changed":
// with a per-item capture the undo still does something (the head-of-history
// auto-cap fires), it just does the wrong amount, so a "state differs" oracle
// passes with and without the fix.

async function rootIds(): Promise<number[]> {
  const list = await req<{ root: { children: { id: number }[] } }>("emitters/list");
  return list.root.children.map((c) => c.id);
}

test("a multi-root delete is ONE undo step (every deleted root returns together)", async () => {
  const before = await rootIds();

  // Three fresh roots. Each add-root is its OWN undo entry — which is the
  // point: the delete gesture must not inherit that per-item granularity.
  const added: number[] = [];
  for (let i = 0; i < 3; i++) {
    const r = await req<{ newId: number }>("emitters/add-root", {});
    added.push(r.newId);
  }
  expect(await rootIds()).toHaveLength(before.length + 3);
  await page.waitForTimeout(1600); // clear the coalesce window

  // The multi-select delete gesture. performDelete sorts descending before
  // sending, because an emitter id is a POSITION that shifts as siblings vanish.
  await req("emitters/delete-many", { ids: [...added].sort((a, b) => b - a) });
  expect(await rootIds()).toEqual(before);

  // THE REGRESSION: with a captureUndo() per deleted root, ONE undo brings back
  // exactly ONE root (before.length + 1), leaving the user to press Ctrl+Z
  // three times to reverse a single gesture.
  await undo();
  expect(await rootIds()).toHaveLength(before.length + 3);

  // Restore the shared host for later specs: re-delete in one step.
  await req("emitters/delete-many", { ids: [...added].sort((a, b) => b - a) });
  expect(await rootIds()).toEqual(before);
});

test("a multi-key curve paste is ONE undo step and does not swallow the prior edit", async () => {
  const id = await firstEmitterId();
  await req("emitters/select", { id });
  const s0 = await getTrackKeyValue(id, "scale", 0);

  // Seed a distinct, separately-captured entry. Without it the assertion could
  // not tell "the paste reverted" from "the paste AND the edit before it
  // reverted" — the same discriminator the rescale spec above needs.
  const seeded = s0 + 5;
  await setTrackKeyValue(id, "scale", 0, seeded);
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(seeded, 3);
  await page.waitForTimeout(1600); // exceed COALESCE_WINDOW_MS

  // Ctrl+V of a three-key clipboard onto the focus track. Interior times only
  // (0 and 100 are the border keys).
  const pasted = [
    { time: 20, value: 11 },
    { time: 40, value: 22 },
    { time: 60, value: 33 },
  ];
  await req("emitters/add-track-keys", { id, track: "scale", keys: pasted });
  for (const k of pasted) {
    expect(await getTrackKeyValue(id, "scale", k.time)).toBeCloseTo(k.value, 3);
  }

  // THE REGRESSION: one captureUndo() per key leaves the first two pasted keys
  // behind after a single undo.
  await undo();
  const times = (await getTrackKeys(id, "scale")).map((k) => k.time);
  for (const k of pasted) {
    expect(times.find((t) => Math.abs(t - k.time) < 1e-3)).toBeUndefined();
  }
  // ...and the seeded edit must SURVIVE — the undo reverses the paste, not the
  // paste plus whatever came before it.
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(seeded, 3);

  // Restore baseline for later specs.
  await undo();
  expect(await getTrackKeyValue(id, "scale", 0)).toBeCloseTo(s0, 3);
});
