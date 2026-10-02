// Guard: every host->UI bridge envelope is serialized in ONE place.
//
// Emitter names are raw .alo bytes, and nlohmann's default
// dump() throws type_error 316 on invalid UTF-8. The bridge had ~40 independent
// `m_emit(env.dump())` / `res.dump()` sites, so one emitter named "\xE9" threw
// through a WebView2 COM callback. The fix made the chokepoint structural:
// BridgeDispatcher::EmitFn takes `const nlohmann::json&`, and the emit lambda,
// DispatchSync and HostBridgeProxy serialize through
// host::SerializeBridgeEnvelope (src/host/BridgeWire.h), which replaces bad
// bytes instead of throwing. Fixing two of 42 sites would have been the easy
// regression, so this test keeps a new site from quietly reintroducing it:
//
//   1. EmitFn takes JSON, not text.
//   2. No `m_emit(` call passes a string (a `.dump()`, a literal, a
//      std::string, or an already-serialized envelope).
//   3. The dispatch translation units and HostBridgeProxy never call `.dump(`
//      themselves — they go through SerializeBridgeEnvelope.
//   4. No PostWebMessageAs* argument is built with a bare `.dump(`.
//   5. SerializeBridgeEnvelope really uses error_handler_t::replace.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { basename, dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const hostDir = join(repoRoot, "src", "host");
const EXCLUDED_DIRS = new Set(["third_party", "spike", "generated"]);
const EXCLUDED_FILES = new Set(["viewport_poc.cpp"]);

// Blank out comments and string/char literal CONTENTS (keeping the quotes and
// every newline), so a comment that quotes `m_emit(env.dump())` or a URL in a
// string can neither trip nor hide a match. Raw strings R"d( ... )d" included.
export function stripCommentsAndStrings(src) {
  let out = "";
  let i = 0;
  const n = src.length;
  const blank = (s) => s.replace(/[^\n]/g, " ");
  while (i < n) {
    const c = src[i];
    const next = src[i + 1];
    if (c === "/" && next === "/") {
      const end = src.indexOf("\n", i);
      const stop = end === -1 ? n : end;
      out += blank(src.slice(i, stop));
      i = stop;
    } else if (c === "/" && next === "*") {
      const end = src.indexOf("*/", i + 2);
      const stop = end === -1 ? n : end + 2;
      out += blank(src.slice(i, stop));
      i = stop;
    } else if (c === "R" && next === '"' && !/[A-Za-z0-9_]/.test(src[i - 1] ?? "")) {
      const open = src.indexOf("(", i + 2);
      const delim = src.slice(i + 2, open);
      const close = src.indexOf(`)${delim}"`, open + 1);
      const stop = close === -1 ? n : close + delim.length + 2;
      out += 'R"' + blank(src.slice(i + 2, stop - 1)) + '"';
      i = stop;
    } else if (c === "'" && /[0-9A-Fa-f]/.test(src[i - 1] ?? "") && /[0-9A-Fa-f]/.test(next ?? "")) {
      out += c; // C++14 digit separator (15'000), not a char literal
      i += 1;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && src[j] !== c && src[j] !== "\n") j += src[j] === "\\" ? 2 : 1;
      out += c + blank(src.slice(i + 1, j)) + (src[j] === c ? c : "");
      i = src[j] === c ? j + 1 : j;
    } else {
      out += c;
      i += 1;
    }
  }
  return out;
}

// Every `name(` call in already-stripped code, with its balanced argument text.
export function callArguments(code, name) {
  const calls = [];
  const re = new RegExp(`\\b${name}\\s*\\(`, "g");
  for (const m of code.matchAll(re)) {
    let depth = 1;
    let j = m.index + m[0].length;
    const start = j;
    while (j < code.length && depth > 0) {
      if (code[j] === "(") depth += 1;
      else if (code[j] === ")") depth -= 1;
      j += 1;
    }
    const line = code.slice(0, m.index).split("\n").length;
    calls.push({ line, arg: code.slice(start, j - 1) });
  }
  return calls;
}

