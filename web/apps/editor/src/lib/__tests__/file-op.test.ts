import { describe, it, expect, beforeEach } from "vitest";
import { runFileOp, useFileOpErrorStore } from "@/lib/file-op";
import type { Bridge } from "@particle-editor/bridge-schema";

function fakeBridge(result: unknown): Bridge {
  return { request: async () => result as never, on: () => () => {} } as Bridge;
}

/** Bridge that answers each request kind from a map (so file/open and the
 *  follow-up emitters/list can return different shapes). */
function bridgeByKind(byKind: Record<string, unknown>): Bridge {
  return {
    request: async (req: { kind: string }) => byKind[req.kind] as never,
    on: () => () => {},
  } as Bridge;
}

beforeEach(() => useFileOpErrorStore.setState({ message: null, title: null }));

describe("runFileOp", () => {
  it("surfaces a real IO failure", async () => {
    await runFileOp(fakeBridge({ ok: false, error: "save failed" }), { kind: "file/save", params: {} });
    expect(useFileOpErrorStore.getState().message).toContain("Couldn't save the file");
  });

  it("stays silent on user-cancel", async () => {
    await runFileOp(fakeBridge({ ok: false, error: "user-cancelled" }), { kind: "file/save", params: {} });
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });

  it("stays silent on success", async () => {
    await runFileOp(fakeBridge({ ok: true, path: "x.alo" }), { kind: "file/save", params: {} });
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });

  it("includes a non-generic error detail", async () => {
    await runFileOp(fakeBridge({ ok: false, error: "C:\\x.alo is read-only" }), { kind: "file/save", params: {} });
    expect(useFileOpErrorStore.getState().message).toContain("read-only");
  });

  it("opening an .alo with no emitters shows the model notice", async () => {
    const bridge = bridgeByKind({
      "file/open": { ok: true, path: "model.alo" },
      "emitters/list": { root: { children: [] } },
    });
    await runFileOp(bridge, { kind: "file/open", params: {} });
    expect(useFileOpErrorStore.getState().message).toMatch(/no particle emitters/i);
    expect(useFileOpErrorStore.getState().title).toBe("No particle emitters");
  });

  it("opening an .alo that has emitters shows no notice", async () => {
    const bridge = bridgeByKind({
      "file/open": { ok: true, path: "effect.alo" },
      "emitters/list": { root: { children: [{ id: 1 }] } },
    });
    await runFileOp(bridge, { kind: "file/open", params: {} });
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });

  it("a cancelled open fetches no emitters and shows no notice", async () => {
    const calls: string[] = [];
    const bridge: Bridge = {
      request: async (req: { kind: string }) => {
        calls.push(req.kind);
        return { ok: false, error: "user-cancelled" } as never;
      },
      on: () => () => {},
    } as Bridge;
    await runFileOp(bridge, { kind: "file/open", params: {} });
    expect(calls).toEqual(["file/open"]); // the list round-trip must NOT fire
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });

  it("a failed emitters/list never blocks or fails the open", async () => {
    const bridge: Bridge = {
      request: async (req: { kind: string }) => {
        if (req.kind === "file/open") return { ok: true, path: "x.alo" } as never;
        throw new Error("list blew up");
      },
      on: () => () => {},
    } as Bridge;
    const r = await runFileOp(bridge, { kind: "file/open", params: {} });
    expect(r.ok).toBe(true);
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });

  it("surfaces a REJECTED request and re-throws (release-audit #11)", async () => {
    const bridge: Bridge = {
      request: async () => { throw new Error("bridge offline"); },
      on: () => () => {},
    } as Bridge;
    await expect(
      runFileOp(bridge, { kind: "file/save", params: {} }),
    ).rejects.toThrow("bridge offline");
    // The rejection is surfaced in the SAME error store (not swallowed), so the
    // caller can keep its prompt open instead of silently abandoning the op.
    expect(useFileOpErrorStore.getState().message).toContain("Couldn't save the file");
    expect(useFileOpErrorStore.getState().message).toContain("bridge offline");
  });
});
