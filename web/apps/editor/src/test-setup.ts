// Vitest setup file: extend expect with jest-dom matchers.
// Imported by all test suites via vitest.config.ts setupFiles.
import "@testing-library/jest-dom/vitest";
import { cleanup } from "@testing-library/react";
import { afterEach, vi } from "vitest";

// Automatically clean up the DOM after each test, matching Jest/jsdom
// default behaviour. Without this, multiple renders in the same test file
// accumulate in the shared jsdom body and cause "found multiple elements"
// errors on the second and later tests.
// Also clear localStorage so per-component persistence (Force Align toggle,
// palette presets, ThemeToggle choice, etc.) doesn't leak across tests.
afterEach(() => {
  cleanup();
  if (typeof window !== "undefined" && window.localStorage) {
    window.localStorage.clear();
  }
});

// jsdom doesn't implement ResizeObserver — Radix Popover uses it internally.
// Stub it with a no-op implementation so Popover mounts without throwing.
if (typeof ResizeObserver === "undefined") {
  globalThis.ResizeObserver = class ResizeObserver {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
}

// jsdom doesn't implement Element.prototype.scrollIntoView (used by
// Radix Select's focus management). Stub it so Select renders without error.
if (!Element.prototype.scrollIntoView) {
  Element.prototype.scrollIntoView = vi.fn();
}

// jsdom doesn't implement window.PointerEvent fully. Radix Select uses
// pointer events for its interaction model. Provide a basic stub.
if (!globalThis.PointerEvent) {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).PointerEvent = class PointerEvent extends MouseEvent {
    public pointerId: number;
    constructor(type: string, params: PointerEventInit = {}) {
      super(type, params);
      this.pointerId = params.pointerId ?? 1;
    }
  };
}

// HTMLElement.prototype.hasPointerCapture / setPointerCapture —
// required by Radix for pointer-capture interactions.
if (!HTMLElement.prototype.hasPointerCapture) {
  HTMLElement.prototype.hasPointerCapture = vi.fn(() => false);
}
if (!HTMLElement.prototype.setPointerCapture) {
  HTMLElement.prototype.setPointerCapture = vi.fn();
}
if (!HTMLElement.prototype.releasePointerCapture) {
  HTMLElement.prototype.releasePointerCapture = vi.fn();
}

// jsdom in this config doesn't expose window.localStorage. Stub it with
// an in-memory Map-backed shim so components using localStorage (e.g.
// ThemeToggle persistence, palette-store) can round-trip in unit tests.
// Each test that touches localStorage should clear it in beforeEach.
if (typeof window.localStorage === "undefined") {
  const store = new Map<string, string>();
  Object.defineProperty(window, "localStorage", {
    configurable: true,
    value: {
      getItem: (k: string) => (store.has(k) ? store.get(k)! : null),
      setItem: (k: string, v: string) => { store.set(k, String(v)); },
      removeItem: (k: string) => { store.delete(k); },
      clear: () => { store.clear(); },
      key: (i: number) => Array.from(store.keys())[i] ?? null,
      get length() { return store.size; },
    },
  });
}

// jsdom doesn't implement window.matchMedia. Stub it with a no-match
// shim (so prefers-color-scheme reads always return false → default
// to light theme in tests; ThemeToggle still exercises localStorage
// persistence even if the matchMedia branch is dead).
if (typeof window.matchMedia === "undefined") {
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: (query: string) => ({
      matches: false,
      media: query,
      onchange: null,
      addListener: vi.fn(),    // deprecated; some libs still call this
      removeListener: vi.fn(),
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      dispatchEvent: vi.fn(() => false),
    }),
  });
}
