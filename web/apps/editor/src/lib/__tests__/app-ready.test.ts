// app-ready — the React→host first-paint handshake used by --capture mode to
// gate the composite-window screenshot on real chrome being painted.
//
// Scope note (honest): jsdom cannot prove the signal fires *after first paint*
// in AppShell — that timing claim is owned by the host.log `ui-ready=1`
// assertion in tasks/capture-refobj.ps1. These tests cover only the shim's
// contract: it posts the exact wire the host needles for, no-ops without
// WebView2, and never throws.

import { describe, it, expect, vi, afterEach } from "vitest";
import { APP_READY_KIND, signalAppReady } from "../app-ready";

type PostMessage = (s: string) => void;

// Mirror the NativeBridge test's mock pattern (bridge/__tests__/native.test.ts):
// install a minimal window.chrome.webview, swap postMessage per test.
function installWebview(postMessage: PostMessage) {
  (window as unknown as { chrome: unknown }).chrome = {
    webview: { postMessage, addEventListener: vi.fn() },
  };
}

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
});

describe("signalAppReady", () => {
  it("posts exactly {\"kind\":\"app/ready\"} when chrome.webview is present", () => {
    const post = vi.fn();
    installWebview(post);

    signalAppReady();

    expect(post).toHaveBeenCalledTimes(1);
    // Assert the literal wire — the host extracts the `kind` value from this
    // message and compares it to `app/ready`, so the exact serialized shape is
    // the contract.
    expect(post).toHaveBeenCalledWith('{"kind":"app/ready"}');
  });

  it("is a no-op and does not throw when chrome.webview is absent (browser/mock dev)", () => {
    // No installWebview — window.chrome is undefined.
    expect(() => signalAppReady()).not.toThrow();
  });

  it("does not throw when chrome.webview exists but postMessage is missing", () => {
    (window as unknown as { chrome: unknown }).chrome = { webview: {} };
    expect(() => signalAppReady()).not.toThrow();
  });

  it("exposes the kind as a stable constant (anti-drift with the host comparison)", () => {
    // The host compares the extracted kind to the literal "app/ready"; if this
    // constant ever drifts, the host gate silently stops firing. Pin it here.
    expect(APP_READY_KIND).toBe("app/ready");
  });
});
