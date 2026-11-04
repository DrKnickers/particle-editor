// Member coverage for `--tool inspect` (benchmark plan section 3.2, check 2).
// The data members declared in ParticleSystem::Emitter (a class), Emitter::Group
// and LinkExemptFlags, minus the documented non-persisted ones, must equal the
// mapped set in tests/fixtures/persisted-members.json. A new member -- padding or
// not -- fails here until it is dumped and mapped (test_particle_system_json
// then checks the mapped key appears in a real dump).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, "..");
const table = JSON.parse(readFileSync(join(repoRoot, "tests", "fixtures", "persisted-members.json"), "utf8"));
const particleHeader = readFileSync(join(repoRoot, "src", "effect", "ParticleSystem.h"), "utf8");
const linkHeader = readFileSync(join(repoRoot, "src", "effect", "LinkGroup.h"), "utf8");

function stripComments(source) {
  return source.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, " ");
}

// The body of `class|struct <name> {...}` (the first definition, not a
// forward declaration), without its outer braces.
export function typeBody(source, keyword, name) {
  const text = stripComments(source);
  const re = new RegExp(`\\b${keyword}\\s+${name}\\b\\s*\\{`, "g");
  const match = re.exec(text);
  if (!match) throw new Error(`no ${keyword} ${name} definition`);
  let depth = 1;
  let i = match.index + match[0].length;
  const start = i;
  for (; i < text.length && depth > 0; i++) {
    if (text[i] === "{") depth++;
    else if (text[i] === "}") depth--;
  }
  return text.slice(start, i - 1);
}

function removeTemplateArgs(text) {
  let out = "";
  let depth = 0;
  for (const ch of text) {
    if (ch === "<") depth++;
    else if (ch === ">") depth = Math.max(0, depth - 1);
    else if (depth === 0) out += ch;
  }
  return out;
}

// Data-member names declared directly in a type body: nested type bodies,
// function bodies and declarations, access specifiers, preprocessor lines,
// typedefs and statics are skipped; `a, b, c;` and `x[N];` are supported.
export function dataMembers(body) {
  const lines = body.split("\n").filter((line) => !line.trim().startsWith("#"));
  const text = lines.join("\n").replace(/\b(public|private|protected)\s*:/g, " ");
  const names = [];
  let statement = "";
  let hadBody = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === "{") {
      let depth = 1;
      for (i++; i < text.length && depth > 0; i++) {
        if (text[i] === "{") depth++;
        else if (text[i] === "}") depth--;
      }
      i--;
      hadBody = true;
      // A function body ends its statement; a type body waits for its `;`.
      if (statement.includes("(")) {
        statement = "";
        hadBody = false;
      }
      continue;
    }
    if (ch !== ";") {
      statement += ch;
      continue;
    }
    const decl = statement.trim();
    statement = "";
    const nested = hadBody;
    hadBody = false;
    if (!decl || nested) continue;
    if (decl.includes("(")) continue;   // functions, constructors, operators
    if (/^(typedef|using|static|friend|enum|struct|class)\b/.test(decl)) continue;
    for (const part of removeTemplateArgs(decl).split(",")) {
      const m = part.replace(/\[[^\]]*\]/g, "").trim().match(/([A-Za-z_]\w*)\s*$/);
      if (m) names.push(m[1]);
    }
  }
  return names;
}

function emitterBody(header) {
  return typeBody(header, "class", "Emitter");
}

function sets(declared, entry) {
  const nonPersisted = new Set(entry.nonPersisted);
  const persisted = declared.filter((name) => !nonPersisted.has(name)).sort();
  return { persisted, mapped: Object.keys(entry.persisted).sort() };
}

test("Emitter: declared data members minus non-persisted equal the mapped set", () => {
  const { persisted, mapped } = sets(dataMembers(emitterBody(particleHeader)), table.Emitter);
  assert.deepEqual(persisted, mapped);
});

test("Group: declared data members equal the mapped set", () => {
  const { persisted, mapped } = sets(dataMembers(typeBody(particleHeader, "struct", "Group")), table.Group);
  assert.deepEqual(persisted, mapped);
});

test("LinkExemptFlags: declared data members equal the mapped set", () => {
  const { persisted, mapped } = sets(dataMembers(typeBody(linkHeader, "struct", "LinkExemptFlags")), table.LinkExemptFlags);
  assert.deepEqual(persisted, mapped);
});

test("the documented runtime members are declared, so subtracting them is meaningful", () => {
  const declared = new Set(dataMembers(emitterBody(particleHeader)));
  for (const name of table.Emitter.nonPersisted) assert.ok(declared.has(name), name);
});

test("nested types and member functions are not members of Emitter", () => {
  const declared = new Set(dataMembers(emitterBody(particleHeader)));
  for (const name of ["keys", "interpolation", "value", "time", "minX", "write", "copy", "Track", "Group"]) {
    assert.ok(!declared.has(name), name);
  }
});

test("a planted unmapped member in a scratch copy of the header fails", () => {
  const planted = particleHeader.replace(/float\s+unknown3f;/, "float unknown3f;\n\t\tfloat plantedMember;");
  assert.notEqual(planted, particleHeader);
  const { persisted, mapped } = sets(dataMembers(emitterBody(planted)), table.Emitter);
  assert.notDeepEqual(persisted, mapped);
  assert.ok(persisted.includes("plantedMember"));
});

test("a planted array member and a nested declaration are parsed", () => {
  const planted = particleHeader.replace(
    /float\s+unknown3f;/,
    "float unknown3f;\n\t\tfloat plantedArray[4];\n\t\tstruct Nested { int inner; float other[2]; };\n\t\tstd::map<int, float> plantedMap;",
  );
  const declared = dataMembers(emitterBody(planted));
  assert.ok(declared.includes("plantedArray"));
  assert.ok(declared.includes("plantedMap"));
  assert.ok(!declared.includes("inner") && !declared.includes("other") && !declared.includes("Nested"));
});
