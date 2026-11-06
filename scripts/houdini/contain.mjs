// [houdini] Process containment and the machine lock.
//
// runContained drives launcher.ps1, which holds the Job Object. The launcher's
// stdin is the kill switch: ending it (deadline, quota) or losing it (this
// process died) kills the whole tree. Nothing here kills by PID, so a reused
// PID can never be hit.
//
// The lock is a named-pipe server. The kernel refuses a second listener and
// frees the name when the holder dies, so there is no stale lock to clear.
import * as net from "node:net";
import { spawn } from "node:child_process";
import { dirname, join, win32 } from "node:path";
import { fileURLToPath } from "node:url";

export const LOCK_NAME = "\\\\.\\pipe\\pe-houdini-lock";
export const LAUNCHER = join(dirname(fileURLToPath(import.meta.url)), "launcher.ps1");
const powershell = (env = process.env) =>
  win32.join(env.SystemRoot ?? "C:\\Windows", "System32", "WindowsPowerShell", "v1.0", "powershell.exe");

// Resolves to { release } when held, or null when another process holds it.
export function acquireLock(name = LOCK_NAME, netImpl = net) {
  return new Promise((resolvePromise, reject) => {
    const server = netImpl.createServer((socket) => socket.destroy());
    server.once("error", (error) => (error.code === "EADDRINUSE" ? resolvePromise(null) : reject(error)));
    server.listen(name, () => resolvePromise({ release: () => new Promise((done) => server.close(() => done())) }));
  });
}

// The environment block Windows wants: "K=V" strings sorted case-insensitively.
export const environmentBlock = (env) =>
  Object.keys(env).sort((a, b) => (a.toUpperCase() < b.toUpperCase() ? -1 : a.toUpperCase() > b.toUpperCase() ? 1 : 0))
    .map((key) => `${key}=${env[key]}`);

// Resolves to { outcome, code, pid, detail }:
//   "exited"    the child exited with `code` (and the job was emptied)
//   "deadline"  timeoutMs passed; the tree was killed
//   "stopped"   watch() returned a reason (e.g. quota); the tree was killed
//   "failed"    the launcher could not start the child, or died
export function runContained({ exe, args, cwd, env, input = "", stdout, stderr }, {
  timeoutMs, watch = null, watchMs = 1000, onLaunched = () => {}, graceMs = 20000,
  spawnImpl = spawn, clock = { setTimeout, clearTimeout, setInterval, clearInterval }, launcher = LAUNCHER,
} = {}) {
  return new Promise((resolvePromise) => {
    const spec = { exe, args, cwd, env: environmentBlock(env), stdout, stderr, input };
    let child, outcome = null, code = null, pid = null, detail = "", buffered = "", grace = null;
    try {
      child = spawnImpl(powershell(), ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", launcher], { windowsHide: true, stdio: ["pipe", "pipe", "pipe"] });
    } catch (error) {
      resolvePromise({ outcome: "failed", code: null, pid: null, detail: `could not start the launcher: ${error.message}` });
      return;
    }
    const stop = (why, text = "") => {
      if (outcome) return;
      outcome = why; detail = text;
      child.stdin.end();
      // A launcher that ignores its stdin is killed; its job handle closes with it.
      grace = clock.setTimeout(() => child.kill(), graceMs);
    };
    const timer = clock.setTimeout(() => stop("deadline", `exceeded ${timeoutMs} ms`), timeoutMs);
    const poll = watch && clock.setInterval(() => {
      const reason = watch();
      if (reason) stop("stopped", reason);
    }, watchMs);
    child.stdin.on("error", () => {});
    child.stdin.write(`${Buffer.from(JSON.stringify(spec), "utf8").toString("base64")}\n`);
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      buffered += chunk;
      let at;
      while ((at = buffered.indexOf("\n")) >= 0) {
        const line = buffered.slice(0, at).trim();
        buffered = buffered.slice(at + 1);
        let m;
        if ((m = /^LAUNCHED (\d+)$/.exec(line))) { pid = Number(m[1]); onLaunched(pid); }
        else if ((m = /^EXITED (-?\d+)$/.exec(line))) { if (!outcome) outcome = "exited"; code = Number(m[1]); }
        else if ((m = /^ERROR (.*)$/.exec(line))) { if (!outcome) outcome = "failed"; detail = m[1]; }
      }
    });
    let launcherErr = "";
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => { if (launcherErr.length < 4096) launcherErr += chunk; });
    child.once("error", (error) => { if (!outcome) { outcome = "failed"; detail = error.message; } });
    child.once("close", () => {
      clock.clearTimeout(timer);
      if (grace) clock.clearTimeout(grace);
      if (poll) clock.clearInterval(poll);
      if (!outcome) { outcome = "failed"; detail = `launcher exited without a result ${launcherErr.trim()}`.trim(); }
      resolvePromise({ outcome, code, pid, detail });
    });
  });
}
