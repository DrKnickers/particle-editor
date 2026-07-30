// autosave/check-recovery + autosave/recover round-trip spec.
//
// The deterministic, harness-safe contracts:
//   1. check-recovery is SUPPRESSED under ordinary --test-host use.
//   2. recover with no pending orphan is a state-preserving failure.
//   3. the harness gives the host a unique child-only TEMP root and explicitly
//      opts one request into the real production scanner/recover handler. That
//      path proves verified handoff-before-delete, PID-reuse identity, failure
//      retention/retry, and explicit discard without touching user autosaves.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import {
  access,
  copyFile,
  mkdir,
  readFile,
  readdir,
  rm,
  rmdir,
  stat,
} from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const AUTOSAVE_TEST_DIR = process.env.PE_AUTOSAVE_TEST_DIR;
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE_A = path.resolve(__dirname, "fixtures/a11y-base-state.alo");
const FIXTURE_B = path.resolve(__dirname, "fixtures/nt-5-singleton.alo");

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as unknown as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("autosave/check-recovery returns no orphan under ordinary --test-host", async () => {
  const r = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    return await b.request({ kind: "autosave/check-recovery", params: {} });
  });
  expect(r).toEqual({ orphan: null });
});

test("autosave/recover{discard} is a safe no-op with no pending orphan", async () => {
  const { result, dirtyBefore, dirtyAfter } = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    const res = await b.request({ kind: "autosave/recover", params: { choice: "discard" } });
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { result: res, dirtyBefore: before.dirty, dirtyAfter: after.dirty };
  });
  expect(result).toEqual({ status: "failed", reason: "no_pending_session" });
  expect(dirtyAfter).toBe(dirtyBefore);
});

async function pathExists(candidate: string) {
  try {
    await access(candidate);
    return true;
  } catch {
    return false;
  }
}

async function emitterCount() {
  return await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const list = await b.request({ kind: "emitters/list", params: {} });
    const count = (nodes: Array<{ children?: unknown[] }>): number =>
      nodes.reduce(
        (sum, node) =>
          sum + 1 + count((node.children ?? []) as Array<{ children?: unknown[] }>),
        0,
      );
    return count(list.root.children);
  });
}

function differentCreationToken(
  currentHex: string,
  offset: bigint,
  excludedHex: string[] = [],
) {
  const current = BigInt(`0x${currentHex}`);
  const mask = (1n << 64n) - 1n;
  let candidate = (current + offset) & mask;
  const excluded = new Set(excludedHex.map((value) => BigInt(`0x${value}`)));
  while (candidate === 0n || candidate === current || excluded.has(candidate)) {
    candidate = (candidate + 1n) & mask;
  }
  return candidate.toString(16).padStart(16, "0");
}

