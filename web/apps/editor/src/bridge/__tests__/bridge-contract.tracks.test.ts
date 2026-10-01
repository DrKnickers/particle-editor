// Contract tests for MockBridge — curve tracks: get/set/add/delete keys, interpolation, and lock aliasing.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — emitters track ops", () => {
  // ─── emitters/get-tracks ─────────────────────
  //
  // Read-only DTO contract. The wire shape is always 7 tracks in
  // `TRACK_NAMES` order; an unknown id returns empty-keys placeholders
  // rather than an error so the React panel can render a "no data"
  // stub without special-casing failure.

  it("emitters/get-tracks returns 7 tracks in TRACK_NAMES order", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },  // Smoke (fixture root)
    });
    expect(r.tracks).toHaveLength(7);
    expect(r.tracks.map((t) => t.name)).toEqual([
      "red", "green", "blue", "alpha",
      "scale", "index", "rotationSpeed",
    ]);
    for (const t of r.tracks) {
      expect(["linear", "smooth", "step"]).toContain(t.interpolation);
      expect(Array.isArray(t.keys)).toBe(true);
      // Keys are sorted ascending by time.
      for (let i = 1; i < t.keys.length; i++) {
        expect(t.keys[i]!.time).toBeGreaterThanOrEqual(t.keys[i - 1]!.time);
      }
    }
    // Fixture sanity: alpha track has the classic fade-in / fade-out
    // four-key shape used in the panel screenshots.
    const alpha = r.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys).toHaveLength(4);
  });

  it("emitters/get-tracks returns 7 empty-keys tracks for an unknown id", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 9999 },
    });
    expect(r.tracks).toHaveLength(7);
    for (const t of r.tracks) {
      expect(t.keys).toEqual([]);
    }
  });

  // ─── track mutations ────────────
  //
  // delete-track-keys removes the named keys (silently skipping border
  // keys — first + last in time order); set-track-interpolation
  // overrides the track's interpolation type. Both round-trip through
  // a subsequent `emitters/get-tracks` to prove the overlay is wired.

  it("emitters/delete-track-keys removes the specified non-border key from the track", async () => {
    const b = new MockBridge();
    // Alpha on id=0 (Smoke) has 4 keys: time 0, 20, 80, 100.
    // Border keys are time 0 and 100. Deleting time=20 should leave
    // 3 keys (times 0, 80, 100); deleting time=0 is a no-op (border).
    await b.request({
      kind: "emitters/delete-track-keys",
      params: { id: 0, track: "alpha", times: [20] },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys).toHaveLength(3);
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 80, 100]);

    // Border-key delete attempt is a silent no-op.
    await b.request({
      kind: "emitters/delete-track-keys",
      params: { id: 0, track: "alpha", times: [0, 100] },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys).toHaveLength(3);
  });

  it("emitters/set-track-interpolation updates the track's interpolation type", async () => {
    const b = new MockBridge();
    // The Smoke fixture's alpha track is "linear" by default. Flip to
    // "smooth" and re-read.
    await b.request({
      kind: "emitters/set-track-interpolation",
      params: { id: 0, track: "alpha", interpolation: "smooth" },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.interpolation).toBe("smooth");

    // Round-trip to "step".
    await b.request({
      kind: "emitters/set-track-interpolation",
      params: { id: 0, track: "alpha", interpolation: "step" },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    expect(after2.tracks.find((t) => t.name === "alpha")?.interpolation).toBe("step");
  });

  // ─── track key mutations ────────────────────
  //
  // set-track-key moves an existing key (erase oldTime, insert
  // newTime/newValue). Border keys silently fix newTime = oldTime.
  // add-track-key inserts a new key, dedupes by epsilon-bump on
  // exact-time collisions.

  it("emitters/set-track-key moves an interior key to (newTime, newValue)", async () => {
    const b = new MockBridge();
    // Alpha on Smoke (id=0) has 4 keys: time 0, 20, 80, 100. Move
    // time=20 → (25, 0.5).
    await b.request({
      kind: "emitters/set-track-key",
      params: {
        id: 0,
        track: "alpha",
        oldTime: 20,
        newTime: 25,
        newValue: 0.5,
      },
    });
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 25, 80, 100]);
    const moved = alpha?.keys.find((k) => k.time === 25);
    expect(moved?.value).toBe(0.5);
    // Border-key time change is silently downgraded to value-only.
    await b.request({
      kind: "emitters/set-track-key",
      params: {
        id: 0,
        track: "alpha",
        oldTime: 0,
        newTime: 5,           // would change the border key's time
        newValue: 0.25,
      },
    });
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys[0]?.time).toBe(0);    // time unchanged
    expect(alpha2?.keys[0]?.value).toBe(0.25); // value moved
  });

  it("emitters/add-track-key inserts a new key and returns the actual inserted time", async () => {
    const b = new MockBridge();
    // Insert time=40 on Alpha (which has 0, 20, 80, 100 by default).
    const r = await b.request({
      kind: "emitters/add-track-key",
      params: { id: 0, track: "alpha", time: 40, value: 0.6 },
    });
    expect(r.time).toBe(40);
    expect(r.value).toBe(0.6);
    const after = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha = after.tracks.find((t) => t.name === "alpha");
    expect(alpha?.keys.map((k) => k.time)).toEqual([0, 20, 40, 80, 100]);
    // Collision at exact time → host bumps by 0.001.
    const r2 = await b.request({
      kind: "emitters/add-track-key",
      params: { id: 0, track: "alpha", time: 40, value: 0.9 },
    });
    expect(r2.time).toBeCloseTo(40.001, 4);
    const after2 = await b.request({
      kind: "emitters/get-tracks",
      params: { id: 0 },
    });
    const alpha2 = after2.tracks.find((t) => t.name === "alpha");
    expect(alpha2?.keys.length).toBe(6);
  });
});

