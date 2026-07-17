// IncrementIndexDialog.
//
// Single-spinner modal that triggers
// `emitters/duplicate-with-index-increment`. Mirrors legacy
// `ShowIncrementDialog` at [src/UI/EmitterList.cpp:2354] — the legacy
// dialog uses a spin control with range 1..999 and a default of 1.
//
// Driven by the `tree-context` atom; mounts at App level.

import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { Spinner } from "@/primitives/Spinner";
import { useTreeContextStore } from "@/lib/tree-context";

type Props = {
  bridge: Bridge;
};

export function IncrementIndexDialog({ bridge }: Props) {
  const open = useTreeContextStore((s) => s.open === "increment");
  const targetId = useTreeContextStore((s) => s.targetEmitterId);
  const close = useTreeContextStore((s) => s.close);

  const [delta, setDelta] = useState(1);
  const [count, setCount] = useState(1);

  // Reset to defaults each time the dialog opens so a cancel-and-reopen
  // round-trip starts fresh.
  useEffect(() => {
    if (open) {
      setDelta(1);
      setCount(1);
    }
  }, [open]);

  const handleOk = () => {
    if (targetId === null) return;
    // Always use the batch request — with count 1 it makes a single duplicate,
    // matching the old single-request behavior, and with count > 1 it chains N
    // duplicates in one undo step (each made from the previous copy so the index
    // track climbs by `delta` each step).
    // Round the count to a whole number: the Spinner commits the raw parsed
    // value (decimals=0 only affects display), so a typed "2.7" shows "3" but
    // would otherwise send 2.7 and make 2 copies — round so the count sent
    // matches the integer the user sees. (delta is a genuine float shift.)
    void bridge.request({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: targetId, delta, count: Math.round(count) },
    });
    close();
  };

  return (
    <Modal
      open={open}
      onOpenChange={(o) => {
        if (!o) close();
      }}
      title="Duplicate with Index Increment"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="grid grid-cols-[auto_1fr] items-center gap-x-3 gap-y-2">
            <label className="text-xs text-text-2" htmlFor="increment-delta">
              Increment by
            </label>
            <Spinner
              value={delta}
              onChange={setDelta}
              min={1}
              max={999}
              step={1}
              decimals={0}
              aria-label="Increment by"
            />
            <label className="text-xs text-text-2" htmlFor="increment-count">
              Repeat
            </label>
            <Spinner
              value={count}
              onChange={setCount}
              min={1}
              max={999}
              step={1}
              decimals={0}
              aria-label="Repeat count"
              testId="increment-repeat"
            />
          </div>
          <p className="text-[11px] leading-relaxed text-text-3">
            Duplicates the emitter and shifts every atlas-index keyframe
            on the duplicate by this delta. If the source has no index
            keys, a single key at t=0 is inserted with the chosen value.
            A repeat count above 1 chains that many duplicates in one step —
            each made from the previous copy, so the index climbs by the
            delta each time.
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
