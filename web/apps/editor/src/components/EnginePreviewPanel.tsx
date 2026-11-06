// Engine still controls beside the viewport. Owns the time and camera inputs;
// lib/engine-preview owns requests/results, and the timeline supplies the initial time.
import { useEffect, useState, type ChangeEvent, type ReactElement } from "react";
import type { Bridge, EngineStateDto, TimelineStateDto } from "@particle-editor/bridge-schema";
import { X } from "lucide-react";
import { Button } from "@/primitives/Button";
import { Checkbox } from "@/primitives/Checkbox";
import { IconButton } from "@/primitives/IconButton";
import { getTimelineState } from "@/lib/timeline";
import { useEngineField } from "@/lib/use-engine-snapshot";
import {
  cancelEnginePreview,
  captureEnginePreview,
  checkEnginePreviewStatus,
  setEnginePreviewOpen,
  useEnginePreview,
  type EnginePreviewStore,
} from "@/lib/engine-preview";

const DEFAULT_TIME_SECONDS = 2.0;
// Mirrors kMaxTimeSeconds in src/host/EnginePreviewPipe.h.
const MAX_TIME_SECONDS = 600;
const TIME_STEP_SECONDS = 0.1;
const TIMING_DECIMAL_PLACES = 2;
const AGE_REFRESH_MS = 1_000;
const MILLISECONDS_PER_SECOND = 1_000;
interface Props {
  bridge: Bridge;
}

export function EnginePreviewPanel({ bridge }: Props): ReactElement | null {
  const isAvailable: boolean =
    useEngineField(bridge, (state: EngineStateDto): boolean => state.enginePreviewAvailable) ?? false;
  const isOpen: boolean = useEnginePreview((state: EnginePreviewStore): boolean => state.isOpen);
  if (!isAvailable || !isOpen) return null;
  return <OpenEnginePreview bridge={bridge} />;
}

function OpenEnginePreview({ bridge }: Props): ReactElement {
  const [time, setTime] = useState<string>((): string => {
    const timeline: TimelineStateDto = getTimelineState();
    return String(timeline.active ? timeline.timeSeconds : DEFAULT_TIME_SECONDS);
  });
  const [matchCamera, setMatchCamera] = useState<boolean>(false);
  const [nowMs, setNowMs] = useState<number>(Date.now);
  const state: EnginePreviewStore = useEnginePreview();
  const timeSeconds: number = Number(time);
  const isValidTime: boolean =
    time.trim() !== "" && Number.isFinite(timeSeconds) && timeSeconds >= 0 && timeSeconds <= MAX_TIME_SECONDS;

  // Recheck the external host whenever the panel opens; the snapshot may be old.
  useEffect((): void => {
    void checkEnginePreviewStatus(bridge);
  }, [bridge]);
  // Refresh the displayed age while a captured image is mounted.
  useEffect((): (() => void) => {
    const timer: ReturnType<typeof setInterval> = setInterval((): void => setNowMs(Date.now()), AGE_REFRESH_MS);
    return (): void => clearInterval(timer);
  }, []);

  let statusText: string = state.status?.state ?? "";
  if (state.isChecking) statusText = "Checking engine host…";
  if (state.isBusy) statusText = "Rendering in engine…";

  return (
    <section
      aria-label="Engine preview"
      aria-busy={state.isBusy}
      className="flex min-h-0 min-w-0 flex-1 flex-col gap-2 overflow-auto border-l border-border bg-panel p-2 text-xs text-text-2"
    >
      <div className="flex items-center justify-between gap-2">
        <h2 className="font-medium text-text">Engine preview</h2>
        <IconButton variant="ghost" label="Close engine preview" onClick={(): void => setEnginePreviewOpen(false)}>
          <X className="size-3.5" />
        </IconButton>
      </div>
      <label className="flex items-center justify-between gap-2">
        Time since spawn (s)
        <input
          className="text-input w-20 disabled:cursor-not-allowed disabled:opacity-40"
          type="number"
          min={0}
          max={MAX_TIME_SECONDS}
          step={TIME_STEP_SECONDS}
          value={time}
          aria-invalid={!isValidTime}
          disabled={state.isBusy}
          onChange={(event: ChangeEvent<HTMLInputElement>): void => setTime(event.target.value)}
        />
      </label>
      <label className="flex items-center gap-2">
        <Checkbox
          checked={matchCamera}
          disabled={state.isBusy}
          onChange={(event: ChangeEvent<HTMLInputElement>): void => setMatchCamera(event.target.checked)}
        />
        Match editor camera
      </label>
      {matchCamera && <p>Camera matching awaits live calibration; the engine view may differ.</p>}
      <div className="flex flex-wrap gap-2">
        <Button
          disabled={state.isBusy || state.isChecking || !isValidTime}
          onClick={(): void => {
            void captureEnginePreview(bridge, timeSeconds, matchCamera);
          }}
        >
          Preview
        </Button>
        <Button
          variant="secondary"
          disabled={!state.isBusy}
          onClick={(): void => {
            void cancelEnginePreview(bridge);
          }}
        >
          Cancel
        </Button>
        <Button
          variant="secondary"
          disabled={state.isBusy || state.isChecking}
          onClick={(): void => {
            void checkEnginePreviewStatus(bridge);
          }}
        >
          Retry status check
        </Button>
      </div>
      <p role="status">{statusText}</p>
      {state.error && (
        <p role="alert" className="text-danger-fg">
          {state.error}
        </p>
      )}
      {state.result ? (
        <figure className="min-w-0">
          <img
            src={state.result.dataUri}
            width={state.result.width}
            height={state.result.height}
            alt="Engine preview still"
            className="h-auto w-full object-contain"
          />
          <figcaption className="mt-1 tabular-nums">
            Round trip {(state.roundTripSeconds ?? state.result.seconds).toFixed(TIMING_DECIMAL_PLACES)} s (engine{" "}
            {state.result.seconds.toFixed(TIMING_DECIMAL_PLACES)} s) ·{" "}
            {Math.max(0, Math.floor((nowMs - (state.capturedAtMs ?? nowMs)) / MILLISECONDS_PER_SECOND))} s ago
          </figcaption>
        </figure>
      ) : (
        <p>Preview saves a separate copy and renders one still in the running engine host.</p>
      )}
    </section>
  );
}
