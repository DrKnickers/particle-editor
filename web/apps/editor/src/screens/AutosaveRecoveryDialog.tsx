// AutosaveRecoveryDialog — crash-recovery prompt for the new UI.
//
// On app mount the container calls `autosave/check-recovery`. If a crashed
// prior session left an orphaned autosave (under %TEMP%\AloParticleEditor\),
// the host returns it and this modal offers to restore it. The host returns
// `{ orphan: null }` under --test-host / when a document is already loaded /
// under the MockBridge, so the dialog never appears in those cases.
//
// Three outcomes (the variant shown depends on which tiers the crash left):
//   - Restore recent — load the 30 s-tier autosave (freshest).
//   - Restore stable — load the 5 min-tier autosave (older known-good).
//   - Discard        — drop the autosave, keep the fresh boot document.
// Restored content opens AS the original filename with unsaved changes
// (dirty), so Ctrl+S targets the original and the temp path never surfaces.
//
// Dismissing without choosing (Esc / overlay / X) is "decide later": no
// `recover` is sent, the orphan stays on disk, and it re-prompts next launch.
// Safer than treating a stray Esc as Discard (no accidental data loss).
//
// The component is split so the presentation is trivially testable and can be
// driven deterministically from the `?demo=autosave-recovery` a11y route:
//   - AutosaveRecoveryView — pure; takes an orphan + an explicit `nowMs` so
//     the relative-age text is deterministic when a fixed now is supplied.
//   - AutosaveRecoveryDialog — wires check-recovery / recover to the view.

import { useEffect, useState } from "react";
import type { AutosaveOrphan, Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";

/** Coarse "N units ago" relative age, mirroring legacy FormatAge
 *  (src/main.cpp:1118). `nowMs` is injectable so tests / the a11y demo route
 *  pin it for a deterministic string. */
export function formatAutosaveAge(mtimeMs: number, nowMs: number): string {
  const diffSec = Math.max(0, Math.floor((nowMs - mtimeMs) / 1000));
  if (diffSec < 5) return "just now";
  if (diffSec < 60) return `${diffSec} seconds ago`;
  const min = Math.floor(diffSec / 60);
  if (min < 60) return `${min} ${min === 1 ? "minute" : "minutes"} ago`;
  const hr = Math.floor(diffSec / 3600);
  return `${hr} ${hr === 1 ? "hour" : "hours"} ago`;
}

export type RecoverChoice = "recent" | "stable" | "discard";

type ViewProps = {
  /** The orphan to offer, or null to keep the dialog closed. */
  orphan: AutosaveOrphan | null;
  /** Current time for age rendering. Injectable for deterministic tests. */
  nowMs?: number;
  /** A choice button was clicked. */
  onChoose: (choice: RecoverChoice) => void;
  /** Dismissed without choosing (Esc / overlay / X) — "decide later". */
  onDismiss: () => void;
};

/** Pure presentation. Renders the 3-state recovery prompt for the tiers the
 *  orphan carries (both → 3 buttons; single tier → restore + discard). */
export function AutosaveRecoveryView({ orphan, nowMs, onChoose, onDismiss }: ViewProps) {
  const now = nowMs ?? Date.now();
  const hasRecent = orphan != null && orphan.recentMtimeMs != null;
  const hasStable = orphan != null && orphan.stableMtimeMs != null;
  const original =
    orphan == null || orphan.originalFilename === ""
      ? "Unsaved new file"
      : orphan.originalFilename;

  return (
    <Modal
      open={orphan != null}
      onOpenChange={(o) => { if (!o) onDismiss(); }}
      title="Recover unsaved changes?"
      size="md"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm text-text">
          <p className="leading-relaxed">
            Unsaved changes from a previous session were found.
          </p>
          <p className="text-text-2">
            Original:{" "}
            {/* Long mod paths have no spaces in their segments; break-all lets
                them wrap to fit the dialog instead of overflowing into a
                horizontal scrollbar. */}
            <span className="font-medium text-text break-all">{original}</span>
          </p>
          {orphan != null && (
            <ul className="flex flex-col gap-1 text-[11px] leading-relaxed text-text-3">
              {hasRecent && (
                <li>
                  Most recent autosave —{" "}
                  <span data-testid="autosave-recent-age">
                    {formatAutosaveAge(orphan.recentMtimeMs!, now)}
                  </span>
                </li>
              )}
              {hasStable && (
                <li>
                  Stable backup —{" "}
                  <span data-testid="autosave-stable-age">
                    {formatAutosaveAge(orphan.stableMtimeMs!, now)}
                  </span>
                </li>
              )}
            </ul>
          )}
        </div>
      </Modal.Body>
      <Modal.Footer>
        <button
          type="button"
          onClick={() => onChoose("discard")}
          data-testid="autosave-discard"
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 focus-ring"
        >
          Discard
        </button>
        {hasStable && (
          <button
            type="button"
            onClick={() => onChoose("stable")}
            data-testid="autosave-restore-stable"
            className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 focus-ring"
          >
            Restore stable
          </button>
        )}
        {hasRecent && (
          <button
            type="button"
            onClick={() => onChoose("recent")}
            data-testid="autosave-restore-recent"
            className="rounded bg-accent px-3 py-1 text-xs font-medium text-white hover:bg-accent focus-ring"
          >
            Restore recent
          </button>
        )}
        {/* Stable-only: the single restore action is the primary button. */}
        {!hasRecent && hasStable && null}
      </Modal.Footer>
    </Modal>
  );
}

type Props = { bridge: Bridge };

/** Container: checks for an orphan on mount and wires the choice back to the
 *  host. Mounted unconditionally in AppShell — a no-op when the host reports
 *  no orphan (the common case, and always so under the mock / --test-host). */
export function AutosaveRecoveryDialog({ bridge }: Props) {
  const [orphan, setOrphan] = useState<AutosaveOrphan | null>(null);

  useEffect(() => {
    let cancelled = false;
    void bridge
      .request({ kind: "autosave/check-recovery", params: {} })
      .then((r) => { if (!cancelled && r.orphan) setOrphan(r.orphan); })
      .catch(() => { /* best-effort — no recovery prompt on failure */ });
    return () => { cancelled = true; };
  }, [bridge]);

  const choose = (choice: RecoverChoice) => {
    void bridge.request({ kind: "autosave/recover", params: { choice } });
    setOrphan(null);
  };

  // Decide-later: close locally, send no recover so the host keeps the orphan
  // for next launch.
  const dismiss = () => setOrphan(null);

  return <AutosaveRecoveryView orphan={orphan} onChoose={choose} onDismiss={dismiss} />;
}