// An `m_emit(` argument that is already text rather than a JSON envelope.
export function isStringArgument(arg) {
  return (
    /\.dump\s*\(/.test(arg) ||
    /^\s*(u8|L|u|U)?R?"/.test(arg) ||
    /\bstd::w?string\b/.test(arg) ||
    /\.c_str\s*\(|\.str\s*\(/.test(arg) ||
    /\bSerializeBridgeEnvelope\s*\(/.test(arg)
  );
}

function hostSources(dir = hostDir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, e.name);
    if (e.isDirectory()) {
      if (!EXCLUDED_DIRS.has(e.name)) out.push(...hostSources(full));
    } else if (/\.(cpp|h)$/i.test(e.name) && !EXCLUDED_FILES.has(e.name)) {
      out.push(full);
    }
  }
  return out;
}

const rel = (file) => relative(repoRoot, file).split(sep).join("/");
const sources = hostSources().map((file) => ({
  file,
  code: stripCommentsAndStrings(readFileSync(file, "utf8")),
}));

test("the scanner itself flags the regression shapes it exists to catch", () => {
  const code = stripCommentsAndStrings(
    [
      "m_emit(env.dump());",
      'm_emit(R"({"type":"evt"})");',
      "m_emit(std::string(buf));",
      "m_emit(SerializeBridgeEnvelope(env));",
      "m_emit(env);                       // m_emit(env.dump()) in a comment",
      'Log("m_emit(x.dump())");',
    ].join("\n"),
  );
  const flagged = callArguments(code, "m_emit").filter((c) => isStringArgument(c.arg));
  assert.deepEqual(flagged.map((c) => c.line), [1, 2, 3, 4]);
});

test("BridgeDispatcher::EmitFn takes a JSON envelope, not text", () => {
  const header = sources.find((s) => basename(s.file) === "BridgeDispatcher.h");
  assert.ok(header, "src/host/BridgeDispatcher.h not found");
  assert.match(
    header.code,
    /using\s+EmitFn\s*=\s*std::function<\s*void\s*\(\s*const\s+nlohmann::json\s*&/,
    "EmitFn must be std::function<void(const nlohmann::json&)> so no emit site serializes on its own",
  );
});

test("no m_emit( call passes a string", () => {
  const offenders = [];
  for (const { file, code } of sources) {
    for (const c of callArguments(code, "m_emit")) {
      if (isStringArgument(c.arg)) offenders.push(`${rel(file)}:${c.line}  m_emit(${c.arg.trim()})`);
    }
  }
  assert.deepEqual(offenders, [], "pass the json envelope itself; the emit lambda serializes it");
});

test("dispatch TUs and HostBridgeProxy serialize only through SerializeBridgeEnvelope", () => {
  const offenders = [];
  for (const { file, code } of sources) {
    const name = basename(file);
    if (!/^BridgeDispatch(er|_\w+)\.cpp$/.test(name) && name !== "HostBridgeProxy.cpp") continue;
    code.split("\n").forEach((text, idx) => {
      if (/\.dump\s*\(/.test(text)) offenders.push(`${rel(file)}:${idx + 1}`);
    });
  }
  assert.deepEqual(offenders, [], "a bare .dump() throws on non-UTF-8 names; use host::SerializeBridgeEnvelope");
});

test("no PostWebMessageAs* argument is serialized with a bare .dump()", () => {
  const offenders = [];
  for (const { file, code } of sources) {
    for (const name of ["PostWebMessageAsJson", "PostWebMessageAsString"]) {
      for (const c of callArguments(code, name)) {
        if (/\.dump\s*\(/.test(c.arg)) offenders.push(`${rel(file)}:${c.line}  ${name}(${c.arg.trim()})`);
      }
    }
  }
  assert.deepEqual(offenders, [], "build the text with host::SerializeBridgeEnvelope");
});

test("HostWindow's emit lambda is the serializer, and its other .dump() calls are the known safe ones", () => {
  const hw = sources.find((s) => basename(s.file) === "HostWindow.cpp");
  assert.ok(hw, "src/host/HostWindow.cpp not found");
  // The lambda handed to BridgeDispatcher carries every event and async
  // response to the UI; reverting it to env.dump() would reopen the
  // invalid-UTF-8 throw for all of them.
  const lambda = /auto\s+emitFn\s*=\s*\[[^\]]*\]\s*\(\s*const\s+nlohmann::json\s*&\s*\w+\s*\)\s*\{([\s\S]*?)\n\s*\};/.exec(hw.code);
  assert.ok(lambda, "the emitFn lambda (const nlohmann::json&) was not found in HostWindow.cpp");
  assert.match(lambda[1], /SerializeBridgeEnvelope\s*\(/, "emitFn must serialize through host::SerializeBridgeEnvelope");
  assert.doesNotMatch(lambda[1], /\.dump\s*\(/, "emitFn must not call .dump() itself");
  // Every other .dump( in the host window sources (HostWindow.cpp, the
  // HostWindow_*.cpp files and HostWindowImpl.h) serializes data nlohmann
  // already parsed (a drive request) or writes a local sidecar file; neither
  // reaches the UI. A new one has to be reviewed and added here by content.
  const allowed = [
    /dispatcher->DispatchSync\(\s*req\.dump\(\)\s*\)/,
    /m_clipRunner->Sidecar\(\)\.dump\(\s*2\s*\)/,
  ];
  const family = sources.filter((s) => /^HostWindow(_\w+)?\.cpp$|^HostWindowImpl\.h$/.test(basename(s.file)));
  assert.equal(family.length, 5, "expected HostWindowImpl.h, HostWindow.cpp and the three HostWindow_*.cpp files");
  const unknown = [];
  for (const { file, code } of family) {
    code.split("\n").forEach((text, idx) => {
      if (/\.dump\s*\(/.test(text) && !allowed.some((re) => re.test(text))) unknown.push(`${basename(file)}:${idx + 1}  ${text.trim()}`);
    });
  }
  assert.deepEqual(unknown, [], "a new .dump() in the host window sources: use SerializeBridgeEnvelope, or allowlist it here if it never reaches the UI");
});

test("both dispatch doors run handlers under RunGuardedDispatch", () => {
  const disp = sources.find((s) => basename(s.file) === "BridgeDispatcher.cpp");
  assert.ok(disp, "src/host/BridgeDispatcher.cpp not found");
  const body = (fn) => {
    const m = new RegExp(`BridgeDispatcher::${fn}\\s*\\([^)]*\\)\\s*\\{`).exec(disp.code);
    assert.ok(m, `BridgeDispatcher::${fn} not found`);
    // Up to the next top-level function: good enough for a presence check.
    const rest = disp.code.slice(m.index + m[0].length);
    const end = rest.search(/\n\}\s*\n/);
    return end === -1 ? rest : rest.slice(0, end);
  };
  assert.match(body("Dispatch"), /\bDispatchParsed\s*\(/, "Dispatch must route through DispatchParsed");
  assert.match(body("DispatchSync"), /\bDispatchParsed\s*\(/, "DispatchSync must route through DispatchParsed");
  assert.match(body("DispatchParsed"), /\bRunGuardedDispatch\s*\(/, "DispatchParsed must wrap DispatchInternal in RunGuardedDispatch");
});

test("SerializeBridgeEnvelope replaces invalid UTF-8 instead of throwing", () => {
  const wire = sources.find((s) => basename(s.file) === "BridgeWire.h");
  assert.ok(wire, "src/host/BridgeWire.h not found");
  const body = /SerializeBridgeEnvelope\s*\([^)]*\)\s*\{([\s\S]*?)\n\}/.exec(wire.code);
  assert.ok(body, "SerializeBridgeEnvelope definition not found in BridgeWire.h");
  assert.match(body[1], /\.dump\s*\([^)]*error_handler_t::replace\s*\)/);
});
