// Keep source-map search text and file contents comments true.
// Paths and commands are navigation instructions, not search claims.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const documents = ["src/README.md", "web/apps/editor/README.md", "CONTRIBUTING.md"];
const sourceRoots = ["src", "web/apps/editor/src", "web/packages/bridge-schema/src"];
const excluded = new Set(["generated", "third_party", "node_modules", "dist"]);
const allowList = [
  { span: ".alo", reason: "Particle file extension; not a search term in every linked source file." },
];

function isFile(path) {
  try { return statSync(path).isFile(); } catch { return false; }
}

function pathExists(path) {
  try { statSync(path); return true; } catch { return false; }
}

function docBlocks(markdown) {
  const publicText = markdown.replace(
    /[ \t]*<!--\s*public-sync:strip\s*-->[\s\S]*?<!--\s*\/public-sync:strip\s*-->[ \t]*/g,
    "",
  );
  const blocks = [];
  let item = null;
  let fence = null;
  for (const line of publicText.split(/\r?\n/)) {
    const marker = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
    if (fence) {
      if (marker && marker[1][0] === fence[0] &&
          marker[1].length >= fence.length && marker[2].trim() === "") fence = null;
      continue;
    }
    if (marker) {
      fence = marker[1];
      continue;
    }
    if (/^\s*\|/.test(line)) {
      item = null;
      blocks.push(line);
    } else if (/^\s*(?:[-+*]|\d+[.)])\s+/.test(line)) {
      item = { indent: line.search(/\S/), lines: [line] };
      blocks.push(item.lines);
    } else if (item && (line.trim() === "" || line.search(/\S/) > item.indent)) {
      item.lines.push(line);
    } else {
      item = null;
    }
  }
  return blocks.map((block) => Array.isArray(block) ? block.join("\n") : block);
}

