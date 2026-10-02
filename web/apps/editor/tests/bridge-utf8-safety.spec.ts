// Bridge UTF-8 safety — a non-UTF-8 emitter name end to end.
//
// Emitter names are the raw bytes of an .alo's name chunk, not UTF-8. Before
// the fix, one emitter named "\xE9..." made the host's default json dump()
// throw: the emitters/list response never arrived, and the async door's throw
// escaped through the WebView2 COM callback. Now every response and event is
// serialized through host::SerializeBridgeEnvelope (src/host/BridgeWire.h),
// which replaces the bad byte with U+FFFD.
//
// The fixture is singleton-emitter.alo with its emitters' "default" name
// patched to "\xE9efault" (same length, so no chunk size changes). This
// exercises:
//   1. a response path  — emitters/preview-from-file returns "�efault", and
//      emitters/list after the import returns it with the import's numeric
//      suffix ("�efault_1");
//   2. an event path    — the emitters/tree/changed push after the import
//      carries the same name;
// then undoes the import so later specs see the tree they expect.

import { test, expect, type Page } from "./helpers/cdp";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { resolve, dirname, join } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const FIXTURE = resolve(__dirname, "fixtures", "singleton-emitter.alo");
const REPLACED_NAME = "�efault";

// 0x16 name chunk: type 0x16, size 8, "default\0".
const NAME_CHUNK = Buffer.from([0x16, 0, 0, 0, 8, 0, 0, 0, ...Buffer.from("default\0", "latin1")]);

let page: Page;
let scratchDir = "";
let patchedPath = "";

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
  const bytes = await readFile(FIXTURE);
  let patched = 0;
  for (let at = bytes.indexOf(NAME_CHUNK); at !== -1; at = bytes.indexOf(NAME_CHUNK, at + 1)) {
    bytes[at + 8] = 0xe9; // 'd' -> Latin-1 e-acute: not valid UTF-8 on its own
    patched += 1;
  }
  expect(patched).toBeGreaterThan(0);
  scratchDir = await mkdtemp(join(tmpdir(), "pe-utf8-name-"));
  patchedPath = join(scratchDir, "utf8-name.alo").replace(/\\/g, "/");
  await writeFile(patchedPath, bytes);
});

test.afterAll(async () => {
  if (scratchDir) await rm(scratchDir, { recursive: true, force: true });
});

test("a non-UTF-8 emitter name reaches the UI as U+FFFD on responses and events", async () => {
  const result = await page.evaluate(async ({ path, name }) => {
    type TreeNode = { id: number; name: string; children?: TreeNode[] };
    const names = (node: TreeNode | undefined): string[] =>
      (node?.children ?? []).flatMap((c) => [c.name, ...names(c)]);
    const collectIds = (node: TreeNode | undefined): number[] =>
      (node?.children ?? []).flatMap((c) => [c.id, ...collectIds(c)]);
    const bridge = window.bridge!;
    // Imported emitters are renamed through GenerateDuplicateName, which adds a
    // numeric suffix ("<name>_01"), so after the import match on the prefix.
    const hasImported = (list: string[]) => list.some((n) => n.startsWith(name));

    const treeEventNames: string[][] = [];
    const off = bridge.on("emitters/tree/changed", (e: { payload: { root: TreeNode } }) => {
      treeEventNames.push(names(e.payload.root));
    });

    // Response path 1: the preview tree of the file itself.
    const preview = await bridge.request({ kind: "emitters/preview-from-file", params: { path } });
    const previewNames = preview.ok ? names(preview.tree) : [];
    const selected = preview.ok ? collectIds(preview.tree) : [];

    const imp = await bridge.request({ kind: "emitters/import-from-file", params: { path, selected } });

    // Response path 2: the live tree, now holding the imported emitters.
    const list = await bridge.request({ kind: "emitters/list", params: {} });
    const listNames = names(list.root);

    // Event path: wait (bounded) for a tree/changed push carrying the name.
    await new Promise<void>((done) => {
      const deadline = Date.now() + 3000;
      const poll = () => {
        if (treeEventNames.some(hasImported) || Date.now() >= deadline) done();
        else setTimeout(poll, 10);
      };
      poll();
    });
    off();

    // Restore the pre-import tree for later specs.
    await bridge.request({ kind: "undo/perform", params: { direction: "undo" } });
    const after = await bridge.request({ kind: "emitters/list", params: {} });

    return {
      previewOk: preview.ok === true,
      previewHasName: previewNames.includes(name),
      importOk: imp.ok === true,
      listHasName: hasImported(listNames),
      eventHasName: treeEventNames.some(hasImported),
      undoneHasName: hasImported(names(after.root)),
    };
  }, { path: patchedPath, name: REPLACED_NAME });

  expect(result.previewOk).toBe(true);
  expect(result.previewHasName).toBe(true);
  expect(result.importOk).toBe(true);
  expect(result.listHasName).toBe(true);
  expect(result.eventHasName).toBe(true);
  expect(result.undoneHasName).toBe(false);
});
