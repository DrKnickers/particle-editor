// Shared, typed Bridge stub for Vitest suites (audit MW4).
//
// Replaces the per-file `make*Bridge` factories that each hand-rolled
// `{ request: vi.fn(), on: vi.fn() } as unknown as Bridge`. Responses are keyed
// by request kind and typed against the schema's ResponseMap, so a stub answer
// for "engine/state/snapshot" must look like (part of) an EngineStateDto; the
// `on` side records every subscriber per event kind (like the real EventHub)
// and `emit()` delivers a typed `{ kind, payload }` event inside act().
//
// Lives under src/test/ (not __tests__/) so Vitest never collects it as a
// suite; it is test-support code only and excluded from coverage.

import type {
  Bridge,
  Event,
  EventKind,
  EventOf,
  Request,
  ResponseMap,
} from "@particle-editor/bridge-schema";
import { act } from "@testing-library/react";
import { vi, type Mock } from "vitest";

type Kind = Request["kind"];
type ParamsOf<K extends Kind> = Extract<Request, { kind: K }>["params"];

/** Object-shaped values may be partial: a stub supplies only the fields the
 *  code under test reads (the snapshot DTO alone has dozens). */
type Loose<T> = T extends readonly unknown[] ? T : T extends object ? Partial<T> : T;

/** A canned response for one request kind. */
export type StubResponse<K extends Kind> = Loose<ResponseMap[K]>;

/** Per-kind responses: a value, or a function of the request params (return a
 *  rejected promise or throw to simulate a transport failure). */
export type StubResponses = {
  [K in Kind]?:
    | StubResponse<K>
    | ((params: ParamsOf<K>) => StubResponse<K> | Promise<StubResponse<K>>);
};

export type BridgeStubOptions = {
  responses?: StubResponses;
  /** Resolved for any kind not in `responses`. Defaults to `{}`. */
  fallback?: unknown;
};

export type BridgeStub = Bridge & {
  request: Mock<(req: Request) => Promise<unknown>>;
  on: Mock<(kind: EventKind, handler: (event: Event) => void) => () => void>;
  /** Deliver `{ kind, payload }` to every current subscriber of `kind`, in act(). */
  emit<K extends EventKind>(kind: K, payload: Loose<EventOf<K>["payload"]>): void;
  /** Number of live (not yet unsubscribed) handlers for `kind`. */
  listenerCount(kind: EventKind): number;
};

export function makeBridgeStub(opts: BridgeStubOptions = {}): BridgeStub {
  const responses = (opts.responses ?? {}) as Record<
    string,
    unknown | ((params: unknown) => unknown)
  >;
  const fallback = "fallback" in opts ? opts.fallback : {};
  const handlers = new Map<EventKind, Set<(event: Event) => void>>();

  const request = vi.fn((req: Request): Promise<unknown> => {
    const r = responses[req.kind];
    if (r === undefined) return Promise.resolve(fallback);
    if (typeof r === "function") {
      try {
        return Promise.resolve((r as (params: unknown) => unknown)(req.params));
      } catch (err) {
        return Promise.reject(err);
      }
    }
    return Promise.resolve(r);
  });

  const on = vi.fn((kind: EventKind, handler: (event: Event) => void) => {
    let set = handlers.get(kind);
    if (!set) handlers.set(kind, (set = new Set()));
    set.add(handler);
    return () => {
      handlers.get(kind)?.delete(handler);
    };
  });

  const emit = (kind: EventKind, payload: unknown): void => {
    const event = { kind, payload } as Event;
    act(() => {
      for (const h of Array.from(handlers.get(kind) ?? [])) h(event);
    });
  };

  const listenerCount = (kind: EventKind): number => handlers.get(kind)?.size ?? 0;

  return { request, on, emit, listenerCount } as unknown as BridgeStub;
}
