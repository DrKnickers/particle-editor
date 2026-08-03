// Task 2.4 contract tests: Background picker wired against the *real*
// native bridge inside ParticleEditor.exe --test-host. Sibling
// of bridge-native.spec.ts (Task 2.2.1) — same CDP-attach harness, same
// `window.bridge` host-object channel.
//
//   - engine/set/skydome-slot        (bundled slot mutation)
//   - engine/set/background          (COLORREF round-trip — Win32 byte order)
//   - engine/set/skydome-custom-path (custom slot persistence)
//   - engine/set/ground-texture       (actual-slot/applied load contract)
//   - engine/set/ground-slot-custom-path (local/remote path boundary)
//   - undo/perform                   (handler dispatch, Task 2.4 surface)
//
// skydome-slot / skydome-custom-path are exercised here purely as a
// native-bridge contract (the host's registry startup-restore still drives
// them); the React BackgroundPicker no longer does — its custom
// skydome-texture slots were removed (it's Game dome + Solid colour only).
//
// Notes on TestHostBridge.on(): the host-object channel doesn't carry
// events, so TestHostBridge.on returns a no-op unsubscribe. Specs that
// would otherwise wait on `engine/state/changed` instead poll a fresh
// `engine/state/snapshot` after the mutation lands.
import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { mkdir, unlink, writeFile } from "node:fs/promises";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const ONE_PIXEL_BMP = Buffer.from(
  "Qk06AAAAAAAAADYAAAAoAAAAAQAAAAEAAAABABgAAAAAAAQAAAATCwAAEwsAAAAAAAAAAAAAAAD/AA==",
  "base64",
);
// Uncompressed 1x1, 24-bit TGA (red BGR pixel). This is accepted by the
// Ground picker, so the success control uses the same file class as a user.
const ONE_PIXEL_TGA = Buffer.from([
  0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0, 0, 0, 255,
]);

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
    { timeout: 15_000 }
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("Background popover opens from the toolbar dropdown trigger", async () => {
  // Task 2.2: the BackgroundPicker slide-in ToolPanel was replaced by a
  // Radix Popover triggered from the Toolbar's Group 4 dropdown. The
  // dropdown button still carries aria-label="Background", but the
  // mounted content is now a popover wrapper (data-radix-popper-content-wrapper)
  // rather than role="dialog". BackgroundPickerBody is now Game dome +
  // Solid colour only (the custom skydome-texture tiles were removed), so the
  // aria-pressed surface is the Space/Land context toggle (2) + the Solid
  // colour swatch (1) = 3. The Primary/Secondary domes are <select>s, not
  // aria-pressed buttons. (e2e against --test-host; not part of the Vitest leg.)
  const probe = await page.evaluate(async () => {
    const btn = document.querySelector<HTMLButtonElement>('button[aria-label="Background"]');
    if (!btn) return { clicked: false, popover: false, slots: 0 };
    btn.click();
    await new Promise((r) => setTimeout(r, 250));
    const popover = document.querySelector('[data-radix-popper-content-wrapper]');
    const slots = popover?.querySelectorAll("button[aria-pressed]").length ?? 0;
    return { clicked: true, popover: !!popover, slots };
  });
  expect(probe.clicked).toBe(true);
  expect(probe.popover).toBe(true);
  expect(probe.slots).toBe(3);
});

