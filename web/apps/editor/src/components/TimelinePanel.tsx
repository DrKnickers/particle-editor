// TimelinePanel — the interactive timeline's row under the viewport
// (design section 11; the session and its store are in lib/timeline.ts).
//
// Rendered only while a session is open, so the default layout is unchanged.
// Opened and closed from the toolbar's timeline toggle and View → Timeline;
// the X here closes it too. Every control goes through the host, which
// replays the frame: nothing here advances the simulation itself.
//
// Labels follow §11.3: the readout is "time since spawn" and a frame number,
// so it doesn't read as the curve editor's "% lifetime".

import { useState } from "react";
import {
  ChevronLeft,
  ChevronRight,
  Pause,
  Play,
  Repeat,
  RotateCcw,
  SkipBack,
  SkipForward,
  Shuffle,
  X,
} from "lucide-react";
import type { Bridge, TimelineStateDto } from "@particle-editor/bridge-schema";
import { IconButton } from "@/primitives/IconButton";
import {
  closeTimeline,
  newTimelineVariation,
  resetTimeline,
  seekTimeline,
  setTimelineLoop,
  setTimelinePlaying,
  stepTimeline,
  useTimeline,
} from "@/lib/timeline";

type Props = { bridge: Bridge };

const ICON = { className: "size-3.5" } as const;

/** The inline status line for a session that can't show a normal frame. */
export function timelineStatusText(state: Pick<TimelineStateDto, "status" | "refused">): string | null {
  if (state.status === "stale") return "The replay failed. Retrying…";
  if (state.status === "empty") return "No emitters to play.";
  if (state.refused) return "The overload guard refused this spawn.";
  return null;
}

export function TimelinePanel({ bridge }: Props) {
  const active = useTimeline((s) => s.active);
  if (!active) return null;
  return <OpenTimeline bridge={bridge} />;
}

function OpenTimeline({ bridge }: Props) {
  const playing = useTimeline((s) => s.playing);
  const loop = useTimeline((s) => s.loop);
  const frame = useTimeline((s) => s.frame);
  const timeSeconds = useTimeline((s) => s.timeSeconds);
  const maxFrames = useTimeline((s) => s.maxFrames);
  const seed = useTimeline((s) => s.seed);
  const status = useTimeline((s) => s.status);
  const refused = useTimeline((s) => s.refused);
  const notice = timelineStatusText({ status, refused });

  // The thumb follows the pointer at once; the host's frame takes over again
  // when it reaches the value asked for, when play or a failure moves on, or
  // when the seeks drain (a refused seek leaves the frame where it was).
  const [shown, setShown] = useState<number | null>(null);
  const hasSettledSeek: boolean = frame === shown || playing || status === "stale";
  if (shown !== null && hasSettledSeek) setShown(null);

  return (
    <section
      data-testid="timeline-panel"
      aria-label="Timeline"
      className="flex flex-col gap-1 border-t border-border bg-panel px-2 py-1.5 text-2xs text-text-2"
    >
      <div className="flex items-center gap-2">
        <div className="flex items-center gap-0.5" role="group" aria-label="Transport">
          <IconButton
            variant="ghost"
            label="Jump to start"
            onClick={() => {
              void seekTimeline(bridge, 0);
            }}
          >
            <SkipBack {...ICON} />
          </IconButton>
          <IconButton variant="ghost" label="Previous frame" onClick={() => stepTimeline(bridge, -1)}>
            <ChevronLeft {...ICON} />
          </IconButton>
          <IconButton
            variant="ghost"
            label={playing ? "Pause" : "Play"}
            pressed={playing}
            onClick={() => {
              void setTimelinePlaying(bridge, !playing);
            }}
          >
            {playing ? <Pause {...ICON} /> : <Play {...ICON} />}
          </IconButton>
          <IconButton variant="ghost" label="Next frame" onClick={() => stepTimeline(bridge, 1)}>
            <ChevronRight {...ICON} />
          </IconButton>
          <IconButton
            variant="ghost"
            label="Jump to end"
            onClick={() => {
              void seekTimeline(bridge, maxFrames);
            }}
          >
            <SkipForward {...ICON} />
          </IconButton>
          <IconButton
            variant="ghost"
            label="Reset"
            tip="Stop and return to frame 0"
            onClick={() => resetTimeline(bridge)}
          >
            <RotateCcw {...ICON} />
          </IconButton>
          <IconButton
            variant="toggle"
            label="Loop"
            pressed={loop}
            onClick={() => {
              void setTimelineLoop(bridge, !loop);
            }}
          >
            <Repeat {...ICON} />
          </IconButton>
        </div>

        <span data-testid="timeline-readout" className="shrink-0 font-mono tabular-nums text-text">
          <span className="text-text-3">Time since spawn </span>
          {timeSeconds.toFixed(3)} s<span className="text-text-3"> · Frame </span>
          {frame}
          <span className="text-text-3"> / {maxFrames}</span>
        </span>

        <input
          type="range"
          aria-label="Playhead"
          min={0}
          max={maxFrames}
          step={1}
          value={shown ?? frame}
          onChange={(e) => {
            const next = parseInt(e.target.value, 10);
            setShown(next);
            void seekTimeline(bridge, next).then(() => setShown(null));
          }}
          className="min-w-24 flex-1 accent-[var(--accent-strong)]"
        />

        <div className="flex shrink-0 items-center gap-1.5">
          <span className="text-text-3">Variation: Locked</span>
          <button
            type="button"
            onClick={() => {
              void newTimelineVariation(bridge);
            }}
            className="flex items-center gap-1 rounded border border-border-2 bg-bg-2 px-1.5 py-0.5 text-text-2 transition-colors motion-reduce:transition-none hover:bg-panel-2 hover:text-text focus-ring"
          >
            <Shuffle className="size-3" />
            New variation
          </button>
          <details className="text-text-3">
            <summary className="cursor-pointer select-none">Seed</summary>
            <span data-testid="timeline-seed" className="font-mono text-text-2">
              {seed}
            </span>
          </details>
        </div>

        <IconButton
          variant="ghost"
          label="Close timeline"
          onClick={() => {
            void closeTimeline(bridge);
          }}
        >
          <X {...ICON} />
        </IconButton>
      </div>

      <p className="text-text-3">
        Plays the effect from a single spawn at the origin. Opening the timeline clears placed instances.
        {notice && (
          <span data-testid="timeline-notice" role="status" className="ml-2 text-warning">
            {notice}
          </span>
        )}
      </p>
    </section>
  );
}
