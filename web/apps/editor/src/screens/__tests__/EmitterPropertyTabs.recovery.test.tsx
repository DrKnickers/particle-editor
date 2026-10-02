import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { act, cleanup, fireEvent, render as rtlRender, screen } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import type { EmitterPropertiesDto } from "@particle-editor/bridge-schema";
import { EmitterPropertyTabs } from "../EmitterPropertyTabs";
import { makeDefaultEngineState, makeFixtureProperties } from "@/bridge/mock-state";
import { makeBridgeStub, type StubResponses } from "@/test/bridge-stub";
import { __resetStatusFeedbackForTests, useStatusFeedback } from "@/lib/status-feedback";

const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: Error) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

const propertiesFor = (id: number) => ({ ...makeFixtureProperties(id), name: `Emitter ${id}` });
function makeBridge(responses: StubResponses = {}) {
  return makeBridgeStub({ responses: {
    "engine/state/snapshot": { ...makeDefaultEngineState(), selectedEmitterId: 1 },
    "emitters/get-properties": ({ id }) => ({ properties: propertiesFor(id) }),
    "emitters/set-properties": { applied: ["name"], skipped: [] },
    ...responses,
  } });
}
async function flush() {
  await act(async () => { await Promise.resolve(); });
}
function editName(value: string) {
  const input = screen.getByLabelText("Name");
  fireEvent.change(input, { target: { value } });
  fireEvent.blur(input);
}
const name = () => (screen.getByLabelText("Name") as HTMLInputElement).value;

beforeEach(() => {
  __resetStatusFeedbackForTests();
  vi.spyOn(console, "warn").mockImplementation(() => {});
});
afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  __resetStatusFeedbackForTests();
});

