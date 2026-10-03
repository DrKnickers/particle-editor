import { memo, useMemo, useRef, type MutableRefObject, type PointerEvent as ReactPointerEvent } from "react";
import type { TrackDto } from "@particle-editor/bridge-schema";
import type { ChannelDef } from "./CurveEditor";
import { buildSmoothPath, buildStepPolyline, buildFillPath } from "./curve-paths";

/** Stroke dash pattern for the locked-mirror curve. Visually distinguishes
 *  a read-only focus channel from an editable one. Feel-tunable. */
const READONLY_DASH = "7 5";

export type CurvePoint = { x: number; y: number; time: number; value: number };

export type CurveLayerModel = {
  channel: ChannelDef;
  track: TrackDto;
  points: CurvePoint[];
  range: { min: number; max: number };
};

export type CurveLayerCacheEntry = {
  channel: ChannelDef;
  track: TrackDto;
  width: number;
  height: number;
  timeMin: number;
  timeMax: number;
  displayMin: number;
  displayMax: number;
  layer: CurveLayerModel;
};

export type CurveDragState = {
  keyTime: number;
  startTime: number;
  startValue: number;
  startClientX: number;
  startClientY: number;
  currentTime: number;
  currentValue: number;
  moved: boolean;
  pointerId: number;
  target: Element | null;
  isGroup: boolean;
  groupDTime: number;
  groupDValue: number;
};

function useLayerRenderCount(): number {
  const countRef = useRef(0);
  countRef.current += 1;
  return countRef.current;
}

type StaticChannelLayerProps = {
  layer: CurveLayerModel;
  focusEnabled: boolean;
  hidden: boolean;
};

export const StaticChannelLayer = memo(function StaticChannelLayer({
  layer,
  focusEnabled,
  hidden,
}: StaticChannelLayerProps) {
  const { channel, track, points } = layer;
  const renderCount = useLayerRenderCount();
  const interp = track.interpolation;
  const smoothPath = useMemo(
    () => (points.length >= 2 && interp === "smooth" ? buildSmoothPath(points) : ""),
    [interp, points],
  );
  const stepPoints = useMemo(
    () => (points.length >= 2 && interp === "step" ? buildStepPolyline(points) : ""),
    [interp, points],
  );
  const linearPoints = useMemo(
    () => (points.length >= 2 && interp === "linear" ? points.map((p) => String(p.x) + "," + String(p.y)).join(" ") : ""),
    [interp, points],
  );
  const layerOpacity = focusEnabled ? 0.4 : 1;
  const strokeW = 2;
  const markerR = focusEnabled ? 3 : 4;
  const markerStroke = focusEnabled ? "none" : "var(--curve-marker-stroke)";
  const markerStrokeW = focusEnabled ? 0 : 1;
  const markerTestId = focusEnabled ? undefined : "curve-key";
  return (
    <g
      data-testid={"curve-layer-" + channel.id}
      data-channel-id={channel.id}
      data-key-count={points.length}
      data-focus="false"
      data-render-count={renderCount}
      style={{ opacity: layerOpacity, visibility: hidden ? "hidden" : undefined }}
    >
      {smoothPath !== "" && (
        <path fill="none" stroke={channel.color} strokeWidth={strokeW} d={smoothPath} pointerEvents="none" />
      )}
      {stepPoints !== "" && (
        <polyline fill="none" stroke={channel.color} strokeWidth={strokeW} points={stepPoints} pointerEvents="none" />
      )}
      {linearPoints !== "" && (
        <polyline fill="none" stroke={channel.color} strokeWidth={strokeW} points={linearPoints} pointerEvents="none" />
      )}
      {points.map((p, i) => (
        <circle
          key={i}
          {...(markerTestId !== undefined ? { "data-testid": markerTestId } : {})}
          data-channel-id={channel.id}
          data-key-time={p.time}
          cx={p.x}
          cy={p.y}
          r={markerR}
          fill={channel.color}
          stroke={markerStroke}
          strokeWidth={markerStrokeW}
          pointerEvents="none"
        />
      ))}
    </g>
  );
});

type FocusChannelLayerProps = {
  layer: CurveLayerModel;
  renderPoints: CurvePoint[];
  focusReadOnly: boolean;
  selectedKeyTimes?: ReadonlySet<number>;
  focusBorderTimes: ReadonlySet<number>;
  hidden: boolean;
  height: number;
  onKeyClick?: (time: number, event: React.MouseEvent | React.PointerEvent) => void;
  onKeyContextMenu?: (
    time: number,
    isBorder: boolean,
    clientX: number,
    clientY: number,
  ) => void;
  startDrag: (
    event: ReactPointerEvent<SVGCircleElement>,
    keyTime: number,
    keyValue: number,
  ) => void;
  dragRef: MutableRefObject<CurveDragState | null>;
  dragConsumedClickRef: MutableRefObject<boolean>;
};

