// Keep native includes independent of MSVC's include-chain search and Windows case.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, posix, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sourceRoot = process.env.SRC_INCLUDE_ROOT ? resolve(process.env.SRC_INCLUDE_ROOT) : resolve(repoRoot, "src");

// Each exception names the SDK/generated owner and its actual callers.
const exemptions = new Map([
  ["WebView2.h", {
    reason: "WebView2 SDK header supplied by NuGet",
    callers: ["host/Compositor.cpp", "host/HostWindowImpl.h", "host/StartupCallbackAdapter.h"],
  }],
  ["WebView2EnvironmentOptions.h", {
    reason: "WebView2 SDK environment options supplied by NuGet",
    callers: ["host/HostWindowImpl.h"],
  }],
  ["expat/expat.h", {
    reason: "Expat's public header supplied by libs/expat-2.8.5/include",
    callers: ["gamedata/xml.h"],
  }],
  ["generated/EmbeddedWebAssets.h", {
    reason: "Generated during the native build by embed-web-dist.mjs",
    callers: ["host/HostWindowImpl.h"],
  }],
]);

function sourceFile(path) {
  return /\.(?:c|cpp|h|hpp)$/.test(path) && !/^(?:host\/(?:third_party|spike)|generated)\//.test(path);
}

function includeProblems(file, token, files) {
  // Reject path shapes before resolving, even when they name an existing file.
  if (/^(?:\/|[A-Za-z]:)/.test(token)) return ["absolute include: " + token];
  if (token.includes("\\")) return ["backslash in include: " + token];
  if (token.split("/").some((part) => part === "." || part === ".." || part === "")) {
    return ["dot or empty segment in include: " + token];
  }
  const exemption = exemptions.get(token);
  if (exemption) return exemption.callers.includes(file) ? [] :
    ["unlisted caller for " + token + " (" + exemption.reason + ")"];
  const target = token.includes("/") ? token : posix.join(posix.dirname(file), token);
  // The set comes from directory-entry spellings, not case-insensitive existsSync.
  if (!files.has(target)) {
    const wrongCase = [...files].find((path) => path.toLowerCase() === target.toLowerCase());
    return [wrongCase ? "wrong case: " + token + "; use " + wrongCase : "missing include: " + token];
  }
  if (token.includes("/") && posix.dirname(target) === posix.dirname(file)) {
    return ["same-folder include must use a bare name: " + token];
  }
  return [];
}

function treeProblems(files) {
  const problems = [];
  const cppNames = new Map();
  const paths = new Set(files.keys());
  for (const [file, text] of files) {
    if (!sourceFile(file)) continue;
    if (file.endsWith(".cpp")) {
      const name = posix.basename(file).toLowerCase();
      if (cppNames.has(name)) problems.push(file + ": duplicate .cpp basename with " + cppNames.get(name));
      cppNames.set(name, file);
    }
    const spans = [...text.matchAll(/(?:u8|u|U|L)?R"([^()\s\\]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\\r\n])*"|'(?:\\[\s\S]|[^'\\\r\n])*'|\/\/[^\r\n]*|\/\*[\s\S]*?\*\//g)];
    for (const match of text.matchAll(/^(?:\uFEFF)?[ \t]*#[ \t]*include[ \t]*"([^"\r\n]+)"/gm)) {
      const directive = match.index + match[0].indexOf("#");
      if (spans.some((span) => span.index <= directive && span.index + span[0].length > directive)) continue;
      const line = text.slice(0, match.index).split("\n").length;
      for (const problem of includeProblems(file, match[1], paths)) problems.push(file + ":" + line + ": " + problem);
    }
  }
  return problems;
}

function readTree(folder, prefix = "") {
  const files = new Map();
  for (const entry of readdirSync(folder, { withFileTypes: true })) {
    const path = prefix + entry.name;
    assert.ok(!entry.isSymbolicLink(), "unexpected symbolic link: " + path);
    if (entry.isDirectory()) {
      for (const [file, text] of readTree(resolve(folder, entry.name), path + "/")) files.set(file, text);
    } else {
      files.set(path, /\.(?:c|cpp|h|hpp)$/.test(path) ? readFileSync(resolve(folder, entry.name), "utf8") : "");
    }
  }
  return files;
}

test("src quoted includes use canonical local or root-relative paths", () => {
  const problems = treeProblems(readTree(sourceRoot));
  assert.deepEqual(problems, [], "src include policy:\n" + problems.join("\n"));
});

test("bare names require a file in the includer's own folder", () => {
  const files = new Set(["rendering/local.h", "effect/shared.h"]);
  assert.deepEqual(includeProblems("rendering/engine.cpp", "local.h", files), []);
  assert.match(includeProblems("rendering/engine.cpp", "shared.h", files)[0], /missing/);
});

test("root-relative includes require exact case and another folder", () => {
  const files = new Set(["rendering/local.h", "effect/Shared.h"]);
  assert.deepEqual(includeProblems("rendering/engine.cpp", "effect/Shared.h", files), []);
  assert.match(includeProblems("rendering/engine.cpp", "effect/shared.h", files)[0], /wrong case/);
  assert.match(includeProblems("rendering/engine.cpp", "Effect/Shared.h", files)[0], /wrong case/);
  assert.match(includeProblems("rendering/engine.cpp", "rendering/local.h", files)[0], /bare name/);
});

test("forbidden path shapes fail before existence checks", () => {
  const files = new Set(["effect/shared.h"]);
  for (const token of ["effect\\shared.h", "../effect/shared.h", "effect/../effect/shared.h",
    "./effect/shared.h", "/effect/shared.h", "C:/effect/shared.h", "\\\\server\\shared.h"]) {
    assert.notDeepEqual(includeProblems("rendering/engine.cpp", token, files), [], token);
  }
});

test("only named external/generated headers at named callers are exempt", () => {
  for (const [token, exemption] of exemptions) {
    assert.ok(exemption.reason);
    assert.deepEqual(includeProblems(exemption.callers[0], token, new Set()), []);
    assert.match(includeProblems("common/utils.cpp", token, new Set())[0], /unlisted caller/);
  }
  assert.match(includeProblems("host/Compositor.cpp", "OtherSdk.h", new Set())[0], /missing/);
});

test("duplicate .cpp basenames fail across the non-vendored source set", () => {
  const files = new Map([
    ["rendering/repeat.cpp", ""], ["effect/repeat.cpp", ""],
    ["host/third_party/repeat.cpp", '#include "../missing.h"'],
    ["host/spike/repeat.cpp", ""], ["generated/repeat.cpp", ""],
  ]);
  assert.deepEqual(treeProblems(files), ["effect/repeat.cpp: duplicate .cpp basename with rendering/repeat.cpp"]);
});

test("directives inside comments and raw strings are ignored", () => {
  const text = '/*\n#include "missing.h"\n*/\nconst char* s = R"fixture(\n#include "also-missing.h"\n)fixture";\n#include "local.h"\n';
  assert.deepEqual(treeProblems(new Map([["rendering/file.cpp", text], ["rendering/local.h", ""]])), []);
});
