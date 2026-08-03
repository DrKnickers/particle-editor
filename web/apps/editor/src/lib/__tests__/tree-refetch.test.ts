import type { Bridge, Request, ResponseFor } from "@particle-editor/bridge-schema";
import { describe, expect, it, vi } from "vitest";

import { requestTreeRefetch } from "../tree-refetch";

function makeBridge() {
  const request = vi.fn(<R extends Request>(req: R) =>
    Promise.resolve({ request: req } as unknown as ResponseFor<R>),
  );
  const bridge = {
    request,
    on: vi.fn(),
  } as unknown as Bridge;
  return { bridge, request };
}

describe("requestTreeRefetch", () => {
  it("collapses N callers in the same tick to one bridge request", async () => {
    const { bridge, request } = makeBridge();
    const req = { kind: "emitters/get-properties", params: { id: 7 } } as const;

    const first = requestTreeRefetch(bridge, req);
    const second = requestTreeRefetch(bridge, req);
    const third = requestTreeRefetch(bridge, req);

    expect(first).toBe(second);
    expect(second).toBe(third);
    expect(request).toHaveBeenCalledTimes(1);
    await expect(first).resolves.toEqual({ request: req });
  });

  it("fires separate bridge requests in different ticks", async () => {
    const { bridge, request } = makeBridge();
    const req = { kind: "emitters/get-properties", params: { id: 7 } } as const;

    const first = requestTreeRefetch(bridge, req);
    await Promise.resolve();
    const second = requestTreeRefetch(bridge, req);

    expect(first).not.toBe(second);
    expect(request).toHaveBeenCalledTimes(2);
  });

  it("does not merge distinct request kinds or params", () => {
    const { bridge, request } = makeBridge();

    void requestTreeRefetch(bridge, { kind: "emitters/get-properties", params: { id: 7 } });
    void requestTreeRefetch(bridge, { kind: "emitters/get-properties", params: { id: 8 } });
    void requestTreeRefetch(bridge, { kind: "emitters/get-tracks", params: { id: 7 } });

    expect(request).toHaveBeenCalledTimes(3);
  });
});