export const FocusChannelLayer = memo(function FocusChannelLayer({
  layer,
  renderPoints,
  focusReadOnly,
  selectedKeyTimes,
  focusBorderTimes,
  hidden,
  height,
  onKeyClick,
  onKeyContextMenu,
  startDrag,
  dragRef,
  dragConsumedClickRef,
}: FocusChannelLayerProps) {
  const { channel, track } = layer;
  const renderCount = useLayerRenderCount();
  const interp = track.interpolation;
  const fillGradId = "curve-fill-" + channel.id;
  const fillPath = useMemo(
    () => (renderPoints.length >= 2 ? buildFillPath(renderPoints, interp, height) : ""),
    [height, interp, renderPoints],
  );
  const smoothPath = useMemo(
    () => (renderPoints.length >= 2 && interp === "smooth" ? buildSmoothPath(renderPoints) : ""),
    [interp, renderPoints],
  );
  const stepPoints = useMemo(
    () => (renderPoints.length >= 2 && interp === "step" ? buildStepPolyline(renderPoints) : ""),
    [interp, renderPoints],
  );
  const linearPoints = useMemo(
    () => (renderPoints.length >= 2 && interp === "linear" ? renderPoints.map((p) => String(p.x) + "," + String(p.y)).join(" ") : ""),
    [interp, renderPoints],
  );
  return (
    <g
      data-testid={"curve-layer-" + channel.id}
      data-channel-id={channel.id}
      data-key-count={renderPoints.length}
      data-focus="true"
      data-readonly={focusReadOnly ? "true" : "false"}
      data-render-count={renderCount}
      style={{ visibility: hidden ? "hidden" : undefined }}
    >
      <defs>
        <linearGradient id={fillGradId} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={channel.color} stopOpacity="0.25" />
          <stop offset="100%" stopColor={channel.color} stopOpacity="0" />
        </linearGradient>
      </defs>
      {fillPath !== "" && (
        <path data-testid="curve-fill" fill={"url(#" + fillGradId + ")"} stroke="none" d={fillPath} pointerEvents="none" />
      )}
      {smoothPath !== "" && (
        <path data-testid="curve-path" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} d={smoothPath} pointerEvents="none" />
      )}
      {stepPoints !== "" && (
        <polyline data-testid="curve-polyline" data-interpolation="step" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} points={stepPoints} pointerEvents="none" />
      )}
      {linearPoints !== "" && (
        <polyline data-testid="curve-polyline" data-interpolation="linear" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} points={linearPoints} pointerEvents="none" />
      )}
      {renderPoints.map((p, i) => {
        const selected = selectedKeyTimes?.has(p.time) ?? false;
        const isBorder = focusBorderTimes.has(p.time);
        const hitR = selected ? 18 : 14;
        const visR = selected ? 6.5 : 5;
        const markerFill = focusReadOnly ? "none" : selected ? "var(--curve-marker-core)" : channel.color;
        const markerStroke = focusReadOnly ? channel.color : selected ? channel.color : "none";
        const markerStrokeWidth = focusReadOnly ? 2 : selected ? 2.5 : 0;
        return (
          <g key={i}>
            <circle
              data-testid="curve-key"
              data-channel-id={channel.id}
              data-key-time={p.time}
              data-selected={selected ? "true" : "false"}
              data-border={isBorder ? "true" : "false"}
              cx={p.x}
              cy={p.y}
              r={hitR}
              fill="transparent"
              stroke="transparent"
              style={{ cursor: onKeyClick ? "pointer" : undefined }}
              onPointerDown={(e) => startDrag(e, p.time, p.value)}
              onContextMenu={(e) => {
                if (focusReadOnly) return;
                if (!onKeyContextMenu) return;
                e.preventDefault();
                e.stopPropagation();
                onKeyContextMenu(p.time, isBorder, e.clientX, e.clientY);
              }}
              onClick={(e) => {
                if (focusReadOnly) return;
                e.stopPropagation();
                if (dragConsumedClickRef.current) {
                  dragConsumedClickRef.current = false;
                  return;
                }
                if (dragRef.current === null) {
                  onKeyClick?.(p.time, e);
                }
              }}
            />
            <circle
              className="curve-key-marker"
              data-selected={selected ? "true" : "false"}
              cx={p.x}
              cy={p.y}
              r={visR}
              fill={markerFill}
              stroke={markerStroke}
              strokeWidth={markerStrokeWidth}
              pointerEvents="none"
            />
          </g>
        );
      })}
    </g>
  );
});
