// AboutDialog — Help → About modal showing app name, version, build date,
// credits, and GitHub link. Version + build date are baked
// at build time via Vite `define` (see vite.config.ts).
//
// Ported from the legacy Win32 editor's About dialog. That dialog and
// the `--legacy-ui` opt-out were removed; this React modal is now
// the sole About surface.

import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { Button } from "@/primitives/Button";
import { EXTERNAL_LINKS } from "@/lib/external-links";
import { fireAndReport, useStatusFeedback } from "@/lib/status-feedback";

// Pull from Vite-injected env. These are JSON-stringified by `define` so
// they're available as plain strings at runtime. Fall back to "unknown"
// to keep the dialog rendering even if a future config drift leaves a
// constant unset.
const VERSION = (import.meta.env.VITE_APP_VERSION as string | undefined) ?? "unknown";
const BUILD_DATE = (import.meta.env.VITE_BUILD_DATE as string | undefined) ?? "unknown";

// The PUBLIC repo — the user-facing project home. Must never point at the
// private repo: this string ships inside the exe's embedded web bundle, so a
// private URL here 404s for every user (it leaked exactly that way in the
// first v0.3.0 zip until a check of the shipped binary caught it).
const GITHUB_URL = EXTERNAL_LINKS.repository;

type Props = {
  bridge: Bridge;
  open: boolean;
  onOpenChange: (open: boolean) => void;
};

export function AboutDialog({ bridge, open, onOpenChange }: Props) {
  const handleOpenRepository = async () => {
    const result = await fireAndReport(bridge, {
      kind: "app/open-external",
      params: { target: "repository" },
    }, "Open project page");
    if (!result?.opened) {
      useStatusFeedback.getState().announce("Could not open project page. Copy the project address into your browser.");
    }
  };

  return (
    <Modal
      bridge={bridge}
      open={open}
      onOpenChange={onOpenChange}
      title="About Particle Editor"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="text-lg font-semibold text-text">
            Particle Editor
          </div>
          <div className="text-text-2">
            Version {VERSION}
          </div>
          <div className="text-xs text-text-3">
            Build date: {BUILD_DATE}
          </div>
          <div className="text-xs text-text-3">
            Forked from Mike.NL's GlyphX Particle Editor v1.5
          </div>
          <p className="mt-2 text-xs leading-relaxed text-text-2">
            Particle editor for the Petroglyph Alamo engine
            (Star Wars: Empire at War / Forces of Corruption).
            Distributed under the MIT licence. This software is provided
            "as is", without warranty of any kind.
          </p>
          <Button variant="secondary" onClick={() => void handleOpenRepository()}>
            Open project page
          </Button>
          <label className="flex flex-col gap-1 text-xs text-text-2">
            Project address
            <input
              className="text-input w-full select-text"
              readOnly
              value={GITHUB_URL}
              onFocus={(e) => e.currentTarget.select()}
            />
          </label>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
