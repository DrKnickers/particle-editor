// Vitest contract tests for the UIA accessibility-tree normalizer
// (Phase 3 a11y close-out).
//
// The normalizer is the pure-TS foundation under the snapshot tests:
// before any golden comparison, raw UIA trees are run through
// `normalize()` so volatile fields (BoundingRectangle, RuntimeId, …)
// don't bake into the golden, child order is deterministic, and the
// Chromium chrome wrappers from the probe (Chrome_WidgetWin_1,
// BrowserRootView, NonClientView, Intermediate D3D Window) are
// flattened away.
//
// These tests cover each of those three transforms plus the recursive
// case to ensure descendant nodes get the same treatment as the root.

import { describe, it, expect } from "vitest";
import { normalize } from "../../../tests/helpers/a11y-normalizer";
import allowlist from "../../../tests/helpers/a11y-allowlist.json";

describe("a11y-normalizer", () => {
  it("drops properties not in the stable set", () => {
    const raw = {
      Name: "File",
      ControlType: "MenuItem",
      BoundingRectangle: "0,0,100,20",
      RuntimeId: "12,345",
      children: [],
    };
    const out = normalize(raw, allowlist);
    expect(out).toEqual({
      Name: "File",
      ControlType: "MenuItem",
      children: [],
    });
  });

  it("sorts children deterministically by AutomationId then Name", () => {
    const raw = {
      Name: "Root",
      ControlType: "Pane",
      children: [
        { Name: "Zeta", ControlType: "Button", AutomationId: "btn-z", children: [] },
        { Name: "Alpha", ControlType: "Button", AutomationId: "btn-a", children: [] },
      ],
    };
    const out = normalize(raw, allowlist);
    expect(out.children?.[0]?.AutomationId).toBe("btn-a");
    expect(out.children?.[1]?.AutomationId).toBe("btn-z");
  });

  it("strips wrapper visuals matched by ControlType", () => {
    const customAllowlist = { ...allowlist, alwaysStripWrappers: ["WebView2Wrapper"] };
    const raw = {
      Name: "Host",
      ControlType: "Window",
      children: [
        {
          Name: "wrapper",
          ControlType: "WebView2Wrapper",
          children: [
            { Name: "MenuBar", ControlType: "MenuBar", children: [] },
          ],
        },
      ],
    };
    const out = normalize(raw, customAllowlist);
    expect(out.children).toHaveLength(1);
    expect(out.children?.[0]?.ControlType).toBe("MenuBar");
  });

  it("strips wrapper visuals matched by ClassName (real Chromium chrome pattern)", () => {
    // Mirrors the actual HWND-mode tree from the probe:
    // AloHostMain → Chrome_WidgetWin_1 → BrowserRootView → NonClientView → ...React
    const raw = {
      Name: "AloParticleEditor",
      ControlType: "Window",
      ClassName: "AloHostMain",
      children: [
        {
          Name: "AloParticleEditor",
          ControlType: "Pane",
          ClassName: "Chrome_WidgetWin_1",
          children: [
            {
              Name: "AloParticleEditor - Web content",
              ControlType: "Pane",
              ClassName: "BrowserRootView",
              children: [
                {
                  Name: "",
                  ControlType: "Pane",
                  ClassName: "NonClientView",
                  children: [
                    { Name: "MenuBar", ControlType: "MenuBar", AutomationId: "menubar", children: [] },
                  ],
                },
              ],
            },
          ],
        },
      ],
    };
    const out = normalize(raw, allowlist); // uses the production allowlist
    // After stripping all 3 chrome wrapper levels: root → menubar
    expect(out.ClassName).toBe("AloHostMain");
    expect(out.children).toHaveLength(1);
    expect(out.children?.[0]?.ControlType).toBe("MenuBar");
  });

  it("recursively normalizes descendants", () => {
    const raw = {
      Name: "Root",
      ControlType: "Pane",
      BoundingRectangle: "0,0,100,100",
      children: [
        {
          Name: "Child",
          ControlType: "Button",
          BoundingRectangle: "5,5,50,50",
          children: [],
        },
      ],
    };
    const out = normalize(raw, allowlist);
    expect(out.BoundingRectangle).toBeUndefined();
    expect(out.children?.[0]?.BoundingRectangle).toBeUndefined();
  });
});
