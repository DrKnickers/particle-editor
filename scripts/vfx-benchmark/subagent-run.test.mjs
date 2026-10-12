import { test } from "node:test";
import assert from "node:assert/strict";
import { attemptPrompt, auditTranscript, schedule } from "./subagent-run.mjs";

const DIR = "C:\\runs\\run-1\\attempts\\0a1b2c3d";
const call = (name, input) => ({ type: "assistant", message: { content: [{ type: "tool_use", name, input }] } });

test("the audit passes a transcript that stays inside its attempt folder", () => {
  const lines = [
    { type: "user", message: { content: attemptPrompt(DIR) } },
    call("Read", { file_path: `${DIR}\\pack\\prompt.md` }),
    call("Read", { file_path: "C:/runs/run-1/attempts/0a1b2c3d/pack/frames/frame-00015.png" }),
    call("Glob", { pattern: "**/*.json", path: `${DIR}\\pack` }),
    call("Grep", { pattern: "lifetime", path: `${DIR}\\pack\\inspect.json` }),
    call("Write", { file_path: `${DIR}\\out\\findings.json`, content: "{}" }),
  ];
  assert.deepEqual(auditTranscript(lines, DIR), { calls: 5, violations: [], escapes: [] });
});

test("an escape: a path outside, a sibling attempt, a missing search path, an unreadable tool, a shell reaching out", () => {
  const e = (input) => auditTranscript([input], DIR).escapes;
  assert.match(e(call("Read", { file_path: "C:\\stores\\corpus-v1\\cases\\x\\answers.json" })).join(), /outside the attempt/);
  assert.match(e(call("Read", { file_path: "C:\\runs\\run-1\\attempts\\0a1b2c3d-other\\pack\\intent.md" })).join(), /outside the attempt/);
  assert.match(e(call("Read", { file_path: `${DIR}\\..\\ffffffff\\out\\findings.json` })).join(), /outside the attempt/);
  assert.match(e(call("Grep", { pattern: "answers" })).join(), /no path/);
  assert.match(e(call("Glob", { pattern: "C:/stores/**/answers.json", path: `${DIR}\\pack` })).join(), /outside the attempt/);
  assert.match(e(call("Agent", { prompt: "help" })).join(), /cannot be read/);
  assert.match(e(call("Bash", { command: "ls" })).join(), /reaches outside/);
  assert.match(e(call("Bash", { command: `cat "${DIR}/../ffffffff/out/findings.json"` })).join(), /reaches outside/);
  assert.match(e(call("Bash", { command: `cd "${DIR}" && cat pack/intent.md` })).join(), /reaches outside/);
  assert.match(e(call("Bash", { command: `head "${DIR}/pack/frames.jsonl"; cat C:/stores/x.json` })).join(), /reaches outside/);
});

test("a rule break inside the folder voids the attempt without being an escape", () => {
  // The run's one case: a shell command on the attempt's own telemetry.
  const bash = auditTranscript([call("Bash", { command: 'wc -l "C:/runs/run-1/attempts/0a1b2c3d/pack/frames.jsonl"; head -c 600 "C:/runs/run-1/attempts/0a1b2c3d/pack/frames.jsonl"' })], DIR);
  assert.deepEqual([bash.violations, bash.escapes], [["Bash is not an allowed tool"], []]);
  const write = auditTranscript([call("Write", { file_path: `${DIR}\\pack\\intent.md` })], DIR);
  assert.deepEqual([write.violations.length, write.escapes], [1, []]);
});

test("the schedule: three primary attempts per case, one bundle-only per eligible case and control", () => {
  const corpus = { attempts: 3, cases: [{ id: "a", kind: "defect", secondaryEligible: true }, { id: "b", kind: "defect", secondaryEligible: false },
    { id: "c", kind: "control", shape: "single" }] };
  const s = schedule(corpus);
  assert.equal(s.filter((x) => x.condition === "primary").length, 9);
  assert.deepEqual(s.filter((x) => x.condition === "bundle-only").map((x) => x.caseId), ["a", "c"]);
  // A draw-order case is the frames-alone test of order: three bundle-only attempts.
  const order = schedule({ attempts: 3, cases: [{ id: "d", kind: "defect", class: "draw-order", secondaryEligible: true }] });
  assert.deepEqual(order.filter((x) => x.condition === "bundle-only").map((x) => x.attempt), [1, 2, 3]);
});
