// RescaleEmitterDialog — Screen 4 Batch B1.
//
// Two-Spinner modal that fires `engine/action/rescale-emitter`. Shape
// mirrors `RescaleDialog` (the system-wide rescale) but the bridge
// call carries an explicit emitter id and only touches the chosen
// emitter via `DoRescaleEmitter`.
//
// Driven by the `tree-context` atom.

import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { Spinner } from "@/primitives/Spinner";
import { useTreeContextStore } from "@/lib/tree-context";

type Props = {
  bridge: Bridge;
};

export function RescaleEmitterDialog({ bridge }: Props) {
  const open = useTreeContextStore((s) => s.open === "rescale");
  const targetId = useTreeContextStore((s) => s.targetEmitterId);
  const close = useTreeContextStore((s) => s.close);

  const [durationScale, setDurationScale] = useState(100);
  const [sizeScale, setSizeScale] = useState(100);

  useEffect(() => {
    if (open) {
      setDurationScale(100);
      setSizeScale(100);
    }
  }, [open]);

  const handleOk = () => {
    if (targetId === null) return;
    void bridge.request({
      kind: "engine/action/rescale-emitter",
      params: {
        id: targetId,
        durationScalePercent: durationScale,
        sizeScalePercent: sizeScale,
      },
    });
    close();
  };

  return (
    <Modal
      open={open}
      onOpenChange={(o) => {
        if (!o) close();
      }}
      title="Rescale Emitter"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="grid grid-cols-[auto_1fr] items-center gap-x-3 gap-y-2">
            <label
              className="text-xs text-text-2"
              htmlFor="rescale-emitter-duration"
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
              htmlFor="rescale-emitter-size"
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
            Applies to the selected emitter only. Use{" "}
            <em>Rescale Particle System…</em> to rescale the entire system.
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
