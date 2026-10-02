// Keep contributor links valid in both the working tree and the public mirror.
// Source maps are checked when present; they are added separately.

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const documents = ["README.md", "CONTRIBUTING.md"];
const sourceMaps = ["src/README.md", "web/apps/editor/README.md"];

function linkPaths(markdown) {
  const publicText = markdown.replace(
    /[ \t]*<!--\s*public-sync:strip\s*-->[\s\S]*?<!--\s*\/public-sync:strip\s*-->[ \t]*/g,
    "",
  );
  let fence = null;
  const prose = publicText.split(/\r?\n/).filter((line) => {
    const marker = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
    if (fence) {
      if (marker && marker[1][0] === fence[0] &&
          marker[1].length >= fence.length && marker[2].trim() === "") fence = null;
      return false;
    }
    if (marker) {
      fence = marker[1];
      return false;
    }
    return true;
  }).join("\n").replace(/(`+)[\s\S]*?\1(?!`)/g, "");

  const destinations = [
    ...prose.matchAll(/\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s)]+)(?:\s+["'][^\n]*?["'])?\s*\)/g),
    ...prose.matchAll(/^ {0,3}\[[^\]\n]+\]:\s*(<[^>\n]+>|\S+)/gm),
  ];
  return destinations.map((match) => {
    let path = match[1];
    if (path.startsWith("<") && path.endsWith(">")) path = path.slice(1, -1);
    return path;
  }).filter((path) => {
    return !/^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(path) &&
      path.split(/[?#]/)[0] !== "../../releases" &&
      !/[<>]/.test(path);
  }).map((path) => decodeURIComponent(path.split(/[?#]/)[0]));
}

function missingLinks(document, markdown) {
  return linkPaths(markdown).filter((path) => !existsSync(resolve(dirname(document), path)));
}

for (const document of [...documents, ...sourceMaps.filter((path) => existsSync(resolve(repoRoot, path)))]) {
  test(`${document} local Markdown links exist`, () => {
    const fullPath = resolve(repoRoot, document);
    const missing = missingLinks(fullPath, readFileSync(fullPath, "utf8"));
    assert.deepEqual(missing, [], `${document}: missing local destinations`);
  });
}

test("link check ignores examples, external links and private regions", () => {
  const markdown = [
    "[web](https://example.com) [email](mailto:editor@example.com) [anchor](#here)",
    "[releases](../../releases#latest) [placeholder](src/<name>.h)",
    "`[inline](missing-inline.md)` ``[inline](missing-code.md)``",
    "```markdown", "[fenced](missing-fenced.md)", "```",
    "~~~", "[fenced](missing-tilde.md)", "~~~",
    "<!-- public-sync:strip -->[private](missing-private.md)<!-- /public-sync:strip -->",
  ].join("\n");
  assert.deepEqual(linkPaths(markdown), []);
});

test("local paths keep their document base and drop fragments", () => {
  assert.deepEqual(linkPaths('[root](../../README.md#download) [map](<file%20name.md>)\n[ref]: ../README.md'),
    ["../../README.md", "file name.md", "../README.md"]);
  const document = resolve(repoRoot, "web", "apps", "editor", "README.md");
  assert.deepEqual(missingLinks(document, "[root](../../../README.md#download)"), []);
  assert.deepEqual(missingLinks(document, "[broken](missing-doc-paths-fixture.md)"),
    ["missing-doc-paths-fixture.md"]);
});
