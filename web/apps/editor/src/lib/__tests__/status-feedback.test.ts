// Unit tests for the status-feedback store + announceWhenOk (F4).
import { describe, it, expect, beforeEach } from "vitest";
import {
  announceWhenOk,
  useStatusFeedback,
  __resetStatusFeedbackForTests,
} from "../status-feedback";

beforeEach(() => {
  __resetStatusFeedbackForTests();
});

const flush = () => new Promise((r) => setTimeout(r, 0));

describe("status-feedback", () => {
  it("announce sets the message and bumps the epoch (latest wins)", () => {
    useStatusFeedback.getState().announce("one");
    useStatusFeedback.getState().announce("two");
    expect(useStatusFeedback.getState().message).toBe("two");
    expect(useStatusFeedback.getState().epoch).toBe(2);
  });

  it("clear() is epoch-guarded: a stale timer cannot clear a newer message", () => {
    useStatusFeedback.getState().announce("one"); // epoch 1
    useStatusFeedback.getState().announce("two"); // epoch 2
    useStatusFeedback.getState().clear(1); // stale
    expect(useStatusFeedback.getState().message).toBe("two");
    useStatusFeedback.getState().clear(2); // current
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("announceWhenOk announces after a plain resolution", async () => {
    announceWhenOk(Promise.resolve({}), "did it");
    await flush();
    expect(useStatusFeedback.getState().message).toBe("did it");
  });

  it("announceWhenOk stays SILENT on ok:false (refused mutation)", async () => {
    announceWhenOk(Promise.resolve({ ok: false }), "nope");
    await flush();
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("announceWhenOk stays SILENT on rejection", async () => {
    announceWhenOk(Promise.reject(new Error("boom")), "nope");
    await flush();
    expect(useStatusFeedback.getState().message).toBeNull();
  });
});
