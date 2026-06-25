// useBackingColorSync — push the resolved theme background colour to the
// host so the DComp composition backing matches the app shell.
//
// In the engine visual is clipped to the scene
// rect, so every transparent DOM region OUTSIDE it (panel gaps, splitter
// seams, rounded-corner wedges) composites over the rearmost host backing
// visual. Painting that backing the app-shell `--bg` makes those regions
// blend into the shell instead of showing the black host backing.
//
// The colour is read live from the resolved `--bg` custom property (so we
// don't duplicate the token values) and pushed:
//   - once on mount (first paint), and
//   - whenever `data-theme` on <html> changes (ThemeToggle sets it).
//
// In browser (MockBridge) mode the host ignores / no-ops the request,
// so this is harmless there. The request is
// fire-and-forget; a rejection (unknown kind under MockBridge) is
// swallowed.

import { useEffect } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

/** Read the live resolved `--bg` token from <html>. Empty string if the
 *  environment doesn't resolve custom properties (e.g. jsdom without a
 *  stub) — callers should skip the push in that case. */
export function readBackingColor(): string {
  return getComputedStyle(document.documentElement)
    .getPropertyValue("--bg")
    .trim();
}

export function useBackingColorSync(bridge: Bridge): void {
  useEffect(() => {
    const root = document.documentElement;

    const push = () => {
      const color = readBackingColor();
      if (!color) return; // nothing resolved yet — skip (next change re-pushes)
      void bridge
        .request({ kind: "host/backing-color", params: { color } })
        .catch(() => {
          /* MockBridge ignores the kind — fire-and-forget */
        });
    };

    push(); // first paint

    // ThemeToggle applies the theme via document.documentElement.dataset.theme.
    // Observe that attribute so a toggle (or the initial OS-preference apply)
    // re-pushes the new --bg.
    const observer = new MutationObserver((records) => {
      for (const r of records) {
        if (r.type === "attributes" && r.attributeName === "data-theme") {
          push();
          break;
        }
      }
    });
    observer.observe(root, {
      attributes: true,
      attributeFilter: ["data-theme"],
    });

    return () => observer.disconnect();
  }, [bridge]);
}
