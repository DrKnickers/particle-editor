// UIA accessibility-tree normalizer for HWND-mode goldens ().
//
// Purpose: take a raw UIA snapshot — produced by the Phase 3 a11y probe
// against the running editor host — and reduce it to a deterministic,
// volatility-free shape suitable for snapshot-style assertions.
//
// Three transforms, in order:
//   1. Property pruning. Only keys in `allowlist.stable` survive.
//      `allowlist.volatile` is the documented inverse (RuntimeId,
//      BoundingRectangle, etc.) and is not consulted directly here —
//      the stable allowlist is authoritative.
//   2. Wrapper stripping. The HWND-mode tree from the T0 probe is
//      wrapped in 3+ levels of Chromium/WebView2 chrome
//      (Chrome_WidgetWin_1 → BrowserRootView → NonClientView → …).
//      These wrappers have no semantic meaning for the React app but
//      sit between the host and the menubar. A child whose
//      AutomationId, ControlType, OR ClassName appears in
//      `allowlist.alwaysStripWrappers` is replaced by its children
//      in place. ClassName match is what catches the Chromium chrome —
//      those nodes don't have an AutomationId or distinguishing
//      ControlType (they're all `Pane`).
//   3. Deterministic sort. UIA child order is implementation-defined
//      across runs, so children are sorted by
//      `AutomationId|Name|ControlType`. Stable across processes,
//      machines, and CI vs. local.

export type UIANode = {
  Name?: string;
  ControlType?: string;
  ClassName?: string;
  AutomationId?: string;
  IsKeyboardFocusable?: boolean;
  IsEnabled?: boolean;
  IsOffscreen?: boolean;
  HasKeyboardFocus?: boolean;
  LocalizedControlType?: string;
  ["LegacyAccessible.Role"]?: string;
  ["LegacyAccessible.State"]?: string;
  ["ExpandCollapse.ExpandCollapseState"]?: string;
  ["SelectionItem.IsSelected"]?: boolean;
  ["Toggle.ToggleState"]?: string;
  children?: UIANode[];
  [k: string]: unknown;
};

export type Allowlist = {
  stable: string[];
  volatile: string[];
  alwaysStripWrappers: string[];
};

export function normalize(node: UIANode, allowlist: Allowlist): UIANode {
  const stable = new Set(allowlist.stable);
  const stripped: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(node)) {
    if (k === "children") continue;
    if (stable.has(k)) stripped[k] = v;
  }
  let children = (node.children ?? []).map((c) => normalize(c, allowlist));
  // Strip wrapper visuals: if a child's AutomationId, ControlType, OR
  // ClassName matches alwaysStripWrappers, replace it with its
  // children. ClassName match is what catches the Chromium/WebView2
  // chrome wrappers (Chrome_WidgetWin_1 etc.) — see T0 probe findings.
  const wrappers = new Set(allowlist.alwaysStripWrappers);
  children = children.flatMap((c) => {
    const isWrapper =
      (c.AutomationId && wrappers.has(c.AutomationId)) ||
      (c.ControlType && wrappers.has(c.ControlType)) ||
      (c.ClassName && wrappers.has(c.ClassName));
    return isWrapper ? (c.children ?? []) : [c];
  });
  // Deterministic sort: AutomationId first, then Name, then ControlType.
  // Use ordinal (byte-value) comparison, NOT localeCompare — locale-sensitive
  // comparison produces inconsistent ordering for separator characters like `|`
  // vs alphabetic characters across runs and environments (Windows ICU table
  // version, OS language pack). Surfaced during T9.4 determinism reruns.
  children.sort((a, b) => {
    const ka = `${a.AutomationId ?? ""}|${a.Name ?? ""}|${a.ControlType ?? ""}`;
    const kb = `${b.AutomationId ?? ""}|${b.Name ?? ""}|${b.ControlType ?? ""}`;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
  });
  stripped.children = children;
  return stripped as UIANode;
}
