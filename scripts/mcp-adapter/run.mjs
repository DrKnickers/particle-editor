// [mcp-adapter] The one place the adapter starts a process: a
// deadline, a cap on what it may print, cancellation, and the whole process
// tree killed on any of them (observe.mjs starts the exe and ffmpeg).
import { spawn, spawnSync } from "node:child_process";

export const MAX_OUTPUT = 1 << 20;
export const EXIT_GRACE_MS = 2000;

function killTree(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  if (process.platform === "win32") spawnSync("taskkill.exe", ["/PID", String(child.pid), "/T", "/F"], { windowsHide: true });
  else child.kill("SIGKILL");
}

// Resolves { code, stdout, stderr, stopped }: `stopped` names why the adapter
// killed it ("timeout", "output", "cancelled"), or is null.
export function runCommand(file, args, { cwd, timeoutMs, maxOutput = MAX_OUTPUT, signal, spawnImpl = spawn } = {}) {
  return new Promise((done) => {
    const child = spawnImpl(file, args, { cwd, windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
    const out = { stdout: "", stderr: "" };
    let stopped = null, size = 0, finished = false;
    const stop = (why) => { if (!stopped) { stopped = why; killTree(child); } };
    const timer = setTimeout(() => stop("timeout"), timeoutMs);
    const onAbort = () => stop("cancelled");
    signal?.addEventListener("abort", onAbort);
    if (signal?.aborted) stop("cancelled");
    for (const stream of ["stdout", "stderr"]) {
      child[stream].setEncoding("utf8");
      child[stream].on("data", (text) => {
        size += Buffer.byteLength(text);
        if (size > maxOutput) return stop("output");
        out[stream] += text;
      });
    }
    const finish = (code) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      clearTimeout(grace);
      signal?.removeEventListener("abort", onAbort);
      child.stdout?.destroy();
      child.stderr?.destroy();
      done({ code, ...out, stopped });
    };
    // `close` waits for the pipes too, and a descendant that outlives the child
    // can hold them open forever. Once the child itself has exited, give the
    // pipes a moment to drain, then finish regardless.
    let grace;
    child.on("exit", (code) => { grace = setTimeout(() => finish(code ?? -1), stopped ? 0 : EXIT_GRACE_MS); });
    child.on("error", (error) => { out.stderr += error.message; finish(-1); });
    child.on("close", (code) => finish(code ?? -1));
  });
}
