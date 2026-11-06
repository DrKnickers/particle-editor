// [mcp-adapter] Path confinement. Every path a tool takes is
// relative to the configured task root, and must stay inside it both as
// written and once links and junctions are resolved.
//
// Node and the exe must agree on what a name means. Win32 strips a trailing dot
// or space from a segment ("escape." opens "escape") and maps device names
// (CON, NUL, COM1) anywhere, where Node's fs keeps them as written, so such
// names are refused rather than interpreted.
import * as fs from "node:fs";
import { dirname, isAbsolute, relative, resolve, win32 } from "node:path";

const fail = (message) => Object.assign(new Error(message), { refused: true });
const DEVICE = /^(con|prn|aux|nul|com[0-9¹²³]|lpt[0-9¹²³]|conin\$|conout\$)(\..*)?$/i;
const FORBIDDEN = /[<>:"|?*\u0000-\u001f]/;

function inside(rootReal, real) {
  const rel = relative(rootReal, real);
  return rel === "" || !(rel === ".." || rel.startsWith("..\\") || rel.startsWith("../") || isAbsolute(rel));
}

// lstat, not exists: a dangling link is still there, and must not be walked past.
function present(path, io) {
  try { io.lstatSync(path); return true; } catch { return false; }
}

// The root itself, resolved once at startup: an existing folder, never a file.
export function taskRoot(path, io = fs) {
  if (typeof path !== "string" || !isAbsolute(path)) throw fail(`--root must be an absolute folder: ${path}`);
  if (!io.existsSync(path) || !io.statSync(path).isDirectory()) throw fail(`--root is not a folder: ${path}`);
  return io.realpathSync.native(path);
}

// `want` is "file" for an input that must exist, "output" for a file the call
// may create or replace, and "folder" for an output folder.
export function confine(rootReal, name, want, io = fs) {
  if (typeof name !== "string" || !name || isAbsolute(name) || win32.isAbsolute(name) ||
      name.split(/[\\/]/).some((part) => !part || part === "." || part === ".." || FORBIDDEN.test(part) ||
        /[. ]$/.test(part) || DEVICE.test(part)))
    throw fail(`not a task-root-relative path: ${JSON.stringify(name)}`);
  const path = resolve(rootReal, name);
  // The deepest part of the path that is there decides where a write would land.
  let existing = path;
  while (!present(existing, io)) existing = dirname(existing);
  let real;
  try { real = io.realpathSync.native(existing); } catch { throw fail(`path runs through a broken link: ${name}`); }
  if (!inside(rootReal, real)) throw fail(`path leaves the task root: ${name}`);
  const exists = existing === path;
  const kind = exists ? io.lstatSync(path) : null;
  if (want === "file" && !(exists && io.statSync(real).isFile())) throw fail(`no such file in the task root: ${name}`);
  if (want === "output" && exists && !kind.isFile()) throw fail(`output is not a plain file: ${name}`);
  if (want === "folder" && exists && !kind.isDirectory()) throw fail(`output folder is not a plain folder: ${name}`);
  // A second hard link may live outside the root, and nothing can tell which.
  if (want !== "folder" && exists && io.statSync(real).nlink > 1) throw fail(`file has more than one hard link: ${name}`);
  // A resolved existing path, or the resolved parent plus the new name.
  return exists ? real : resolve(real, relative(existing, path));
}
