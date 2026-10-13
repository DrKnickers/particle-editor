// The interactive timeline on the real exe (design section 11).
//
// The replay proof's E12 checks that timeline seeks are bit-identical to the
// proof's replays, but the proof drives the controller synchronously: it has
// no idle loop. This spec covers the interactive half through the bridge:
// the idle slot advancing play at 60 frames a second, pause, seeks, the
// session gate, a replay when an edit lands while parked (a camera move only
// re-bakes a non-weather frame), the user's view surviving the session, the
// end-of-session restore, and a page reload closing the session.

import { test, expect, type Page } from "./helpers/cdp";
import type { TimelineStateDto } from "@particle-editor/bridge-schema";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test.beforeEach(async () => {
  await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/end", params: {} });
    await b.request({ kind: "file/new", params: {} });
  });
});

test.afterAll(async () => {
  await page.evaluate(() => window.bridge!.request({ kind: "timeline/end", params: {} }));
});

async function state(): Promise<TimelineStateDto> {
  return page.evaluate(() => window.bridge!.request({ kind: "timeline/set", params: {} }));
}

test("begin opens at frame 0 and freezes the free-run clock; end restores it", async () => {
  const result = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "engine/set/paused", params: { paused: false } });
    const begin = await b.request({ kind: "timeline/begin", params: {} });
    const again = await b.request({ kind: "timeline/begin", params: {} });
    const during = await b.request({ kind: "engine/state/snapshot", params: {} });
    const live = await b.request({ kind: "engine/query/live-instances", params: {} });
    const end = await b.request({ kind: "timeline/end", params: {} });
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    const endAgain = await b.request({ kind: "timeline/end", params: {} });
    return { begin, again, duringPaused: during.paused, instances: live.instances, end, afterPaused: after.paused, endAgain };
  });
  expect(result.begin).toMatchObject({ active: true, frame: 0, status: "paused", maxFrames: 600 });
  expect(result.again).toEqual(result.begin);
  expect(result.duringPaused).toBe(true);
  expect(result.instances).toBe(1);
  expect(result.end).toMatchObject({ active: false, status: "closed" });
  expect(result.afterPaused).toBe(false);
  expect(result.endAgain).toEqual(result.end);
});

test("an empty document opens an empty session and still freezes the free-run clock", async () => {
  const result = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "emitters/delete", params: { id: 0 } });
    await b.request({ kind: "engine/set/paused", params: { paused: false } });
    const begin = await b.request({ kind: "timeline/begin", params: {} });
    const during = await b.request({ kind: "engine/state/snapshot", params: {} });
    const live = await b.request({ kind: "engine/query/live-instances", params: {} });
    const end = await b.request({ kind: "timeline/end", params: {} });
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { begin, duringPaused: during.paused, instances: live.instances, end, afterPaused: after.paused };
  });
  expect(result.begin).toMatchObject({ active: true, frame: 0, status: "empty" });
  expect(result.duringPaused).toBe(true);
  expect(result.instances).toBe(0);
  expect(result.end).toMatchObject({ active: false, status: "closed" });
  expect(result.afterPaused).toBe(false);
});

test("the session keeps the user's view: camera, ground, skydome, bloom and MSAA", async () => {
  const views = await page.evaluate(async () => {
    const b = window.bridge!;
    const view = async () => {
      const s = await b.request({ kind: "engine/state/snapshot", params: {} });
      const msaa = await b.request({ kind: "engine/query/msaa-levels", params: {} });
      return { camera: s.camera, ground: s.ground, skydomeSlot: s.skydomeSlot, skydomePrimaryName: s.skydomePrimaryName,
        skydomeSecondaryName: s.skydomeSecondaryName, bloom: s.bloom, msaa: msaa.current };
    };
    const before = await view();
    await b.request({ kind: "timeline/begin", params: {} });
    const begun = await view();
    await b.request({ kind: "timeline/seek", params: { frame: 120 } });
    const sought = await view();
    await b.request({ kind: "timeline/end", params: {} });
    return { before, begun, sought, ended: await view() };
  });
  expect(views.begun).toEqual(views.before);
  expect(views.sought).toEqual(views.before);
  expect(views.ended).toEqual(views.before);
});

test("the session gate refuses free-run transport, Clear and the spawner", async () => {
  const outcomes = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/begin", params: {} });
    const attempts: Array<[string, () => Promise<unknown>]> = [
      ["engine/set/paused", () => b.request({ kind: "engine/set/paused", params: { paused: false } })],
      ["engine/action/step-frames", () => b.request({ kind: "engine/action/step-frames", params: { frames: 1 } })],
      ["engine/action/clear", () => b.request({ kind: "engine/action/clear", params: {} })],
      // Refused before its params are read.
      ["spawner/start", () => b.request({ kind: "spawner/start", params: {} as never })],
      ["spawner/trigger", () => b.request({ kind: "spawner/trigger", params: {} })],
      ["spawner/stop", () => b.request({ kind: "spawner/stop", params: {} })],
    ];
    const results: Record<string, string> = {};
    for (const [kind, attempt] of attempts) {
      try { await attempt(); results[kind] = "ran"; }
      catch (error) { results[kind] = String((error as Error).message ?? error); }
    }
    const live = await b.request({ kind: "engine/query/live-instances", params: {} });
    return { results, instances: live.instances };
  });
  expect(Object.keys(outcomes.results)).toHaveLength(6);
  for (const [kind, outcome] of Object.entries(outcomes.results))
    expect(outcome, kind).toContain(`${kind} is unavailable while the timeline is open`);
  expect(outcomes.instances).toBe(1);
});