test("engine/set/skydome-slot reports exact applied and actual slots for invalid, fallback, no-op, and bundled requests", async () => {
  const result = await page.evaluate(async () => {
    type AnyBridge = {
      request(r: { kind: string; params: object }): Promise<unknown>;
    };
    const b = (window as { bridge?: AnyBridge }).bridge;
    if (!b) throw new Error("window.bridge not attached");
    type SlotResult = { slot: number; applied: boolean };
    const setSlot = (slot: number) =>
      b.request({ kind: "engine/set/skydome-slot", params: { slot } }) as Promise<SlotResult>;
    const getSlot = async () =>
      ((await b.request({
        kind: "engine/state/snapshot",
        params: {},
      })) as { skydomeSlot: number }).skydomeSlot;

    try {
      // Overreach control: bundled slot 5 must still apply.
      const bundled = await setSlot(5);

      // Exact wrong value: invalid slot 12 must not be echoed as if selected.
      const invalid = await setSlot(12);
      const afterInvalid = await getSlot();

      // Deterministic load failure: custom slot 9 is empty while inactive.
      await b.request({
        kind: "engine/set/skydome-custom-path",
        params: { slot: 9, path: "" },
      });
      const emptyCustom = await setSlot(9);
      const afterEmptyCustom = await getSlot();

      // Overreach controls: Off is a valid no-op after fallback, and bundled
      // slot 5 remains selectable afterward.
      const offNoop = await setSlot(0);
      const bundledAgain = await setSlot(5);
      return {
        bundled,
        invalid,
        afterInvalid,
        emptyCustom,
        afterEmptyCustom,
        offNoop,
        bundledAgain,
      };
    } finally {
      await setSlot(0);
      await b.request({
        kind: "engine/set/skydome-custom-path",
        params: { slot: 9, path: "" },
      });
    }
  });

  expect(result.bundled).toEqual({ slot: 5, applied: true });
  expect(result.invalid).toEqual({ slot: 5, applied: false });
  expect(result.afterInvalid).toBe(5);
  expect(result.emptyCustom).toEqual({ slot: 0, applied: false });
  expect(result.afterEmptyCustom).toBe(0);
  expect(result.offNoop).toEqual({ slot: 0, applied: true });
  expect(result.bundledAgain).toEqual({ slot: 5, applied: true });
});

test("engine/set/background round-trips a COLORREF (orange = 0x000088ff)", async () => {
  // COLORREF in Win32 is 0x00BBGGRR — low byte is red, not blue. The
  // colorref.ts helpers in the React app handle the swap; this spec
  // exercises the wire format directly. 0x000088ff is "orange-ish":
  // R=0xff, G=0x88, B=0x00 — verifies the dispatcher doesn't reorder
  // bytes on the way through.
  const after = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({ kind: "engine/set/background", params: { rgb: 0x000088ff } });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { background: number };
    return snap.background;
  });
  expect(after).toBe(0x000088ff);
});

test("engine/set/skydome-custom-path persists across snapshots (slot 9)", async () => {
  const after = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    await b.request({
      kind: "engine/set/skydome-custom-path",
      params: { slot: 9, path: "C:/fake/test.dds" },
    });
    const snap = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { skydomeCustomPaths: string[] };
    // The DTO flattens slots 9..11 to a 0..2 array (see
    // BuildEngineStateSnapshot in BridgeDispatcher.cpp).
    return snap.skydomeCustomPaths[0];
  });
  expect(after).toBe("C:/fake/test.dds");
});

test("custom slot paths accept local UNC hosts but still reject remote hosts", async () => {
  const localPath = "\\\\wsl.localhost\\Ubuntu\\textures\\ground.dds";
  const remotePath = "\\\\attacker\\share\\ground.dds";
  const result = await page.evaluate(async ({ localPath, remotePath }) => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");

    let localResolved = true;
    try {
      await b.request({
        kind: "engine/set/skydome-custom-path",
        params: { slot: 10, path: localPath },
      });
    } catch {
      localResolved = false;
    }
    const afterLocal = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { skydomeCustomPaths: string[] };

    let remoteRejected = false;
    try {
      await b.request({
        kind: "engine/set/skydome-custom-path",
        params: { slot: 10, path: remotePath },
      });
    } catch {
      remoteRejected = true;
    }
    const afterRemote = (await b.request({
      kind: "engine/state/snapshot",
      params: {},
    })) as { skydomeCustomPaths: string[] };
    await b.request({
      kind: "engine/set/skydome-custom-path",
      params: { slot: 10, path: "" },
    });
    return {
      localResolved,
      localStored: afterLocal.skydomeCustomPaths[1],
      remoteRejected,
      afterRemote: afterRemote.skydomeCustomPaths[1],
    };
  }, { localPath, remotePath });

  expect(result.localResolved).toBe(true);
  expect(result.localStored).toBe(localPath);
  expect(result.remoteRejected).toBe(true);
  expect(result.afterRemote).toBe(localPath);
});