describe("EmitterPropertyTabs recovery", () => {
  it("shows a rejected read with Retry and loads current values on retry", async () => {
    let failed = true;
    const retry = deferred<{ properties: EmitterPropertiesDto }>();
    const bridge = makeBridge({ "emitters/get-properties": () => failed
      ? Promise.reject(new Error("read unavailable")) : retry.promise });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    expect(screen.getByRole("alert").textContent).toContain("read unavailable");
    expect(screen.queryByText("Loading…")).toBeNull();
    expect(screen.queryByLabelText("Name")).toBeNull();
    failed = false;
    fireEvent.click(screen.getByRole("button", { name: "Retry" }));
    expect(screen.queryByRole("alert")).toBeNull();
    expect(screen.getByRole("status").textContent).toContain("Loading…");
    await act(async () => { retry.resolve({ properties: propertiesFor(1) }); });
    expect(name()).toBe("Emitter 1");
    expect(screen.queryByRole("alert")).toBeNull();
  });

  it("removes previous fields while a new selection is pending or failed", async () => {
    const pending = deferred<{ properties: EmitterPropertiesDto }>();
    let retried = false;
    const bridge = makeBridge({ "emitters/get-properties": ({ id }) => id === 2 && !retried
      ? pending.promise : { properties: propertiesFor(id) } });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    const oldInput = screen.getByLabelText("Name");
    fireEvent.change(oldInput, { target: { value: "old draft" } });
    bridge.emit("emitters/selected", { id: 2 });
    expect(screen.queryByLabelText("Name")).toBeNull();
    fireEvent.blur(oldInput);
    await act(async () => { pending.reject(new Error("second read failed")); });
    expect(screen.queryByLabelText("Name")).toBeNull();
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/set-properties")).toHaveLength(0);
    retried = true;
    fireEvent.click(screen.getByRole("button", { name: "Retry" }));
    await flush();
    expect(name()).toBe("Emitter 2");
  });

  it("keeps the form and optimistic values during a same-selection tree refresh", async () => {
    const refresh = deferred<{ properties: EmitterPropertiesDto }>();
    let reads = 0;
    const bridge = makeBridge({ "emitters/get-properties": () => ++reads === 1
      ? { properties: propertiesFor(1) } : refresh.promise });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("optimistic");
    await flush();
    const input = screen.getByLabelText("Name");
    bridge.emit("emitters/tree/changed", {});
    expect(screen.getByLabelText("Name")).toBe(input);
    expect(name()).toBe("optimistic");
    await act(async () => { refresh.reject(new Error("refresh failed")); });
    expect(name()).toBe("optimistic");
    expect(screen.getByRole("button", { name: "Retry" })).toBeTruthy();
  });

  it.each(["reject", "skip"] as const)("reports a %s edit once and restores authoritative values", async (result) => {
    const bridge = makeBridge({ "emitters/set-properties": () => result === "reject"
      ? Promise.reject(new Error("write refused")) : { applied: [], skipped: ["name"] } });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("refused draft");
    expect(name()).toBe("refused draft");
    await flush();
    expect(name()).toBe("Emitter 1");
    expect(useStatusFeedback.getState().message).toMatch(result === "reject" ? /failed.*write refused/i : /name/);
    expect(useStatusFeedback.getState().epoch).toBe(1);
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/set-properties")).toHaveLength(1);
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/get-properties")).toHaveLength(2);
  });

  it.each(["resolve", "reject"] as const)("discards a stale same-ID read that later %ss", async (settle) => {
    const old = deferred<{ properties: EmitterPropertiesDto }>();
    let reads = 0;
    const bridge = makeBridge({ "emitters/get-properties": () => ++reads === 1
      ? old.promise : { properties: { ...propertiesFor(1), name: "latest" } } });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    bridge.emit("emitters/tree/changed", {});
    await flush();
    expect(name()).toBe("latest");
    await act(async () => {
      if (settle === "resolve") old.resolve({ properties: propertiesFor(1) });
      else old.reject(new Error("obsolete read"));
    });
    expect(name()).toBe("latest");
    expect(screen.queryByRole("alert")).toBeNull();
  });

  it.each(["resolve", "reject"] as const)("discards an A -> B -> A read that later %ss", async (settle) => {
    const old = deferred<{ properties: EmitterPropertiesDto }>();
    let reads = 0;
    const bridge = makeBridge({ "emitters/get-properties": ({ id }) => ++reads === 1
      ? old.promise : { properties: { ...propertiesFor(id), name: `latest ${id}` } } });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    bridge.emit("emitters/selected", { id: 2 });
    await flush();
    bridge.emit("emitters/selected", { id: 1 });
    await flush();
    await act(async () => {
      if (settle === "resolve") old.resolve({ properties: propertiesFor(1) });
      else old.reject(new Error("obsolete A"));
    });
    expect(name()).toBe("latest 1");
    expect(screen.queryByRole("alert")).toBeNull();
  });

  it.each([2, null])("does not let the selection snapshot overwrite live selection %s", async (id) => {
    const seed = deferred<ReturnType<typeof makeDefaultEngineState>>();
    const bridge = makeBridge({ "engine/state/snapshot": () => seed.promise });
    render(<EmitterPropertyTabs bridge={bridge} />);
    bridge.emit("emitters/selected", { id });
    await flush();
    await act(async () => { seed.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 }); });
    if (id === null) expect(screen.queryByLabelText("Name")).toBeNull();
    else expect(name()).toBe("Emitter 2");
  });

  it("reloads A when an A -> B -> A selection change is batched", async () => {
    let reads = 0;
    const bridge = makeBridge({ "emitters/get-properties": () => ++reads === 1
      ? { properties: propertiesFor(1) } : Promise.reject(new Error("new A read failed")) });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    await act(async () => {
      bridge.emit("emitters/selected", { id: 2 });
      bridge.emit("emitters/selected", { id: 1 });
    });
    expect(screen.queryByLabelText("Name")).toBeNull();
    expect(screen.getByRole("alert").textContent).toContain("new A read failed");
    expect(reads).toBe(2);
  });

  it("clears the previous bridge's fields and ignores its late snapshot", async () => {
    const seed = deferred<ReturnType<typeof makeDefaultEngineState>>();
    const first = makeBridge({ "engine/state/snapshot": () => seed.promise });
    const second = makeBridge({ "engine/state/snapshot": { selectedEmitterId: null } });
    const view = render(<EmitterPropertyTabs bridge={first} />);
    first.emit("emitters/selected", { id: 1 });
    await flush();
    expect(name()).toBe("Emitter 1");
    view.rerender(<EmitterPropertyTabs bridge={second} />);
    expect(screen.queryByLabelText("Name")).toBeNull();
    await act(async () => { seed.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 }); });
    expect(screen.queryByLabelText("Name")).toBeNull();
    expect(second.request.mock.calls.filter(([r]) => r.kind === "emitters/get-properties")).toHaveLength(0);
  });

  it.each(["resolve", "reject"] as const)("discards a previous bridge's property read that %ss", async (settle) => {
    const old = deferred<{ properties: EmitterPropertiesDto }>();
    const first = makeBridge({ "emitters/get-properties": () => old.promise });
    const second = makeBridge({ "emitters/get-properties": () => ({ properties: { ...propertiesFor(1), name: "new bridge" } }) });
    const view = render(<EmitterPropertyTabs bridge={first} />);
    await flush();
    view.rerender(<EmitterPropertyTabs bridge={second} />);
    await flush();
    await act(async () => {
      if (settle === "resolve") old.resolve({ properties: propertiesFor(1) });
      else old.reject(new Error("old bridge read"));
    });
    expect(name()).toBe("new bridge");
    expect(screen.queryByRole("alert")).toBeNull();
  });

  it("hides loaded fields while the next bridge's selection seed is pending", async () => {
    const seed = deferred<ReturnType<typeof makeDefaultEngineState>>();
    const first = makeBridge();
    const second = makeBridge({ "engine/state/snapshot": () => seed.promise });
    const view = render(<EmitterPropertyTabs bridge={first} />);
    await flush();
    expect(name()).toBe("Emitter 1");
    view.rerender(<EmitterPropertyTabs bridge={second} />);
    expect(screen.queryByLabelText("Name")).toBeNull();
    expect(second.request.mock.calls.filter(([r]) => r.kind === "emitters/get-properties")).toHaveLength(0);
    await act(async () => { seed.resolve({ ...makeDefaultEngineState(), selectedEmitterId: null }); });
    expect(screen.getByTestId("emitter-property-tabs-placeholder")).toBeTruthy();
  });

  it.each(["reject", "skip"] as const)("does not let an old %s edit supersede a newer selection's read", async (result) => {
    const edit = deferred<{ applied: string[]; skipped: string[] }>();
    const read = deferred<{ properties: EmitterPropertiesDto }>();
    const bridge = makeBridge({
      "emitters/set-properties": () => edit.promise,
      "emitters/get-properties": ({ id }) => id === 2 ? read.promise : { properties: propertiesFor(id) },
    });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("draft");
    bridge.emit("emitters/selected", { id: 2 });
    await act(async () => {
      if (result === "reject") edit.reject(new Error("old edit"));
      else edit.resolve({ applied: [], skipped: ["name"] });
    });
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/get-properties")).toHaveLength(2);
    await act(async () => { read.resolve({ properties: propertiesFor(2) }); });
    expect(name()).toBe("Emitter 2");
  });

  it("does not let a rejected edit replace a newer same-ID refresh", async () => {
    const edit = deferred<{ applied: string[]; skipped: string[] }>();
    const read = deferred<{ properties: EmitterPropertiesDto }>();
    let reads = 0;
    const bridge = makeBridge({
      "emitters/set-properties": () => edit.promise,
      "emitters/get-properties": () => ++reads === 1 ? { properties: propertiesFor(1) } : read.promise,
    });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("draft");
    bridge.emit("emitters/tree/changed", {});
    await act(async () => { edit.reject(new Error("old edit")); });
    expect(reads).toBe(2);
    await act(async () => { read.resolve({ properties: { ...propertiesFor(1), name: "refreshed" } }); });
    expect(name()).toBe("refreshed");
  });

  it("does not recover an old rejected edit after selection A -> B -> A", async () => {
    const edit = deferred<{ applied: string[]; skipped: string[] }>();
    const read = deferred<{ properties: EmitterPropertiesDto }>();
    let reads = 0;
    const bridge = makeBridge({
      "emitters/set-properties": () => edit.promise,
      "emitters/get-properties": ({ id }) => ++reads < 3 ? { properties: propertiesFor(id) } : read.promise,
    });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("draft");
    bridge.emit("emitters/selected", { id: 2 });
    await flush();
    bridge.emit("emitters/selected", { id: 1 });
    await act(async () => { edit.reject(new Error("old A edit")); });
    expect(reads).toBe(3);
    await act(async () => { read.resolve({ properties: { ...propertiesFor(1), name: "current A" } }); });
    expect(name()).toBe("current A");
  });

  it.each(["resolve", "reject"] as const)("ignores a read that %ss after unmount and removes subscriptions", async (settle) => {
    const read = deferred<{ properties: EmitterPropertiesDto }>();
    const bridge = makeBridge({ "emitters/get-properties": () => read.promise });
    const view = render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    view.unmount();
    await act(async () => {
      if (settle === "resolve") read.resolve({ properties: propertiesFor(1) });
      else read.reject(new Error("late read"));
    });
    expect(bridge.listenerCount("emitters/selected")).toBe(0);
    expect(bridge.listenerCount("emitters/tree/changed")).toBe(0);
    expect(useStatusFeedback.getState().message).toBeNull();
  });

  it("does not recover a rejected edit after unmount", async () => {
    const edit = deferred<{ applied: string[]; skipped: string[] }>();
    const bridge = makeBridge({ "emitters/set-properties": () => edit.promise });
    const view = render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    editName("draft");
    view.unmount();
    await act(async () => { edit.reject(new Error("late edit")); });
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/get-properties")).toHaveLength(1);
  });

  it.each(["cancel", "reject"] as const)("keeps browse %s from committing, reporting only rejection", async (result) => {
    const bridge = makeBridge({ "textures/browse": () => result === "cancel"
      ? { filename: "" } : Promise.reject(new Error("browse unavailable")) });
    render(<EmitterPropertyTabs bridge={bridge} />);
    await flush();
    fireEvent.mouseDown(screen.getByTestId("tab-trigger-appearance"), { button: 0, ctrlKey: false });
    await flush();
    fireEvent.click(screen.getByRole("button", { name: "Browse for Color:" }));
    await flush();
    expect(bridge.request.mock.calls.filter(([r]) => r.kind === "emitters/set-properties")).toHaveLength(0);
    if (result === "cancel") expect(useStatusFeedback.getState().message).toBeNull();
    else expect(useStatusFeedback.getState().message).toMatch(/failed.*browse unavailable/i);
  });
});