test("real recovery establishes a verified session handoff before old-tier deletion", async () => {
  if (!AUTOSAVE_TEST_DIR) {
    throw new Error("native harness did not provide PE_AUTOSAVE_TEST_DIR");
  }

  await mkdir(AUTOSAVE_TEST_DIR, { recursive: true });
  await page.evaluate(async () => {
    // Ensure hasCurrentFile=false so the non-test suppression predicates do not
    // mask the explicitly isolated scanner call.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    await (window as any).bridge.request({ kind: "file/new", params: {} });
  });

  const deadPid = 4294967294;
  const firstOld = path.join(
    AUTOSAVE_TEST_DIR,
    `autosave-${deadPid}-0000000000000001-recent.alo`,
  );
  let blockedDestination: string | null = null;

  try {
    await copyFile(FIXTURE_A, firstOld);
    const firstCheck = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/check-recovery",
        params: { __testAllowRecovery: true },
      });
    });
    expect(firstCheck.orphan).not.toBeNull();

    const firstRecover = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/recover",
        params: { choice: "recent" },
      });
    });
    expect(firstRecover).toEqual({ status: "recovered" });
    expect(await pathExists(firstOld)).toBe(false);
    expect(await emitterCount()).toBe(3); // fixture A: root + lifetime + death

    const afterFirst = await readdir(AUTOSAVE_TEST_DIR);
    const replacementName = afterFirst.find((name) =>
      /^autosave-\d+-[0-9a-f]{16}-recent\.alo$/i.test(name),
    );
    expect(replacementName).toBeDefined();
    if (!replacementName) return;

    const parsed = /^autosave-(\d+)-([0-9a-f]{16})-recent\.alo$/i.exec(
      replacementName,
    );
    expect(parsed).not.toBeNull();
    if (!parsed) return;
    const [, livePid, liveCreation] = parsed;
    const replacementPath = path.join(AUTOSAVE_TEST_DIR, replacementName);
    expect((await stat(replacementPath)).isFile()).toBe(true);

    // Simulate PID reuse directly: same currently-live PID, different creation
    // FILETIME. PID-only liveness suppresses this exact orphan.
    const reusedCreation = differentCreationToken(liveCreation, 1n);
    const secondOld = path.join(
      AUTOSAVE_TEST_DIR,
      `autosave-${livePid}-${reusedCreation}-recent.alo`,
    );
    await copyFile(FIXTURE_B, secondOld);
    const secondCheck = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/check-recovery",
        params: { __testAllowRecovery: true },
      });
    });
    expect(secondCheck.orphan).not.toBeNull();

    // The debug/test-host seam makes the attempt-local loaded object serialize
    // an out-of-range group type. Production parse verification must reject
    // that candidate before replacement. Ordinary Autosave::Write would accept
    // it, so this phase pins the dispatcher to WriteRecoveryHandoff rather than
    // merely proving the helper in isolation.
    const currentBeforeVerificationFault = await readFile(replacementPath);
    const rejectedUnparseableHandoff = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/recover",
        params: {
          choice: "recent",
          __testAllowRecovery: true,
          __testCorruptHandoffCandidate: true,
        },
      });
    });
    expect(rejectedUnparseableHandoff).toEqual({
      status: "failed",
      reason: "handoff_write_failed",
    });
    expect(await pathExists(secondOld)).toBe(true);
    expect(await readFile(replacementPath)).toEqual(currentBeforeVerificationFault);
    expect(await emitterCount()).toBe(3);
    expect((await readdir(AUTOSAVE_TEST_DIR)).some((name) => name.endsWith(".tmp")))
      .toBe(false);

    // A directory at the exact destination lets the candidate serialize and
    // parse, then forces MoveFileExW to fail. The old orphan and fixture A in
    // memory must survive; the pending session must remain retryable.
    await rm(replacementPath);
    await mkdir(replacementPath);
    blockedDestination = replacementPath;
    const failedHandoff = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/recover",
        params: { choice: "recent" },
      });
    });
    expect(failedHandoff).toEqual({
      status: "failed",
      reason: "handoff_write_failed",
    });
    expect(await pathExists(secondOld)).toBe(true);
    expect(await emitterCount()).toBe(3);
    expect((await readdir(AUTOSAVE_TEST_DIR)).some((name) => name.endsWith(".tmp")))
      .toBe(false);

    await rmdir(replacementPath);
    blockedDestination = null;
    const retry = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/recover",
        params: { choice: "recent" },
      });
    });
    expect(retry).toEqual({ status: "recovered" });
    expect(await pathExists(secondOld)).toBe(false);
    expect(await emitterCount()).toBe(2); // fixture B
    expect((await stat(replacementPath)).isFile()).toBe(true);

    // Explicit discard remains the intentional no-handoff delete path, and it
    // cannot consume the already-established current-session replacement.
    const discardCreation =
      differentCreationToken(liveCreation, 2n, [reusedCreation]);
    const discardOld = path.join(
      AUTOSAVE_TEST_DIR,
      `autosave-${livePid}-${discardCreation}-recent.alo`,
    );
    await copyFile(FIXTURE_A, discardOld);
    const discardCheck = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/check-recovery",
        params: { __testAllowRecovery: true },
      });
    });
    expect(discardCheck.orphan).not.toBeNull();
    const discarded = await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return await (window as any).bridge.request({
        kind: "autosave/recover",
        params: { choice: "discard" },
      });
    });
    expect(discarded).toEqual({ status: "discarded" });
    expect(await pathExists(discardOld)).toBe(false);
    expect((await stat(replacementPath)).isFile()).toBe(true);
  } finally {
    if (blockedDestination) {
      await rmdir(blockedDestination).catch(() => {});
    }
    await page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      await (window as any).bridge.request({ kind: "file/new", params: {} });
    }).catch(() => {});
  }
});
