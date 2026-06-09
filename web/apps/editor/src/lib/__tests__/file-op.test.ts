import { describe, it, expect, beforeEach } from "vitest";
import { runFileOp, useFileOpErrorStore } from "@/lib/file-op";
import type { Bridge } from "@particle-editor/bridge-schema";

function fakeBridge(result: unknown): Bridge {
  return { request: async () => result as never, on: () => () => {} } as Bridge;
}

beforeEach(() => useFileOpErrorStore.setState({ message: null }));

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
});