test("ground custom-path setter accepts a local path and rejects remote UNC without overwriting it", async ({}, testInfo) => {
  await mkdir(testInfo.outputDir, { recursive: true });
  const localPath = testInfo.outputPath("ground-local.tga");
  const remotePath = "\\\\attacker\\share\\ground.dds";
  await writeFile(localPath, ONE_PIXEL_TGA);

  try {
    const result = await page.evaluate(async ({ localPath, remotePath }) => {
      const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
        .bridge;
      if (!b) throw new Error("window.bridge not attached");

      try {
        await b.request({ kind: "engine/set/ground-texture", params: { slot: 0 } });
        await b.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot: 6, path: localPath },
        });
        const afterLocal = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as { groundSlotCustomPaths: string[] };

        let remoteRejected = false;
        try {
          await b.request({
            kind: "engine/set/ground-slot-custom-path",
            params: { slot: 6, path: remotePath },
          });
        } catch {
          remoteRejected = true;
        }
        const afterRemote = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as { groundSlotCustomPaths: string[] };
        return {
          localStored: afterLocal.groundSlotCustomPaths[6],
          remoteRejected,
          afterRemote: afterRemote.groundSlotCustomPaths[6],
        };
      } finally {
        await b.request({ kind: "engine/set/ground-texture", params: { slot: 0 } });
        await b.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot: 6, path: "" },
        });
      }
    }, { localPath, remotePath });

    // Exact guard values: the remote host is refused and the prior local path
    // remains byte-for-byte unchanged. The local assignment is the overreach
    // control that fails if every custom path is rejected.
    expect(result.localStored).toBe(localPath);
    expect(result.remoteRejected).toBe(true);
    expect(result.afterRemote).toBe(localPath);
  } finally {
    await unlink(localPath).catch(() => {});
  }
});

test("ground texture selection reports corrupt custom fallback without rejecting valid local texture", async ({}, testInfo) => {
  await mkdir(testInfo.outputDir, { recursive: true });
  const validPath = testInfo.outputPath("ground-valid.tga");
  const corruptPath = testInfo.outputPath("ground-corrupt.dds");
  await writeFile(validPath, ONE_PIXEL_TGA);
  await writeFile(corruptPath, Buffer.from("not a texture", "utf8"));

  try {
    const result = await page.evaluate(async ({ validPath, corruptPath }) => {
      const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
        .bridge;
      if (!b) throw new Error("window.bridge not attached");
      type SlotResult = { slot: number; applied: boolean };
      const setGroundSlot = (slot: number) =>
        b.request({ kind: "engine/set/ground-texture", params: { slot } }) as Promise<SlotResult>;
      const getGroundState = async () => {
        const snapshot = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as { groundTexture: number; dirty: boolean };
        return {
          groundTexture: snapshot.groundTexture,
          dirty: snapshot.dirty,
        };
      };

      try {
        await setGroundSlot(0);
        await b.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot: 5, path: validPath },
        });
        const valid = await setGroundSlot(5);
        const afterValid = await getGroundState();

        await setGroundSlot(0);
        await b.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot: 5, path: corruptPath },
        });
        // Arrange a nonzero pre-call slot, then clear document dirtiness using
        // the production file/new action. file/new clears live instances and
        // the document baseline but deliberately preserves view/environment
        // state, including ground slot 4 and slot-5's custom path.
        await setGroundSlot(4);
        await b.request({ kind: "file/new", params: {} });
        const beforeCorrupt = await getGroundState();
        const corrupt = await setGroundSlot(5);
        const afterCorrupt = await getGroundState();
        return { valid, afterValid, beforeCorrupt, corrupt, afterCorrupt };
      } finally {
        await setGroundSlot(0);
        await b.request({
          kind: "engine/set/ground-slot-custom-path",
          params: { slot: 5, path: "" },
        });
        await b.request({ kind: "file/new", params: {} });
      }
    }, { validPath, corruptPath });

    // Exact wrong values: a successful dirt fallback must not be reported as
    // slot 5/applied:true. The decodable local TGA is the overreach control.
    expect(result.valid).toEqual({ slot: 5, applied: true });
    expect(result.afterValid.groundTexture).toBe(5);
    expect(result.beforeCorrupt).toEqual({ groundTexture: 4, dirty: false });
    expect(result.corrupt).toEqual({ slot: 0, applied: false });
    expect(result.afterCorrupt).toEqual({ groundTexture: 0, dirty: true });
  } finally {
    await unlink(validPath).catch(() => {});
    await unlink(corruptPath).catch(() => {});
  }
});

