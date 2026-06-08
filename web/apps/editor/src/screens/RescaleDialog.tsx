// RescaleDialog — Edit → Rescale… modal. Rescales the entire active
// particle system by a duration-scale and size-scale percentage. Two
// Spinner rows + Cancel/OK footer.
//
// Bridge call: engine/action/rescale-system { durationScalePercent,
// sizeScalePercent }. Returns Record<string, never>.
//
// Legacy chrome: src/main.cpp's WM_COMMAND launcher for IDD_RESCALE_SYSTEM
// (line 1524) stays for `--legacy-ui`; this is the new-UI counterpart.

import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { Spinner } from "@/primitives/Spinner";

type Props = {
  bridge: Bridge;
  open: boolean;
  onOpenChange: (open: boolean) => void;
};

export function RescaleDialog({ bridge, open, onOpenChange }: Props) {
  // Local draft state — reset to 100/100 each time the dialog opens so
  // a cancelled-and-reopened flow starts fresh.
  const [durationScale, setDurationScale] = useState(100);
  const [sizeScale, setSizeScale] = useState(100);

  useEffect(() => {
    if (open) {
      setDurationScale(100);
      setSizeScale(100);
    }
  }, [open]);

  const handleOk = () => {
    void bridge.request({
      kind: "engine/action/rescale-system",
      params: {
        durationScalePercent: durationScale,
        sizeScalePercent: sizeScale,
      },
    });
    onOpenChange(false);
  };

  return (
    <Modal
      open={open}
      onOpenChange={onOpenChange}
      title="Rescale Particle System"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="grid grid-cols-[auto_1fr] items-center gap-x-3 gap-y-2">
            <label
              className="text-xs text-text-2"
              htmlFor="rescale-duration"
            >
              Duration scale
            </label>
            <Spinner
              value={durationScale}
              onChange={setDurationScale}
              min={1}
              max={1000}
              step={1}
              decimals={0}
              unit="%"
              aria-label="Duration scale"
            />
            <label
              className="text-xs text-text-2"
              htmlFor="rescale-size"
            >
              Size scale
            </label>
            <Spinner
              value={sizeScale}
              onChange={setSizeScale}
              min={1}
              max={1000}
              step={1}
              decimals={0}
              unit="%"
              aria-label="Size scale"
            />
          </div>
          <p className="text-[11px] leading-relaxed text-text-3">
            Applies to the entire particle system. Use{" "}
            <em>Rescale Emitter…</em> to rescale a single emitter.
          </p>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton>Cancel</Modal.CancelButton>
        <Modal.OkButton onClick={handleOk}>OK</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
