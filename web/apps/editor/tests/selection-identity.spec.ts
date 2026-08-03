// Native selection-identity contracts.
//
// Emitter ids on the wire are positional, so structural deletion must preserve
// the selected emitter by stable identity (or clear it when its subtree dies).
// Document replacement must likewise publish the reset selection in its first
// state snapshot, not merely repair the final scalar after a stale event.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { copyFile, mkdtemp, rm } from "node:fs/promises";
import { resolve, dirname, join } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

type DeletionKind = "emitters/delete" | "emitters/delete-many" | "emitters/cut";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test.beforeEach(async () => {
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    await (window as any).bridge.request({ kind: "file/new", params: {} });
  });
});

for (const deletionKind of [
  "emitters/delete",
  "emitters/delete-many",
  "emitters/cut",
] as const satisfies readonly DeletionKind[]) {
  test(`selection identity across emitter deletion: ${deletionKind}`, async () => {
    const result = await page.evaluate(async (kind) => {
      type TreeNode = {
        id: number;
        stableId: number;
        children: TreeNode[];
      };
      type Bridge = {
        request<T>(request: { kind: string; params: object }): Promise<T>;
        on(
          kind: string,
          handler: (event: { payload: { id: number | null } }) => void,
        ): () => void;
      };

      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge as Bridge;
      const sleep = (ms: number) => new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
      const list = () =>
        b.request<{ root: TreeNode }>({ kind: "emitters/list", params: {} });
      const snapshot = () =>
        b.request<{ selectedEmitterId: number | null }>({
          kind: "engine/state/snapshot",
          params: {},
        });
      const flatten = (node: TreeNode): TreeNode[] => [
        node,
        ...node.children.flatMap(flatten),
      ];
      const selectedStableId = async () => {
        const [tree, state] = await Promise.all([list(), snapshot()]);
        if (state.selectedEmitterId === null) return null;
        return (
          flatten(tree.root).find((node) => node.id === state.selectedEmitterId)
            ?.stableId ?? null
        );
      };
      const addRoots = async (count: number) => {
        for (let i = 0; i < count; i++) {
          await b.request<{ newId: number }>({
            kind: "emitters/add-root",
            params: {},
          });
        }
      };
      const deleteIds = async (ids: number[]) => {
        const announced: Array<number | null> = [];
        const off = b.on("emitters/selected", (event) => {
          announced.push(event.payload.id);
        });
        if (kind === "emitters/delete") {
          await b.request({ kind, params: { id: ids[0] } });
        } else {
          await b.request({ kind, params: { ids } });
        }
        await sleep(50);
        off();
        const state = await snapshot();
        const tree = await list();
        return {
          announced,
          selectedId: state.selectedEmitterId,
          selectedStableId: await selectedStableId(),
          nodes: flatten(tree.root),
        };
      };

      // Lower-index deletion: C2 must remain selected as C1. Baseline leaves
      // positional id 2 behind, which now selects D.
      await b.request({ kind: "file/new", params: {} });
      await addRoots(4);
      const lowerBefore = (await list()).root.children;
      const [a, , c, d, e] = lowerBefore;
      if (!a || !c || !d || !e) throw new Error("lower-index fixture needs five roots");
      await b.request({ kind: "emitters/select", params: { id: c.id } });
      const lower = await deleteIds(
        kind === "emitters/delete" ? [a.id] : [a.id, e.id],
      );

      // Recursive deletion: B1 is inside A0's subtree. Baseline keeps id 1,
      // which selects surviving root D after A+B disappear.
      await b.request({ kind: "file/new", params: {} });
      const ancestorRoot = (await list()).root.children[0];
      if (!ancestorRoot) throw new Error("ancestor fixture needs a root");
      const childResult = await b.request<{ newId: number }>({
        kind: "emitters/add-lifetime-child",
        params: { parentId: ancestorRoot.id },
      });
      if (childResult.newId < 0) throw new Error("could not add selected child");
      await addRoots(3);
      const ancestorBefore = await list();
      const ancestorNodes = flatten(ancestorBefore.root);
      const selectedChild = ancestorNodes.find((node) => node.id === childResult.newId);
      const ancestorRoots = ancestorBefore.root.children;
      const ancestorTail = ancestorRoots.at(-1);
      const survivingD = ancestorRoots[2];
      if (!selectedChild || !ancestorTail || !survivingD) {
        throw new Error("ancestor fixture shape is incomplete");
      }
      await b.request({
        kind: "emitters/select",
        params: { id: selectedChild.id },
      });
      const ancestor = await deleteIds(
        kind === "emitters/delete"
          ? [ancestorRoot.id]
          : [ancestorRoot.id, ancestorTail.id],
      );

      // Overreach guard: deleting a higher-index unrelated root must neither
      // clear nor re-announce the unchanged A0 selection.
      await b.request({ kind: "file/new", params: {} });
      await addRoots(2);
      const overreachRoots = (await list()).root.children;
      const selectedA = overreachRoots[0];
      const unrelatedC = overreachRoots[2];
      if (!selectedA || !unrelatedC) throw new Error("overreach fixture needs three roots");
      await b.request({ kind: "emitters/select", params: { id: selectedA.id } });
      const overreach = await deleteIds([unrelatedC.id]);

      return {
        lower: {
          ...lower,
          cStableId: c.stableId,
          dStableId: d.stableId,
        },
        ancestor: {
          ...ancestor,
          selectedChildStableId: selectedChild.stableId,
          survivingDStableId: survivingD.stableId,
        },
        overreach: {
          ...overreach,
          selectedAStableId: selectedA.stableId,
        },
      };
    }, deletionKind);

    expect.soft(result.lower.selectedId).toBe(1);
    expect.soft(result.lower.selectedStableId).toBe(result.lower.cStableId);
    expect
      .soft(
        result.lower.nodes.find(
          (node) => node.stableId === result.lower.dStableId,
        )?.id,
      )
      .toBe(2);
    expect.soft(result.lower.announced).toEqual([1]);

    expect.soft(result.ancestor.selectedId).toBeNull();
    expect.soft(result.ancestor.selectedStableId).toBeNull();
    expect.soft(
      result.ancestor.nodes.some(
        (node) => node.stableId === result.ancestor.selectedChildStableId,
      ),
    ).toBe(false);
    expect.soft(
      result.ancestor.nodes.find(
        (node) => node.stableId === result.ancestor.survivingDStableId,
      )?.id,
    ).toBe(1);
    expect.soft(result.ancestor.announced).toEqual([null]);

    expect.soft(result.overreach.selectedId).toBe(0);
    expect.soft(result.overreach.selectedStableId).toBe(
      result.overreach.selectedAStableId,
    );
    expect.soft(result.overreach.announced).toEqual([]);
  });
}

