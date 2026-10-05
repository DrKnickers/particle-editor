// Keep the particle stream module-owned, and pin its launch/automation seeds.

import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sourceRoot = process.env.PARTICLE_RNG_ROOT ? resolve(process.env.PARTICLE_RNG_ROOT) : resolve(repoRoot, "src");
const launchSeed = "ParticleRandom::Seed(ParticleRandom::LaunchSeed(interactiveSession, static_cast<std::uint32_t>(time(nullptr))));";
const automationSeed = "ParticleRandom::Seed(ParticleRandom::kAutomationSeed);";

function stripNonCode(text) {
  // Leave offsets and newlines intact so diagnostics still name the source line.
  return text.replace(/(?:u8|u|U|L)?R"([^()\s\\]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\\r\n])*"|'(?:\\[\s\S]|[^'\\\r\n])*'|\/\/[^\r\n]*|\/\*[\s\S]*?\*\//g,
    (span) => span.replace(/[^\r\n]/g, " "));
}

function readTree(folder, prefix = "") {
  const files = new Map();
  for (const entry of readdirSync(folder, { withFileTypes: true })) {
    const path = prefix + entry.name;
    assert.ok(!entry.isSymbolicLink(), "unexpected symbolic link: " + path);
    if (entry.isDirectory()) {
      for (const [file, text] of readTree(resolve(folder, entry.name), path + "/")) files.set(file, text);
    } else if (/\.(?:c|cpp|h|hpp)$/.test(path)) {
      files.set(path, readFileSync(resolve(folder, entry.name), "utf8"));
    }
  }
  return files;
}

function treeProblems(files) {
  const problems = [];
  const code = new Map();
  for (const [file, text] of files) {
    const stripped = stripNonCode(text);
    code.set(file, stripped.replace(/\s/g, ""));
    for (const match of stripped.matchAll(/\b(?:std\s*::\s*)?(?:rand|srand)\s*\(/g)) {
      const line = stripped.slice(0, match.index).split("\n").length;
      problems.push(file + ":" + line + ": CRT random call: " + match[0]);
    }
  }
  for (const [file, call] of [
    ["main.cpp", launchSeed],
    ["host/CaptureRunner.cpp", automationSeed],
    ["host/HostWindow_Record.cpp", automationSeed],
  ]) {
    if (!code.get(file)?.includes(call.replace(/\s/g, ""))) {
      problems.push(file + ": missing pinned seed call: " + call);
    }
  }
  return problems;
}

test("src has no CRT random calls and keeps the particle seed boundaries", () => {
  const problems = treeProblems(readTree(sourceRoot));
  assert.deepEqual(problems, [], "particle RNG policy:\n" + problems.join("\n"));
});

function scratchProblems(changes) {
  const root = mkdtempSync(resolve(tmpdir(), "particle-rng-surface-"));
  try {
    const files = new Map([
      ["main.cpp", launchSeed + "\n"],
      ["host/CaptureRunner.cpp", automationSeed + "\n"],
      ["host/HostWindow_Record.cpp", automationSeed + "\n"],
      ...changes,
    ]);
    for (const [file, text] of files) {
      const path = resolve(root, file);
      mkdirSync(dirname(path), { recursive: true });
      writeFileSync(path, text);
    }
    return treeProblems(readTree(root));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

for (const [file, call] of [
  ["simulation/SpawnerDriver.cpp", "std::rand()"],
  ["host/third_party/random.c", "rand()"],
  ["simulation/random.hpp", "srand(1)"],
  ["host/spike/random.cpp", "std::srand(1)"],
  ["generated/random.h", "rand()"],
]) {
  test("CRT guard rejects a call in " + file + " and names its line", () => {
    assert.deepEqual(scratchProblems([[file, "/* ignored\ncomment */\n" + call + ";\n"]]),
      [file + ":3: CRT random call: " + call.slice(0, call.indexOf("(") + 1)]);
  });
}

test("CRT guard ignores comments, literals and identifiers ending in rand", () => {
  assert.deepEqual(scratchProblems([["simulation/SpawnerDriver.cpp", [
    "// std::rand() and srand(1)",
    "/* std::srand(1)\nrand() */",
    'const char* s = "std::rand() and srand(1)";',
    'const char* escaped = "\\\" std::rand()";',
    'const char* raw = R"fixture(std::rand()\nsrand(1))fixture";',
    "const char c = '('; const char quote = '\\'';",
    "operand(); MyRand(); myrand(); mysrand();",
  ].join("\n")]]), []);
});

for (const [name, replacement] of [
  ["removed launch Seed call", ""],
  ["interactiveSession replaced by true", launchSeed.replace("interactiveSession", "true")],
  ["entropy argument replaced by a constant", launchSeed.replace("static_cast<std::uint32_t>(time(nullptr))", "1u")],
  ["launch Seed call only in a comment", "// " + launchSeed],
  ["launch Seed call only in a string", JSON.stringify(launchSeed)],
]) {
  test("launch call pin rejects " + name, () => {
    const problems = scratchProblems([["main.cpp", replacement]]);
    assert.equal(problems.length, 1);
    assert.match(problems[0], /^main\.cpp: missing pinned seed call:/);
  });
}

test("seed call pins ignore whitespace", () => {
  assert.deepEqual(scratchProblems([["main.cpp", launchSeed.replaceAll("(", "(\n ").replaceAll(",", ",\t")]]), []);
});

for (const file of ["host/CaptureRunner.cpp", "host/HostWindow_Record.cpp"]) {
  test("automation seed pin rejects a missing call in " + file, () => {
    assert.deepEqual(scratchProblems([[file, ""]]), [file + ": missing pinned seed call: " + automationSeed]);
  });
}
