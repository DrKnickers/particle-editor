import { test } from "node:test";
import assert from "node:assert/strict";
import { samplesFromBeats, samplesFromTimeline, frameIndexForMs, renderContactSheet } from "./contact-sheet.mjs";

test("samplesFromBeats covers open, every beat, walk/type endpoints, and the final frame", () => {
  const s = samplesFromBeats({
    durationMs: 20000,
    beats: [
      { at: 3000, do: "type", ref: "testid:emitter-name-input", text: "Core", expect: "name reads Core" },
      { at: 8000, do: "step", field: "tailSize", values: [55, 62, 70], stepMs: 400, hover: { ref: "testid:spinner-tail-length" } },
    ],
  });
  const labels = s.map((x) => x.label);
  assert.equal(s[0].ms, 0);
  assert.ok(labels.some((l) => l.includes('typed "Core"') && l.includes("expect: name reads Core")));
  assert.ok(labels.some((l) => l.includes("= 70 (walk done)")));
  assert.equal(s[s.length - 1].label, "final frame");
  // the walk-done sample lands after the last step: 8000 + 3*400
  assert.ok(s.some((x) => x.ms === 9200));
  // `expect` describes the RESULT: for delayed actions it must NOT caption the
  // beat-START sample (the state can't exist yet — typing begins at at+700)
  const startSample = s.find((x) => x.ms === 3000);
  assert.ok(!startSample.label.includes("expect:"));
});

test("samplesFromTimeline samples press down-edges and at-events, sorted", () => {
  const s = samplesFromTimeline({
    durationMs: 6000,
    tracks: [
      { at: 500, kind: "ui/focus-channel", params: { channel: "red" } },
      { cursor: [
        { t: 900, target: { kind: "element", ref: "channel-row:red" }, vis: true, press: false },
        { t: 1100, target: { kind: "element", ref: "channel-row:red" }, vis: true, press: true, activate: true },
        { t: 1200, target: { kind: "element", ref: "channel-row:red" }, vis: true, press: true },
        { t: 1250, target: { kind: "element", ref: "channel-row:red" }, vis: true, press: false },
      ] },
    ],
  });
  const pressSamples = s.filter((x) => x.label.startsWith("press"));
  assert.equal(pressSamples.length, 1); // down-EDGE only, not every pressed key
  assert.ok(pressSamples[0].label.includes("(activate)"));
  for (let i = 1; i < s.length; i++) assert.ok(s[i].ms >= s[i - 1].ms);
});

test("frameIndexForMs returns the CONTAINING frame (floor) and clamps", () => {
  assert.equal(frameIndexForMs(0, 60, 300), 0);
  assert.equal(frameIndexForMs(1000, 60, 300), 60);
  assert.equal(frameIndexForMs(999999, 60, 300), 299);
  assert.equal(frameIndexForMs(-5, 60, 300), 0);
  // half-frame boundary: ms=9 at 60fps is INSIDE frame 0 (frame 1 starts at ~16.7ms)
  assert.equal(frameIndexForMs(9, 60, 300), 0);
  assert.equal(frameIndexForMs(16, 60, 300), 0);
  assert.equal(frameIndexForMs(17, 60, 300), 1);
});

test("renderContactSheet emits one figure per unique frame+label and escapes captions", () => {
  const frames = Array.from({ length: 100 }, (_, i) => `frame_${String(i).padStart(5, "0")}.png`);
  const html = renderContactSheet(
    [
      { ms: 0, label: "open (frame 0)" },
      { ms: 0, label: "open (frame 0)" }, // dup — dropped
      { ms: 500, label: "click <b>bold</b>" },
    ],
    { frameFiles: frames, fps: 60, title: "demo", imgDirRel: null },
  );
  assert.equal((html.match(/<figure>/g) || []).length, 2);
  assert.ok(html.includes("frame_00030.png"));
  assert.ok(html.includes("click &lt;b&gt;bold&lt;/b&gt;")); // escaped, not injected
});
