// useHostMessage: the React side of the ui/* message hub (bridge/ui-message.ts).
// Covers StrictMode's mount/unmount/mount (exactly one webview listener stays),
// the latest-handler ref, and detaching on unmount.

import { StrictMode } from "react";
import { describe, it, expect, beforeEach, afterEach } from "vitest";
import { render, act } from "@testing-library/react";
import { useHostMessage } from "../use-host-message";

type Listener = (e: { data: unknown }) => void;
let listeners: Listener[];

beforeEach(() => {
  listeners = [];
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      addEventListener: (ev: string, h: Listener) => { if (ev === "message") listeners.push(h); },
      removeEventListener: (ev: string, h: Listener) => {
        if (ev === "message") listeners = listeners.filter((l) => l !== h);
      },
    },
  };
});

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
});

const deliver = (data: unknown) => act(() => listeners.forEach((l) => l({ data })));

function Probe({ onText }: { onText: (text: string) => void }) {
  useHostMessage("ui/set-picker-search", (msg) => onText(msg.text));
  return null;
}

describe("useHostMessage", () => {
  it("keeps exactly one webview listener through StrictMode's double mount", () => {
    const seen: string[] = [];
    const { unmount } = render(
      <StrictMode>
        <Probe onText={(t) => seen.push(t)} />
      </StrictMode>,
    );
    expect(listeners).toHaveLength(1);
    deliver({ type: "ui/set-picker-search", text: "AT-AT" });
    expect(seen).toEqual(["AT-AT"]);
    unmount();
    expect(listeners).toHaveLength(0);
  });

  it("always runs the latest handler without resubscribing", () => {
    const first: string[] = [];
    const second: string[] = [];
    const { rerender, unmount } = render(<Probe onText={(t) => first.push(t)} />);
    const attached = listeners[0];
    rerender(<Probe onText={(t) => second.push(t)} />);
    expect(listeners).toEqual([attached]);
    deliver({ type: "ui/set-picker-search", text: "X-wing" });
    expect(first).toEqual([]);
    expect(second).toEqual(["X-wing"]);
    unmount();
  });
});