test("play advances about 60 frames a second, pause holds the frame, seeks land exactly", async () => {
  await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/begin", params: {} });
    (window as unknown as { timelineEvents: TimelineStateDto[] }).timelineEvents = [];
    b.on("timeline/state", (e) => (window as unknown as { timelineEvents: TimelineStateDto[] }).timelineEvents.push(e.payload));
    await b.request({ kind: "timeline/set", params: { playing: true } });
  });
  const started = Date.now();
  await page.waitForTimeout(1000);
  const playing = await state();
  const elapsed = (Date.now() - started) / 1000;
  expect(playing.status).toBe("playing");
  // Paced by wall time, capped catch-up: no burst, and no crawl.
  expect(playing.frame).toBeGreaterThan(45 * elapsed);
  expect(playing.frame).toBeLessThanOrEqual(Math.ceil(60 * elapsed) + 5);
  expect(playing.timeSeconds).toBeCloseTo(playing.frame / 60, 10);
  const events = await page.evaluate(() => (window as unknown as { timelineEvents: TimelineStateDto[] }).timelineEvents);
  expect(events.length).toBeGreaterThan(3);
  expect(events.every((e, i) => i === 0 || e.frame >= events[i - 1]!.frame)).toBe(true);

  // Pausing holds the played frame, and it is the frame a seek replays from a
  // reset: playing and seeking agree.
  const held = await page.evaluate(async () => {
    const b = window.bridge!;
    const paused = await b.request({ kind: "timeline/set", params: { playing: false } });
    const live = await b.request({ kind: "engine/query/live-instances", params: {} });
    return { paused, live };
  });
  await page.waitForTimeout(300);
  const later = await page.evaluate(async () => {
    const b = window.bridge!;
    const state = await b.request({ kind: "timeline/set", params: {} });
    const live = await b.request({ kind: "engine/query/live-instances", params: {} });
    return { state, live };
  });
  expect(later.state.frame).toBe(held.paused.frame);
  expect(later.live).toEqual(held.live);
  const reseek = await page.evaluate(async (f) => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/seek", params: { frame: f } });
    return b.request({ kind: "engine/query/live-instances", params: {} });
  }, held.paused.frame);
  expect(reseek).toEqual(held.live);

  for (const frame of [120, 7, 600, 0]) {
    const landed = await page.evaluate((f) => window.bridge!.request({ kind: "timeline/seek", params: { frame: f } }), frame);
    expect(landed).toMatchObject({ frame, timeSeconds: frame / 60, status: frame === 600 ? "ended" : "paused" });
  }
  const clamped = await page.evaluate(() => window.bridge!.request({ kind: "timeline/seek", params: { frame: 5000 } }));
  expect(clamped.frame).toBe(600);
  await expect(page.evaluate(() => window.bridge!.request({ kind: "timeline/seek", params: { frame: 1.5 } })))
    .rejects.toThrow(/frame must be an integer/);
});

test("play stops on the last frame without loop and wraps with loop", async () => {
  await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/begin", params: {} });
    await b.request({ kind: "timeline/seek", params: { frame: 590 } });
    await b.request({ kind: "timeline/set", params: { playing: true } });
  });
  await expect.poll(async () => (await state()).status, { timeout: 5000 }).toBe("ended");
  expect((await state()).frame).toBe(600);
  await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/seek", params: { frame: 590 } });
    await b.request({ kind: "timeline/set", params: { playing: true, loop: true } });
  });
  await expect.poll(async () => { const s = await state(); return s.playing && s.frame < 590; }, { timeout: 5000 }).toBe(true);
});

test("an edit while parked replays the frame as a new revision; a camera move only re-bakes it", async () => {
  const parked = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "timeline/begin", params: {} });
    return b.request({ kind: "timeline/seek", params: { frame: 90 } });
  });
  await page.evaluate(() => window.bridge!.request({
    kind: "emitters/set-properties", params: { id: 0, patch: { lifetime: 3 } },
  }));
  // The edit replays the frame; the web's estimate push that follows the
  // tree update is a second input change and may replay it again.
  await expect.poll(async () => (await state()).revision, { timeout: 3000 }).toBeGreaterThan(parked.revision);
  await page.waitForTimeout(300);
  const edited = await state();
  expect(edited.frame).toBe(90);
  expect(edited.revision).toBeLessThanOrEqual(parked.revision + 2);

  // The new document has no weather emitters: the camera is not a replay
  // input, so the frame stays put while its billboards re-bake.
  const before = await page.evaluate(() => window.bridge!.request({ kind: "engine/query/live-instances", params: {} }));
  await page.evaluate(async () => {
    const b = window.bridge!;
    const snapshot = await b.request({ kind: "engine/state/snapshot", params: {} });
    const camera = snapshot.camera;
    await b.request({ kind: "engine/set/camera", params: {
      ...camera, position: [camera.position[0] + 50, camera.position[1], camera.position[2]],
    } });
  });
  await page.waitForTimeout(300);
  const moved = await state();
  expect(moved.revision).toBe(edited.revision);
  expect(moved.frame).toBe(90);
  const after = await page.evaluate(() => window.bridge!.request({ kind: "engine/query/live-instances", params: {} }));
  expect(after).toEqual(before);

  const variation = await page.evaluate(() => window.bridge!.request({ kind: "timeline/new-variation", params: {} }));
  expect(variation.seed).not.toBe(parked.seed);
  expect(variation.frame).toBe(90);
});

test("a page reload closes the host's session", async () => {
  await page.evaluate(() => window.bridge!.request({ kind: "timeline/begin", params: {} }));
  await page.reload();
  await page.waitForFunction(() => !!window.bridge);
  await expect.poll(async () => page.evaluate(async () => {
    try { await window.bridge!.request({ kind: "timeline/set", params: {} }); return "open"; }
    catch (error) { return String((error as Error).message ?? error); }
  }), { timeout: 10_000 }).toContain("the timeline is not open");
});
