// Host → page ui/* messages: the --record control channel that rides the same
// chrome.webview "message" stream as the bridge's res/evt envelopes but is not
// part of the request/event schema (NativeBridge ignores it). `UiMessage` is
// its contract; `onUiMessage` is the single webview listener that parses each
// message once and routes it by `type` (React components use useHostMessage).
//
// Adding a ui/* message: add its arm to `UiMessage`, its parser to `PARSERS`
// (validate every field — a malformed push must be dropped, not acted on), and
// the type to the host's allowlist (src/host/ClipTimeline.h) if a timeline
// sends it.

// The global `window.chrome.webview` type lives in ./native.ts.
import type {} from "./native";
import { EventHub } from "./event-hub";
import { coerceMessage } from "./wire";
import { isRecordHeadlessMessage, parseCursorMessage, type CursorMessage } from "@/lib/record-cursor-bridge";
import {
  parseCursorTickMessage,
  parseCursorTrackMessage,
  type RecordCursorKey,
  type RecordCursorTick,
} from "@/lib/record-cursor-track";
import {
  parseFocusChannelMessage,
  parseHidePanelMessage,
  parseOpenPickerMessage,
  parsePoseDragMessage,
  parseRevealCurveChannelMessage,
  parseSelectKeyMessage,
  parseSetPickerCollapseMessage,
  parseSetPickerSearchMessage,
  parseSetThemeMessage,
  parseShowPanelMessage,
} from "@/lib/record-control-messages";

type Parsed<F extends (data: unknown) => unknown> = NonNullable<ReturnType<F>>;

export type UiMessage =
  // Synthetic cursor (record-cursor-bridge.ts / record-cursor-track.ts).
  | ({ type: "ui/cursor"; frame: number } & CursorMessage)
  | { type: "ui/record-headless" }
  | { type: "ui/cursor-track"; keys: RecordCursorKey[] }
  | ({ type: "ui/cursor-tick" } & RecordCursorTick)
  // UI steering (record-control-messages.ts).
  | { type: "ui/set-theme"; theme: Parsed<typeof parseSetThemeMessage> }
  | { type: "ui/focus-channel"; channel: string }
  | { type: "ui/reveal-curve-channel"; channel: string }
  | { type: "ui/select-key"; track: string; time: number }
  | { type: "ui/hide-panel" }
  | ({ type: "ui/show-panel" } & Parsed<typeof parseShowPanelMessage>)
  | ({ type: "ui/pose-drag" } & Parsed<typeof parsePoseDragMessage>)
  | ({ type: "ui/open-picker" } & Parsed<typeof parseOpenPickerMessage>)
  | { type: "ui/set-picker-search"; text: string }
  | { type: "ui/picker-collapse"; keys: string[] };

export type UiMessageType = UiMessage["type"];
export type UiMessageOf<T extends UiMessageType> = Extract<UiMessage, { type: T }>;

type Body<T extends UiMessageType> = Omit<UiMessageOf<T>, "type">;

const PARSERS: { [T in UiMessageType]: (m: Record<string, unknown>) => Body<T> | null } = {
  "ui/cursor": (m) => {
    const c = parseCursorMessage(m);
    return c && { ...c, frame: typeof m.frame === "number" ? m.frame : 0 };
  },
  "ui/record-headless": (m) => (isRecordHeadlessMessage(m) ? {} : null),
  "ui/cursor-track": (m) => {
    const keys = parseCursorTrackMessage(m);
    return keys && { keys };
  },
  "ui/cursor-tick": parseCursorTickMessage,
  "ui/set-theme": (m) => {
    const theme = parseSetThemeMessage(m);
    return theme && { theme };
  },
  "ui/focus-channel": (m) => {
    const channel = parseFocusChannelMessage(m);
    return channel === null ? null : { channel };
  },
  "ui/reveal-curve-channel": (m) => {
    const channel = parseRevealCurveChannelMessage(m);
    return channel === null ? null : { channel };
  },
  "ui/select-key": parseSelectKeyMessage,
  "ui/hide-panel": (m) => (parseHidePanelMessage(m) ? {} : null),
  "ui/show-panel": parseShowPanelMessage,
  "ui/pose-drag": parsePoseDragMessage,
  "ui/open-picker": parseOpenPickerMessage,
  "ui/set-picker-search": parseSetPickerSearchMessage,
  "ui/picker-collapse": parseSetPickerCollapseMessage,
};

/** Parse a host message into a validated `UiMessage`, or null (not a ui/*
 *  message, an unknown ui/* type, or a malformed one). Accepts the object
 *  (PostWebMessageAsJson) and string (PostWebMessageAsString) forms. */
export function parseUiMessage(data: unknown): UiMessage | null {
  const m = coerceMessage(data);
  if (!m || typeof m.type !== "string" || !Object.hasOwn(PARSERS, m.type)) return null;
  const type = m.type as UiMessageType;
  const body = PARSERS[type](m);
  return body ? ({ ...body, type } as UiMessage) : null;
}

type WebView = NonNullable<NonNullable<Window["chrome"]>["webview"]>;

const hub = new EventHub<{ [T in UiMessageType]: UiMessageOf<T> }>("ui-message");
let attachedTo: WebView | null = null;
let subscriptions = 0;

function dispatch(e: { data: unknown }): void {
  const msg = parseUiMessage(e.data);
  if (msg) hub.emit(msg.type, msg);
}

function detach(): void {
  attachedTo?.removeEventListener?.("message", dispatch);
  attachedTo = null;
}

/** Subscribe to one ui/* message type. Returns an idempotent unsubscribe; a
 *  no-op outside WebView2 (browser dev, jsdom without a fake webview). The
 *  webview listener is attached on the first subscription and removed with
 *  the last, and re-attached if `chrome.webview` has been replaced. */
export function onUiMessage<T extends UiMessageType>(
  type: T,
  handler: (msg: UiMessageOf<T>) => void,
): () => void {
  const webview = window.chrome?.webview;
  if (!webview?.addEventListener) return () => {};
  if (attachedTo !== webview) {
    detach();
    webview.addEventListener("message", dispatch);
    attachedTo = webview;
  }
  const off = hub.on(type, handler);
  subscriptions += 1;
  let active = true;
  return () => {
    if (!active) return;
    active = false;
    off();
    subscriptions -= 1;
    if (subscriptions === 0) detach();
  };
}
