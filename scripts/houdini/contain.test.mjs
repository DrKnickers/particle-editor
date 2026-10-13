// [houdini] The Job Object launcher and the machine lock, run for real with
// system tools standing in for hython (plan §5 "Lock", risk 5).
//
// The tree is cmd -> ping, and ping holds an output file that cmd opened for
// it. Windows refuses to delete a file a live process holds, so "the file can
// be deleted" proves that every process in the tree is gone, grandchild
// included, without tracking any PID.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, existsSync } from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, win32 } from "node:path";
import { acquireLock, environmentBlock, runContained } from "./contain.mjs";

const windows = { skip: process.platform !== "win32" && "the launcher is Windows-only" };
const system32 = win32.join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const cmd = win32.join(system32, "cmd.exe");
const ENV = { SystemRoot: process.env.SystemRoot ?? "C:\\Windows", PATH: system32, MARKER: "only-this" };

const scratch = (t) => {
  const dir = mkdtempSync(join(tmpdir(), "pe-houdini-contain-"));
  t.after(() => rmSync(dir, { recursive: true, force: true, maxRetries: 20, retryDelay: 250 }));
  return dir;
};
const spec = (dir, command, extra = {}) => ({ exe: cmd, args: ["/d", "/c", command], cwd: dir, env: ENV,
  stdout: join(dir, "out.txt"), stderr: join(dir, "err.txt"), ...extra });
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const deletable = (path) => { try { rmSync(path); return true; } catch { return false; } };
async function until(condition, ms = 15000) {
  for (const started = Date.now(); Date.now() - started < ms; await sleep(100)) if (condition()) return true;
  return false;
}
// Start the cmd -> ping tree; resolves once ping holds `held.txt`.
const HOLD = "ping -n 120 127.0.0.1 > held.txt";
async function whenHeld(dir) {
  const held = join(dir, "held.txt");
  assert.ok(await until(() => existsSync(held) && !deletable(held)), "the tree never held held.txt");
  return held;
}

test("environment blocks are sorted case-insensitively", () => {
  assert.deepEqual(environmentBlock({ b: "2", A: "1", PATH: "p", Path2: "q", _x: "y" }),
    ["A=1", "b=2", "PATH=p", "Path2=q", "_x=y"]);
});

test("the child gets the input on stdin and its exit code is reported", windows, async (t) => {
  const dir = scratch(t);
  const result = await runContained(spec(dir, "sort & exit 3", { input: "b-line\r\na-line\r\n" }), { timeoutMs: 60000 });
  assert.equal(result.outcome, "exited");
  assert.equal(result.code, 3);
  assert.ok(result.pid > 0);
  assert.equal(readFileSync(join(dir, "out.txt"), "utf8"), "a-line\r\nb-line\r\n");
});

test("the child sees exactly the environment it is given", windows, async (t) => {
  const dir = scratch(t);
  const result = await runContained(spec(dir, "set"), { timeoutMs: 60000 });
  assert.equal(result.outcome, "exited");
  const names = readFileSync(join(dir, "out.txt"), "utf8").split(/\r?\n/).filter(Boolean).map((line) => line.split("=")[0]);
  // cmd adds COMSPEC, PATHEXT and PROMPT itself; nothing is inherited from this process.
  assert.deepEqual(names.sort(), ["COMSPEC", "MARKER", "PATH", "PATHEXT", "PROMPT", "SystemRoot"].sort());
});

test("a deadline kills the whole tree, grandchild included", windows, async (t) => {
  const dir = scratch(t);
  const running = runContained(spec(dir, HOLD), { timeoutMs: 6000 });
  const held = await whenHeld(dir);
  const result = await running;
  assert.equal(result.outcome, "deadline");
  assert.ok(deletable(held), "a process in the tree survived the deadline");
});

test("a watch that returns a reason (the stage quota) kills the tree", windows, async (t) => {
  const dir = scratch(t);
  let held = null;
  const result = await runContained(spec(dir, HOLD), {
    timeoutMs: 60000, watchMs: 200,
    watch: () => {
      if (!existsSync(join(dir, "held.txt"))) return null;
      held = join(dir, "held.txt");
      return "stage exceeded 1 MiB";
    },
  });
  assert.equal(result.outcome, "stopped");
  assert.equal(result.detail, "stage exceeded 1 MiB");
  assert.ok(held && deletable(held), "a process in the tree survived the stop");
});

test("killing the launcher alone (no /T) still kills the tree", windows, async (t) => {
  const dir = scratch(t);
  let launcher = null;
  const running = runContained(spec(dir, HOLD), {
    timeoutMs: 120000, spawnImpl: (...args) => (launcher = spawn(...args)),
  });
  const held = await whenHeld(dir);
  spawnSync(win32.join(system32, "taskkill.exe"), ["/F", "/PID", String(launcher.pid)], { windowsHide: true });
  const result = await running;
  assert.equal(result.outcome, "failed");
  assert.ok(await until(() => deletable(held)), "the tree outlived its launcher");
});

test("a child that can't start is a launch failure", windows, async (t) => {
  const dir = scratch(t);
  const result = await runContained(spec(dir, "", { exe: join(dir, "missing.exe"), args: [] }), { timeoutMs: 60000 });
  assert.equal(result.outcome, "failed");
  assert.match(result.detail, /cannot find|not find/i);
  assert.equal(result.pid, null);
});

const lockName = () => `\\\\.\\pipe\\pe-houdini-lock-test-${process.pid}-${Math.random().toString(16).slice(2)}`;

test("the lock refuses a second holder and frees on release", windows, async () => {
  const name = lockName();
  const first = await acquireLock(name);
  assert.ok(first);
  assert.equal(await acquireLock(name), null);
  await first.release();
  const again = await acquireLock(name);
  assert.ok(again);
  await again.release();
});

test("the lock frees when its holder is killed", windows, async () => {
  const name = lockName();
  const holder = spawn(process.execPath, ["--input-type=module", "-e",
    `import { acquireLock } from ${JSON.stringify(new URL("./contain.mjs", import.meta.url).href)};
     const held = await acquireLock(${JSON.stringify(name)}); console.log(held ? "HELD" : "BUSY"); setInterval(() => {}, 1000);`],
    { stdio: ["ignore", "pipe", "inherit"], windowsHide: true });
  const line = await new Promise((done) => holder.stdout.once("data", (data) => done(String(data).trim())));
  assert.equal(line, "HELD");
  assert.equal(await acquireLock(name), null, "another process's lock was not refused");
  holder.kill();
  await new Promise((done) => holder.once("close", done));
  const after = await acquireLock(name);
  assert.ok(after, "the lock outlived its holder");
  await after.release();
});
