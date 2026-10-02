// Real host refusal contracts. The shared native harness launches --test-host,
// which normally exempts document replacement. __testRefuseUnsaved opts only
// this request into the interactive guard; it does not change session state.
// TestHostBridge forwards it verbatim to the C++ dispatcher over host-object
// IPC, so a nested failure resolves here rather than throwing a transport error.
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { bridgeRequest } from "./helpers/bridge-request";
import { test, expect, type Page } from "./helpers/cdp";

const fixturePath = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "fixtures/singleton-emitter.alo",
);
let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test.beforeEach(async () => {
  await bridgeRequest(page, { kind: "file/new", params: {} });
});

test.afterEach(async () => {
  // No opt-in on cleanup: ordinary test-host replacement remains exempt even
  // after an assertion failure, and the next spec receives a clean document.
  await bridgeRequest(page, { kind: "file/new", params: {} });
});

async function documentSnapshot() {
  return page.evaluate(async () => {
    const b = window.bridge!;
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const nodes: typeof tree.root[] = [];
    const visit = (node: typeof tree.root) => {
      nodes.push(node);
      node.children.forEach(visit);
    };
    // The tree root is synthetic; only its descendants are real emitters.
    tree.root.children.forEach(visit);
    const emitters = [];
    for (const node of nodes) {
      const { properties } = await b.request({
        kind: "emitters/get-properties", params: { id: node.id },
      });
      const { tracks } = await b.request({
        kind: "emitters/get-tracks", params: { id: node.id },
      });
      emitters.push({ id: node.id, properties, tracks });
    }
    const state = await b.request({ kind: "engine/state/snapshot", params: {} });
    return {
      tree,
      emitters,
      editor: {
        dirty: state.dirty,
        currentFilePath: state.currentFilePath,
        selectedEmitterId: state.selectedEmitterId,
      },
      undo: { canUndo: state.canUndo, canRedo: state.canRedo },
    };
  });
}

for (const kind of ["file/new", "file/open"] as const) {
  test(`${kind} refuses unsaved work and proceeds after confirmed discard`, async () => {
    const fresh = await documentSnapshot();
    expect(fresh.editor.dirty).toBe(false);
    expect(fresh.tree.root.children).toHaveLength(1);

    // A real particle fixture supplies a non-null current file path. Direct
    // params.path avoids GetOpenFileNameW for both the refused and allowed Open.
    expect(await bridgeRequest(page, {
      kind: "file/open", params: { path: fixturePath },
    })).toMatchObject({ ok: true });
    const loaded = await documentSnapshot();
    expect(loaded.editor.dirty).toBe(false);
    expect(loaded.editor.currentFilePath).toBe(fixturePath);

    const firstId = loaded.tree.root.children[0]?.id;
    if (firstId === undefined) throw new Error("fixture has no emitter");
    await bridgeRequest(page, {
      kind: "emitters/set-properties",
      params: { id: firstId, patch: { lifetime: 123 } },
    });
    await bridgeRequest(page, { kind: "emitters/add-root", params: {} });
    const tree = await bridgeRequest(page, { kind: "emitters/list", params: {} });
    const last = tree.root.children.at(-1);
    if (!last) throw new Error("could not add an emitter");
    await bridgeRequest(page, { kind: "emitters/select", params: { id: last.id } });

    const before = await documentSnapshot();
    expect(before.editor).toMatchObject({
      dirty: true,
      currentFilePath: loaded.editor.currentFilePath,
      selectedEmitterId: last.id,
    });
    expect(before.emitters).toHaveLength(loaded.emitters.length + 1);
    expect(before.emitters[0].properties.lifetime).toBe(123);
    expect(before.undo.canUndo).toBe(true);

    const params = { __testRefuseUnsaved: true, path: fixturePath };
    // No discardUnsaved: the outer transport reply is successful, while its
    // nested result reports refusal. Exact equality guards that contract.
    const refused = kind === "file/new"
      ? await bridgeRequest(page, { kind, params: { __testRefuseUnsaved: true } })
      : await bridgeRequest(page, { kind, params });
    expect(refused).toEqual({ ok: false, error: "unsaved-changes" });
    // Read the host, not React's potentially trailing dirty bit. Includes the
    // stable tree identities, authored data, selection, file path and history.
    expect(await documentSnapshot()).toEqual(before);

    const accepted = kind === "file/new"
      ? await bridgeRequest(page, {
          kind, params: { __testRefuseUnsaved: true, discardUnsaved: true },
        })
      : await bridgeRequest(page, {
          kind, params: { ...params, discardUnsaved: true },
        });
    expect(accepted).toEqual(kind === "file/new" ? {} : { ok: true, path: fixturePath });

    const after = await documentSnapshot();
    expect(after.editor).toEqual({
      dirty: false,
      currentFilePath: kind === "file/new" ? null : loaded.editor.currentFilePath,
      selectedEmitterId: 0,
    });
    // New creates the default single root; Open reloads the pristine fixture.
    // Stable IDs are deliberately absent here because replacement renews them.
    const authored = (list: typeof after.emitters) =>
      list.map(({ properties, tracks }) => ({ properties, tracks }));
    expect(authored(after.emitters)).toEqual(
      authored(kind === "file/new" ? fresh.emitters : loaded.emitters),
    );
    expect(after.tree.root.children).toHaveLength(
      kind === "file/new" ? 1 : loaded.tree.root.children.length,
    );
    expect(after.undo).toEqual(fresh.undo);
  });
}