test("clearing an active custom skydome path succeeds without hiding real load failures", async ({}, testInfo) => {
  await mkdir(testInfo.outputDir, { recursive: true });
  const texturePath = testInfo.outputPath("active-skydome.bmp");
  const missingPath = testInfo.outputPath("missing-skydome.bmp");
  await writeFile(texturePath, ONE_PIXEL_BMP);

  try {
    const result = await page.evaluate(async ({ texturePath, missingPath }) => {
      const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
        .bridge;
      if (!b) throw new Error("window.bridge not attached");

      try {
        await b.request({
          kind: "engine/set/skydome-custom-path",
          params: { slot: 9, path: texturePath },
        });
        await b.request({ kind: "engine/set/skydome-slot", params: { slot: 9 } });
        const active = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as { skydomeSlot: number };

        let missingRejected = false;
        try {
          await b.request({
            kind: "engine/set/skydome-custom-path",
            params: { slot: 9, path: missingPath },
          });
        } catch {
          missingRejected = true;
        }

        // Recover the active texture before exercising the clear operation.
        await b.request({
          kind: "engine/set/skydome-custom-path",
          params: { slot: 9, path: texturePath },
        });
        let clearResolved = true;
        try {
          await b.request({
            kind: "engine/set/skydome-custom-path",
            params: { slot: 9, path: "" },
          });
        } catch {
          clearResolved = false;
        }
        const cleared = (await b.request({
          kind: "engine/state/snapshot",
          params: {},
        })) as { skydomeCustomPaths: string[] };
        return {
          activeSlot: active.skydomeSlot,
          missingRejected,
          clearResolved,
          clearedPath: cleared.skydomeCustomPaths[0],
        };
      } finally {
        await b.request({ kind: "engine/set/skydome-slot", params: { slot: 0 } });
        await b.request({
          kind: "engine/set/skydome-custom-path",
          params: { slot: 9, path: "" },
        });
      }
    }, { texturePath, missingPath });

    expect(result.activeSlot).toBe(9);
    expect(result.missingRejected).toBe(true);
    expect(result.clearResolved).toBe(true);
    expect(result.clearedPath).toBe("");
  } finally {
    await unlink(texturePath).catch(() => {});
  }
});

test("undo/perform dispatches end-to-end and resolves with a boolean `applied`", async () => {
  // This spec asserts only that the undo/perform handler is wired
  // end-to-end through the bridge (its original Task 2.4 intent).
  // It originally asserted `applied:false` on the premise that no
  // captures existed yet — that premise is stale: undo capture landed
  // with the emitter work (functional coverage lives in
  // undo-navigation.spec.ts), and earlier specs in the shared-host
  // run legitimately seed captures (e.g. atlas-picker.spec.ts commits
  // track-key edits), so `applied` is order-dependent by design.
  const r = await page.evaluate(async () => {
    const b = (window as { bridge?: { request(r: { kind: string; params: object }): Promise<unknown> } })
      .bridge;
    if (!b) throw new Error("window.bridge not attached");
    const undone = (await b.request({
      kind: "undo/perform",
      params: { direction: "undo" },
    })) as { applied?: unknown };
    // Keep the shared host state-neutral: if the undo applied (an
    // earlier spec's capture), redo it so later specs see the state
    // those specs left behind.
    if (undone.applied === true) {
      await b.request({ kind: "undo/perform", params: { direction: "redo" } });
    }
    return undone;
  });
  expect(typeof (r as { applied?: unknown }).applied).toBe("boolean");
});