function inlineSpans(text) {
  return [...text.matchAll(/(`+)([\s\S]*?)\1(?!`)/g)].map((match) => {
    return match[2].replace(/\\([!"#$%&'()*+,\-./:;<=>?@[\]\\^_`{|}~])/g, "$1");
  });
}

function linkedFiles(document, block) {
  const prose = block.replace(/(`+)[\s\S]*?\1(?!`)/g, "");
  return [...prose.matchAll(/\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s)]+)(?:\s+["'][^\n]*?["'])?\s*\)/g)]
    .map((match) => {
      let path = match[1];
      if (path.startsWith("<") && path.endsWith(">")) path = path.slice(1, -1);
      if (/^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(path) || /[<>]/.test(path)) return null;
      return resolve(dirname(document), decodeURIComponent(path.split(/[?#]/)[0]));
    }).filter((path) => path && isFile(path));
}

function missingSearchText(document, markdown) {
  const missing = [];
  for (const block of docBlocks(markdown)) {
    const files = linkedFiles(document, block);
    if (files.length === 0) continue;
    const contents = files.map((file) => readFileSync(file, "utf8"));
    for (const span of inlineSpans(block)) {
      if (pathExists(resolve(dirname(document), span)) || pathExists(resolve(repoRoot, span)) ||
          /^(?:node |pnpm |msbuild |git )/.test(span) ||
          allowList.some((entry) => entry.span === span)) continue;
      if (!contents.some((text) => text.includes(span))) {
        missing.push(`${relative(repoRoot, document)}: ${block.slice(0, 60)}: ` +
          `missing span ${JSON.stringify(span)}; searched ${files.map((file) => relative(repoRoot, file)).join(", ")}`);
      }
    }
  }
  return missing;
}

function missingContentsText(source) {
  const lines = source.split(/\r?\n/);
  const marker = lines.slice(0, 60).findIndex((line) => line.includes("Contents (search for the quoted text)"));
  if (marker === -1) return [];
  const entries = [];
  let end = marker + 1;
  for (; end < lines.length; end++) {
    const comment = /^\s*(?:\/\/|\*)(.*)$/.exec(lines[end]);
    if (!comment || /^\s*\*\//.test(lines[end])) break;
    if (comment[1].trim() === "") {
      end++;
      break;
    }
    entries.push(...[...comment[1].matchAll(/"([^"]+)"/g)].map((match) => match[1]));
  }
  const body = lines.slice(end).join("\n");
  return entries.filter((entry) => !body.includes(entry));
}

function sourceFiles(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    if (excluded.has(entry.name)) return [];
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(path);
    return entry.isFile() ? [path] : [];
  });
}

test("source-map rows and list items contain true search text", () => {
  assert.ok(allowList.every((entry) => entry.reason.trim()), "allow-list entries need a reason");
  const missing = documents.flatMap((document) => {
    const fullPath = resolve(repoRoot, document);
    let markdown = readFileSync(fullPath, "utf8");
    if (document === "CONTRIBUTING.md") {
      const start = markdown.indexOf("## Start here");
      const end = markdown.indexOf("## Which check", start);
      assert.ok(start !== -1 && end > start, "contributor search-text sections must exist");
      markdown = markdown.slice(start, end);
    }
    return missingSearchText(fullPath, markdown);
  });
  assert.deepEqual(missing, [], missing.join("\n"));
});

test("file contents comments point to text after the header", () => {
  const missing = sourceRoots.flatMap((root) => sourceFiles(resolve(repoRoot, root)))
    .flatMap((file) => missingContentsText(readFileSync(file, "utf8"))
      .map((span) => `${relative(repoRoot, file)}: missing contents text ${JSON.stringify(span)}`));
  assert.deepEqual(missing, [], missing.join("\n"));
});

test("search-text check accepts true claims and reports false claims", () => {
  const document = resolve(repoRoot, "src/README.md");
  assert.deepEqual(missingSearchText(document,
    '| [schema](../web/packages/bridge-schema/src/index.ts) | `export type RequestId` |'), []);
  const missing = missingSearchText(document,
    '- [schema](../web/packages/bridge-schema/src/index.ts)\n  search for `missing-doc-search-fixture`');
  assert.equal(missing.length, 1);
  assert.ok(missing[0].includes("src"));
  assert.ok(missing[0].includes("- [schema]"));
  assert.ok(missing[0].includes("missing-doc-search-fixture"));
  assert.ok(missing[0].includes("index.ts"));
});

test("search-text check skips paths, commands and blocks without file links", () => {
  const document = resolve(repoRoot, "src/README.md");
  const markdown = [
    '| [schema](../web/packages/bridge-schema/src/index.ts) | `README.md` `web/apps/editor/README.md` |',
    '- [schema](../web/packages/bridge-schema/src/index.ts)',
    '  `node missing.mjs` `pnpm missing` `msbuild missing` `git missing`',
    '- No file link: `missing-doc-search-fixture`',
    '- [external](https://example.com): `missing-doc-search-fixture`',
    '- [directory](../web/): `missing-doc-search-fixture`',
    '```markdown', '| [schema](../web/packages/bridge-schema/src/index.ts) | `missing-fenced` |', '```',
    '<!-- public-sync:strip -->- [schema](../web/packages/bridge-schema/src/index.ts) `missing-private`<!-- /public-sync:strip -->',
  ].join("\n");
  assert.deepEqual(missingSearchText(document, markdown), []);
});

test("search-text check accepts a reasoned allow-list entry", () => {
  const document = resolve(repoRoot, "src/README.md");
  assert.ok(!readFileSync(resolve(repoRoot, "scripts/doc-paths.test.mjs"), "utf8").includes(".alo"));
  assert.deepEqual(missingSearchText(document,
    '| [links](../scripts/doc-paths.test.mjs) | `.alo` |'), []);
});

test("search spans unescape Markdown punctuation and stay in their own block", () => {
  assert.deepEqual(inlineSpans('`a\\|b` ``literal ` tick``'), ["a|b", "literal ` tick"]);
  const document = resolve(repoRoot, "src/README.md");
  assert.equal(missingSearchText(document, [
    '| [links](../scripts/doc-paths.test.mjs) | `export type RequestId` |',
    '| [schema](../web/packages/bridge-schema/src/index.ts) | `export type RequestId` |',
  ].join("\n")).length, 1);
});

test("contents check rejects missing entries and ends at a blank comment", () => {
  assert.deepEqual(missingContentsText([
    '// Contents (search for the quoted text)',
    '//   Present: "actualBody"', '//   Missing: "onlyInHeader"', '//',
    '// actualBody', 'const value = 1;',
  ].join("\n")), ["onlyInHeader"]);
  assert.deepEqual(missingContentsText([
    '/* Contents (search for the quoted text)',
    ' * Present: "actualBody"', ' */', 'const actualBody = 1;',
  ].join("\n")), []);
});
