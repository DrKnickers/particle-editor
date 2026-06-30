import { useDockAnim } from "@/lib/dock-anim";
import { computeSceneRect } from "@/lib/scene-rect";
import type { CursorTarget, RecordCursorKey } from "@/lib/record-cursor-track";

export interface ResolvedCursorCenter {
  x: number;
  y: number;
  ok: boolean;
}

export interface ResolvedCursorElement extends ResolvedCursorCenter {
  ref: string;
}

export interface EvaluatedRecordCursor extends ResolvedCursorCenter {
  vis: boolean;
  press: boolean;
  resolved: ResolvedCursorElement[];
}

const UNRESOLVED: ResolvedCursorCenter = { x: Number.NaN, y: Number.NaN, ok: false };

function quoteAttr(value: string): string {
  return JSON.stringify(value);
}
function selectorForRef(ref: string): { selector: string; atlasTile: boolean } | null {
  const parts = ref.split(":");
  if (parts[0] === "curve-key" && parts.length === 3) {
    return {
      selector: `[data-testid="curve-key"][data-channel-id=${quoteAttr(parts[1])}][data-key-time=${quoteAttr(parts[2])}]`,
      atlasTile: false,
    };
  }
  if (parts[0] === "atlas-tile" && parts.length === 2) {
    return { selector: `[data-testid="atlas-cell"][data-frame=${quoteAttr(parts[1])}]`, atlasTile: true };
  }
  if (parts[0] === "channel-row" && parts.length === 2) {
    return { selector: `[data-testid=${quoteAttr(`curve-channel-row-${parts[1]}`)}]`, atlasTile: false };
  }
  return null;
}

function atlasSettled(): boolean {
  const { atlasGridMounted, animating } = useDockAnim.getState();
  return atlasGridMounted && !animating;
}

export function resolveTargetCenter(target: CursorTarget): ResolvedCursorCenter {
  if (target.kind === "point") return { x: target.x, y: target.y, ok: true };

  const mapped = selectorForRef(target.ref);
  if (!mapped) return { ...UNRESOLVED };
  if (mapped.atlasTile && !atlasSettled()) return { ...UNRESOLVED };

  const el = document.querySelector<HTMLElement>(mapped.selector);
  if (!el) return { ...UNRESOLVED };
  const rect = computeSceneRect(el);
  return { x: rect.x + rect.w / 2, y: rect.y + rect.h / 2, ok: true };
}

function resolvedElementFor(key: RecordCursorKey, resolved: ResolvedCursorCenter): ResolvedCursorElement[] {
  return key.target.kind === "element" ? [{ ref: key.target.ref, ...resolved }] : [];
}

// A single key (clamped ends, or a degenerate span). If its target can't be
// resolved right now, FORCE the cursor hidden (vis:false) for this frame rather
// than render it at a bogus position — a press-frame miss is caught loudly host-
// side via the `resolved` set; a non-press transit miss is benign (just hidden).
function holdKey(key: RecordCursorKey): EvaluatedRecordCursor {
  const resolved = resolveTargetCenter(key.target);
  return {
    x: resolved.ok ? resolved.x : 0,
    y: resolved.ok ? resolved.y : 0,
    ok: resolved.ok,
    vis: resolved.ok ? key.vis : false,
    press: key.press,
    resolved: resolvedElementFor(key, resolved),
  };
}

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

function smoothstep(u: number): number {
  return u * u * (3 - 2 * u);
}

export function evalRecordCursor(keys: readonly RecordCursorKey[], t: number): EvaluatedRecordCursor {
  if (keys.length === 0) {
    return { x: Number.NaN, y: Number.NaN, ok: false, vis: false, press: false, resolved: [] };
  }

  const ordered = [...keys].sort((a, b) => a.t - b.t);
  const first = ordered[0];
  const last = ordered[ordered.length - 1];
  if (t <= first.t) return holdKey(first);
  if (t >= last.t) return holdKey(last);

  for (let i = 1; i < ordered.length; i += 1) {
    const a = ordered[i - 1];
    const b = ordered[i];
    if (t > b.t) continue;

    const ar = resolveTargetCenter(a.target);
    const br = resolveTargetCenter(b.target);
    // Report `b` — the key whose vis/press this frame steps to (below), i.e. the
    // one the cursor is heading to / pressing. The host's fail gate is `press &&
    // !ok`, so the pressed element MUST be the reported one (at t==b.t the cursor
    // has arrived at b and may be clicking it; reporting `a` would let an
    // unresolved arrival slip the gate). During a non-press transit toward a not-
    // yet-mounted `b` (atlas dock mid-open), press is false, so its ok:false is
    // benign and the cursor just hides until `b` is ready.
    const resolved = resolvedElementFor(b, br);
    const span = b.t - a.t;
    const u = span > 0 ? clamp01((t - a.t) / span) : 1;
    const ue = smoothstep(u);
    if (ar.ok && br.ok) {
      return {
        x: ar.x + (br.x - ar.x) * ue,
        y: ar.y + (br.y - ar.y) * ue,
        ok: true,
        vis: b.vis,
        press: b.press,
        resolved,
      };
    }
    if (ar.ok) {
      // Upcoming target not ready yet — hold at the resolved (due) end.
      return { x: ar.x, y: ar.y, ok: true, vis: b.vis, press: b.press, resolved };
    }
    if (br.ok) {
      // The due element vanished (e.g. its channel defocused as we leave it) but
      // the destination is ready — head there. `resolved` carries a's ok:false so
      // the host fails ONLY if this is a press frame.
      return { x: br.x, y: br.y, ok: false, vis: b.vis, press: b.press, resolved };
    }
    // Neither end resolvable mid-transit — hide the cursor this frame.
    return { x: 0, y: 0, ok: false, vis: false, press: b.press, resolved };
  }

  return holdKey(last);
}