type ReplacementEvent =
  | { kind: "engine/state/changed"; selectedEmitterId: number | null }
  | { kind: "emitters/tree/changed" }
  | { kind: "emitters/selected"; id: number | null };

const expectedReplacementEvents: ReplacementEvent[] = [
  { kind: "engine/state/changed", selectedEmitterId: 0 },
  { kind: "emitters/tree/changed" },
  { kind: "emitters/selected", id: 0 },
];

async function selectSecondEmitter(path: string) {
  await page.evaluate(async (fixturePath) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "file/open", params: { path: fixturePath } });
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const second = tree.root?.children?.[1];
    if (!second) throw new Error("replacement fixture needs two emitters");
    await b.request({ kind: "emitters/select", params: { id: second.id } });
    // Dispatch entry flushes any pending state/tree event before returning the
    // snapshot, so later subscriptions observe only the replacement under test.
    await b.request({ kind: "engine/state/snapshot", params: {} });
  }, path);
}

async function captureReplacementEvents(request: {
  kind: string;
  params: object;
}) {
  return page.evaluate(async (replacementRequest) => {
    type Bridge = {
      request<T>(request: { kind: string; params: object }): Promise<T>;
      on(kind: string, handler: (event: { payload: unknown }) => void): () => void;
    };
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge as Bridge;
    const seen: ReplacementEvent[] = [];
    const offState = b.on("engine/state/changed", (event) => {
      seen.push({
        kind: "engine/state/changed",
        selectedEmitterId: (event.payload as { selectedEmitterId: number | null })
          .selectedEmitterId,
      });
    });
    const offTree = b.on("emitters/tree/changed", () => {
      seen.push({ kind: "emitters/tree/changed" });
    });
    const offSelection = b.on("emitters/selected", (event) => {
      seen.push({
        kind: "emitters/selected",
        id: (event.payload as { id: number | null }).id,
      });
    });

    const response = await b.request<unknown>(replacementRequest);
    await new Promise((resolveWait) => setTimeout(resolveWait, 50));
    // A follow-up dispatch forces any incorrectly coalesced events to flush
    // while the listeners are still attached.
    const snapshot = await b.request<{ selectedEmitterId: number | null }>({
      kind: "engine/state/snapshot",
      params: {},
    });
    await new Promise((resolveWait) => setTimeout(resolveWait, 20));
    offState();
    offTree();
    offSelection();
    return { events: seen, response, selectedEmitterId: snapshot.selectedEmitterId };
  }, request);
}

test("file/open publishes the reset selection in its first ordered state event", async () => {
  const fixturePath = resolve(__dirname, "fixtures/singleton-emitter.alo");
  await selectSecondEmitter(fixturePath);
  // Exceed kEmitCoalesceMs so the old implementation deterministically exposed
  // stale selectedEmitterId=1 in its first immediate state event.
  await page.waitForTimeout(50);
  const result = await captureReplacementEvents({
    kind: "file/open",
    params: { path: fixturePath },
  });

  expect(result.events).toEqual(expectedReplacementEvents);
  expect(result.selectedEmitterId).toBe(0);
});

test("document replacement keeps state -> tree -> selection when coalescing is primed", async () => {
  const fixturePath = resolve(__dirname, "fixtures/singleton-emitter.alo");
  await selectSecondEmitter(fixturePath);
  const result = await captureReplacementEvents({
    kind: "debug/emit-document-replaced",
    params: {},
  });

  expect(result.events).toEqual(expectedReplacementEvents);
  expect(result.selectedEmitterId).toBe(0);
});

test("autosave/recover uses the same ordered replacement notification", async () => {
  const fixturePath = resolve(__dirname, "fixtures/singleton-emitter.alo");
  const scratchDir = await mkdtemp(join(tmpdir(), "pe-selection-recovery-"));
  const recoveryPath = join(scratchDir, "recent.alo");
  try {
    await copyFile(fixturePath, recoveryPath);
    await selectSecondEmitter(fixturePath);
    await page.evaluate(async (path) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge;
      await b.request({
        kind: "debug/seed-autosave-recovery",
        params: { path, originalFilename: "" },
      });
    }, recoveryPath);

    const result = await captureReplacementEvents({
      kind: "autosave/recover",
      params: { choice: "recent" },
    });
    expect.soft(result.response).toEqual({ status: "recovered" });
    expect.soft(result.events).toEqual(expectedReplacementEvents);
    expect(result.selectedEmitterId).toBe(0);
  } finally {
    await rm(scratchDir, { recursive: true, force: true });
  }
});
