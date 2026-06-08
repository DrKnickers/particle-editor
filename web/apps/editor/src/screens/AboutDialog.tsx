// AboutDialog — Help → About modal showing app name, version, build date,
// credits, and GitHub link. No bridge call; version + build date are baked
// at build time via Vite `define` (see vite.config.ts).
//
// Legacy chrome: src/main.cpp's AboutProc stays for `--legacy-ui`; this is
// the new-UI counterpart, not a replacement.

import { Modal } from "@/components/Modal";

// Pull from Vite-injected env. These are JSON-stringified by `define` so
// they're available as plain strings at runtime. Fall back to "unknown"
// to keep the dialog rendering even if a future config drift leaves a
// constant unset.
const VERSION = (import.meta.env.VITE_APP_VERSION as string | undefined) ?? "unknown";
const BUILD_DATE = (import.meta.env.VITE_BUILD_DATE as string | undefined) ?? "unknown";

const GITHUB_URL = "https://github.com/DrKnickers/particle-editor";

type Props = {
  open: boolean;
  onOpenChange: (open: boolean) => void;
};

export function AboutDialog({ open, onOpenChange }: Props) {
  return (
    <Modal
      open={open}
      onOpenChange={onOpenChange}
      title="About AloParticleEditor"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="text-lg font-semibold text-text">
            AloParticleEditor
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
          <a
            href={GITHUB_URL}
            target="_blank"
            rel="noopener noreferrer"
            className="text-xs text-accent underline hover:text-accent"
          >
            {GITHUB_URL}
          </a>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