describe("emitters/set-track-lock — read aliasing (native parity)", () => {
  // These tests pin the DERIVE-AT-READ semantics: a locked channel is a live
  // view of its master's CANONICAL content, not a stale copy taken at lock
  // time. Three invariants that must all hold simultaneously:
  //   1. Master edits AFTER the lock are visible through the follower.
  //   2. Unlock restores the follower's own pre-lock canonical content.
  //   3. Chained locks (blue→green while green→red) present green's
  //      CANONICAL content, not the mirror green currently displays.

  it("Mirror after master edit: green reflects red keys added after lock", async () => {
    const b = new MockBridge();
    // Lock green → red on emitter id 1.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Add a key on RED after the lock (time=42, value=0.42).
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 42, value: 0.42 },
    });
    const r = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const green = r.tracks.find((t) => t.name === "green");
    const red   = r.tracks.find((t) => t.name === "red");
    expect(green?.lockedTo).toBe("red");
    // green must mirror red's current keys exactly.
    expect(green?.keys).toEqual(red?.keys);
    // The key added after the lock must appear in green.
    expect(green?.keys.some((k) => k.time === 42)).toBe(true);
  });

  it("Unlock restores: green regains its own pre-lock canonical keys", async () => {
    const b = new MockBridge();
    // Capture green's canonical keys BEFORE locking.
    const before = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const preLockGreenKeys = before.tracks.find((t) => t.name === "green")!.keys;

    // Lock green → red.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Add a key on red so the locked state diverges from the pre-lock snapshot.
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 77, value: 0.77 },
    });

    // Unlock green (lockTo: null).
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: null },
    });

    const after = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const greenAfter = after.tracks.find((t) => t.name === "green");
    expect(greenAfter?.lockedTo).toBeNull();
    // green's keys must match the pre-lock snapshot exactly.
    expect(greenAfter?.keys).toEqual(preLockGreenKeys);
  });

  it("Chained lock: blue→green presents green's CANONICAL keys (not red mirror)", async () => {
    const b = new MockBridge();
    // Capture green's pre-lock canonical keys.
    const before = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const greenCanonicalKeys = before.tracks.find((t) => t.name === "green")!.keys;

    // Lock green → red (green now displays red's content).
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "green", lockTo: "red" },
    });
    // Mutate red so green's displayed mirror diverges from green's canonical.
    await b.request({
      kind: "emitters/add-track-key",
      params: { id: 1, track: "red", time: 55, value: 0.55 },
    });

    // Lock blue → green. The alias must follow green's CANONICAL content
    // (what trackContents[green] holds), NOT the red mirror green displays.
    await b.request({
      kind: "emitters/set-track-lock",
      params: { id: 1, channel: "blue", lockTo: "green" },
    });

    const r = await b.request({ kind: "emitters/get-tracks", params: { id: 1 } });
    const blue = r.tracks.find((t) => t.name === "blue");
    expect(blue?.lockedTo).toBe("green");
    // blue must show green's pre-lock (canonical) keys, NOT red's keys.
    expect(blue?.keys).toEqual(greenCanonicalKeys);
  });
});
