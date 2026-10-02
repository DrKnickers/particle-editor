// HostWindow_Viewport.cpp — HostWindowImpl::ViewportWndProc, the window
// procedure for the D3D9 viewport: camera drags, manipulator handles, the
// Shift-spawn preview and keyboard input. The drag state it reads and writes
// lives on HostWindowImpl (HostWindowImpl.h).
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "HostWindowImpl.h"

namespace host {

LRESULT HostWindowImpl::ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        // rendering happens on the main-loop idle path
        // (PeekMessage-drain → render). WM_PAINT just validates the
        // invalid region so Windows doesn't keep firing it. Same pattern
        // as the legacy editor's main loop, where WM_PAINT also did
        // nothing visible and the idle render owns the pipeline.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        // Suppress GDI erase — D3D9 owns the surface.
        return 1;

    // ---------------------------------------------------------------
    // viewport interaction — camera controls.
    //
    // Mirrors the legacy editor's handler. The math
    // for MOVE / ROTATE / ZOOM is lifted verbatim from legacy so the
    // user's muscle-memory carries over: drag delta scales /2.0f for
    // rotate (full-window-width drag ≈ 180°), distance/1000 for
    // MOVE multiplier, sqrt(olddist)-based scaling for ZOOM.
    //
    // Scope: camera controls and the shift-click-to-spawn path. The status-bar mouse-coord push
    // (legacy 3041) is a polish item.
    //
    // Engine state emission: SetCamera bypasses the dispatcher
    // setter ladder, so we must call EmitEngineStateChanged()
    // ourselves after each mutation to keep React subscribers in
    // sync. View state is not file content — no markDirty here
    // (matches legacy, which never calls SetFileChanged for camera).
    // ---------------------------------------------------------------
    case WM_LBUTTONDOWN:
    {
        if (!engine) return 0;
        // Smoke instrumentation — verify what wParam
        // actually arrived from the synthesized PostMessage.
        Log("[ArchC-engine] WM_LBUTTONDOWN wp=0x%llx MK_SHIFT=%d MK_CONTROL=%d hasPS=%d emitters=%zu attached=%d\n",
            static_cast<unsigned long long>(wp),
            (wp & MK_SHIFT) ? 1 : 0,
            (wp & MK_CONTROL) ? 1 : 0,
            particleSystem ? 1 : 0,
            particleSystem ? particleSystem->getEmitters().size() : 0,
            LiveAttachedSystem() ? 1 : 0);
        // The viewport popup is hidden and WebView2 owns
        // keyboard routing; we forward keystrokes through the bridge. We do
        // NOT SetFocus the hidden popup — that briefly succeeds (visibility
        // isn't a precondition; WS_EX_NOACTIVATE only blocks user-driven
        // activation), then OS focus management snaps it back, firing a
        // spurious WM_KILLFOCUS the defensive kill below would read as "user
        // Alt-Tab'd, drop the spawn."
        // Shift+LMB also triggers cursor-bound spawn. The
        // legacy keydown-only path (case WM_KEYDOWN below) requires the
        // viewport HWND to have focus when Shift is pressed, but
        // WebView2 holds focus from the React UI by default — the
        // user's typical "shift-then-click" gesture swallows the
        // initial WM_KEYDOWN in WebView2 and the spawn never fires.
        // By trapping the click while MK_SHIFT is set, we provide a
        // click-based entry point that doesn't depend on WM_KEYDOWN
        // routing. Skip the camera drag so the spawn doesn't compete
        // with a MOVE drag. Release on Shift-keyup or LBUTTONUP — the
        // existing WM_KEYUP handler kills the attached instance.
        // Legacy parity: if a cursor-bound preview already exists (spawned
        // by an earlier WM_KEYDOWN VK_SHIFT or by the fallback below),
        // LMB-down enters OBJECT_Z drag mode for height adjustment. LMB-up
        // will then detach the preview, placing it permanently in the scene.
        // Matches the legacy editor. Do NOT enter a camera drag
        // — placement is the entire intent of this click while a preview
        // is alive.
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            m_dragMode     = DragMode::OBJECT_Z;
            m_dragStartCam = engine->GetCamera();
            m_dragStartX   = (short)LOWORD(lp);
            m_dragStartY   = (short)HIWORD(lp);
            SetCapture(hwnd);
            Log("[ArchC-engine] LMB-down OBJECT_Z drag (placing attached=%p)\n",
                static_cast<void*>(attached));
            return 0;
        }
        // Reference-object manipulator: if a handle (translate
        // arrow or rotate ring) is under the cursor, grab it (drag moves/rotates the
        // object) — this wins over camera orbit AND over the Shift+LMB particle-spawn
        // below (so Shift-clicking a handle is a precise grab, not a spawn).
        // PickManipulatorHandle returns NONE unless the object is selected, so a MISS
        // (including empty-space Shift-clicks) falls through to the spawn / camera path.
        {
            const Engine::ManipHandle h =
                engine->PickManipulatorHandle((short)LOWORD(lp), (short)HIWORD(lp));
            if (h.kind != Engine::ManipHandle::NONE)
            {
                m_manipKind     = h.kind;
                m_manipAxis     = h.axis;
                m_manipStartPos = engine->GetReferencePosition();
                m_manipStartRot = engine->GetReferenceRotation();
                if (h.kind == Engine::ManipHandle::TRANSLATE)
                {
                    // Grab offset (axis param at press) so the object doesn't jump
                    // on the first move; if degenerate, fall back to 0.
                    if (!engine->ManipulatorAxisParam((short)LOWORD(lp), (short)HIWORD(lp),
                                                      h.axis, m_manipStartPos, m_manipGrabT0))
                        m_manipGrabT0 = 0.0f;
                    m_manipPrevT  = m_manipGrabT0;   // seed accumulate-per-move (first move delta = 0 -> no jump)
                    m_manipAccumT = 0.0f;
                }
                else if (h.kind == Engine::ManipHandle::PLANE)
                {
                    // Seed prev from the in-plane offset at press so the first move
                    // delta is 0 (no jump); accumulators start at 0. Anchor to the FIXED
                    // grab position (m_manipStartPos) for the whole drag -- see below.
                    if (!engine->ManipulatorPlaneOffset((short)LOWORD(lp), (short)HIWORD(lp),
                                                        h.axis, m_manipStartPos, m_manipPrevU, m_manipPrevV))
                    { m_manipPrevU = 0.0f; m_manipPrevV = 0.0f; }
                    m_manipAccumU = 0.0f;
                    m_manipAccumV = 0.0f;
                }
                else   // ROTATE
                {
                    // Grab angle on the ring; the accumulator starts at 0 so the
                    // first move is a no-op delta (no jump). If degenerate, the
                    // first valid move re-seeds prev (accum stays 0 until then).
                    if (!engine->ManipulatorRingAngle((short)LOWORD(lp), (short)HIWORD(lp),
                                                      h.axis, m_manipGrabAngle))
                        m_manipGrabAngle = 0.0f;
                    m_manipPrevAngle  = m_manipGrabAngle;
                    m_manipAccumAngle = 0.0f;
                }
                // Tell the engine which handle is being dragged (drives the guide line / rotate sweep / dim).
                // Rotate seeds both angles to the grab angle (applied == grab at accum 0); translate uses 0.
                const float grabA = (h.kind == Engine::ManipHandle::ROTATE) ? m_manipGrabAngle : 0.0f;
                if (engine) engine->SetManipulatorActiveDrag(h, grabA, grabA);
                m_dragMode = DragMode::MANIPULATE;
                SetCapture(hwnd);
                return 0;
            }
        }
        // Round 5 fallback: Shift+LMB with no existing preview spawns
        // one in-place (covers the case where WM_KEYDOWN VK_SHIFT didn't
        // fire because WebView2 held focus). Then immediately enter
        // OBJECT_Z so the user can drag-Z in the same gesture and LMB-up
        // places it.
        if ((wp & MK_SHIFT) && particleSystem && !particleSystem->getEmitters().empty())
        {
            int cx = (short)LOWORD(lp);
            int cy = (short)HIWORD(lp);
            m_lastCursorX = cx;
            m_lastCursorY = cy;
            D3DXVECTOR3 pos;
            GetCursorPos3D(engine.get(), (short)cx, (short)cy, pos);
            m_mouseCursor.SetPosition(pos);
            m_attachedParticleSystem = engine->MakeInstanceHandle(
                engine->SpawnParticleSystem(*particleSystem, &m_mouseCursor));
            Log("[ArchC-engine] SHIFT+LMB spawn cx=%d cy=%d pos=(%.3f,%.3f,%.3f) result=%p\n",
                cx, cy, pos.x, pos.y, pos.z,
                static_cast<void*>(m_attachedParticleSystem.ptr));
#ifndef NDEBUG
            // Mirror the cursor-unproject
            // diagnostic at this alternate spawn entry. Consistent
            // grep prefix lets all three call sites (WM_MOUSEMOVE
            // throttled emit, WM_KEYDOWN VK_SHIFT, WM_LBUTTONDOWN
            // SHIFT-fallback) be filtered together.
            int dx, dy, dw, dh;
            const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
            Log("[cursor-unproject] SHIFT+LMB in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
                cx, cy,
                dscene ? "scene" : "full-rt",
                dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
                pos.x, pos.y, pos.z);
#endif
            m_dragMode     = DragMode::OBJECT_Z;
            m_dragStartCam = engine->GetCamera();
            m_dragStartX   = cx;
            m_dragStartY   = cy;
            SetCapture(hwnd);
            return 0;
        }
        // Click-to-select: clicking an UNLOCKED object body selects it (the gizmo
        // appears); clicking empty space deselects it and falls through to camera
        // MOVE. A LOCKED object is navigation-transparent — a body hit is NOT
        // consumed, so the click reaches the camera MOVE/ZOOM path below (matching
        // RMB orbit, which never picks). The consume rule lives in the unit-tested
        // RefLockConsumeBodyClick (RefLock.h). (Handle grabs above already won when
        // the object was selected; a locked object is never selected, so none exist.)
        {
            const bool hit = engine->PickReferenceObject((short)LOWORD(lp), (short)HIWORD(lp));
            if (RefLockConsumeBodyClick(hit, engine->IsReferenceLocked()))
            {
                engine->SetReferenceObjectSelected(true);
                return 0;   // consume — don't pan when clicking an unlocked object
            }
            // Miss, or a hit on a LOCKED object: deselect, then fall through to
            // camera MOVE/ZOOM. While locked this is idempotent — selection is
            // already forced false and hover/active manip already cleared.
            engine->SetReferenceObjectSelected(false);
        }
        // Plain LMB drag — camera MOVE / ZOOM (no preview involved).
        m_dragMode     = (wp & MK_CONTROL) ? DragMode::ZOOM : DragMode::MOVE;
        m_dragStartCam = engine->GetCamera();
        m_dragStartX   = (short)LOWORD(lp);
        m_dragStartY   = (short)HIWORD(lp);
        SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN:
    {
        if (!engine) return 0;
        // An RMB press mid-LMB-manipulate-drag takes over the mode;
        // commit the in-flight move first so its persist/dirty isn't dropped.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        }
        m_dragMode     = (wp & MK_CONTROL) ? DragMode::ZOOM : DragMode::ROTATE;
        m_dragStartCam = engine->GetCamera();
        m_dragStartX   = (short)LOWORD(lp);
        m_dragStartY   = (short)HIWORD(lp);
        SetCapture(hwnd);
        // See WM_LBUTTONDOWN — we don't SetFocus the hidden
        // popup, which would trigger the spurious WM_KILLFOCUS → kill loop.
        return 0;
    }
    case WM_LBUTTONUP:
    {
        // Manipulator drag release: commit the moved transform once
        // (gated persist + dirty + emit). Per-move only set+emitted (no persist),
        // so the registry/dirty flag is touched exactly once per gesture.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode  = DragMode::NONE;
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
            ReleaseCapture();
            return 0;
        }
        // Legacy parity: if a cursor-bound preview was being dragged
        // for placement (OBJECT_Z mode, or any state with an attached
        // preview), DETACH it now. After Detach the system stays in
        // the world at its current position and continues to emit —
        // it is no longer parented to m_mouseCursor. The user can
        // then click again (while still holding Shift) to spawn a
        // fresh preview, repeating the click-to-place gesture.
        // Matches the legacy editor.
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-engine] LMB-up placing attached=%p (Detach, system stays alive)\n",
                static_cast<void*>(attached));
            engine->DetachParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem.Reset();
        }
        m_dragMode = DragMode::NONE;
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONUP:
    {
        m_dragMode = DragMode::NONE;
        ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
    {
        // Capture lost (Alt-Tab away mid-drag, foreign SetCapture, etc.).
        // If a manipulator drag was interrupted, commit its current
        // position so the move isn't silently lost (the engine already holds the
        // last per-move position; CommitReferenceObjectTransform persists it).
        if (m_dragMode == DragMode::MANIPULATE && dispatcher)
            dispatcher->CommitReferenceObjectTransform();
        // Drop drag state so the next mouse-move doesn't ride a stale start camera
        // / grabbed axis.
        m_dragMode  = DragMode::NONE;
        ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!engine) return 0;

        int mx = (short)LOWORD(lp);
        int my = (short)HIWORD(lp);
        m_lastCursorX = mx;
        m_lastCursorY = my;

        // Manipulator drag. TRANSLATE: accumulate precision-
        // scaled per-move axis-param deltas (m_manipAccumT += (now - prev) * factor)
        // and apply newPos = startPos + axis*m_manipAccumT — with factor==1 this
        // telescopes to (now - grab), i.e. the old absolute-from-grab; factor=0.2 while
        // Shift is held gives a finer drag with no jump on toggle. ROTATE: accumulate
        // wrapped, precision-scaled per-move ring-angle deltas onto the snapshot
        // rotation's Euler component for that ring (no jump, multi-turn). When
        // engine->GetSnapEnabled(), TRANSLATE rounds X/Y to the grid spacing (Z/height
        // free) and ROTATE rounds the driven Euler component to 15° (both finer by the
        // same factor while Shift held). Only set + emit here so the picker spinners
        // track live; persistence is deferred to LMB-up.
        if (m_dragMode == DragMode::MANIPULATE && m_manipAxis >= 0)
        {
            // [gizmo-drag-teardown] A drag continues only while the object is still
            // selected AND unlocked. An out-of-band clear / mod-switch / new-file /
            // deselect / lock deselects it (lock deselects too), so self-abort here
            // before reading or applying any move — we must never drag a stale/gone/
            // frozen object. Fully end the gesture like the per-site drag-end tail, but
            // WITHOUT committing (we must not persist a stale transform): ResetManipDragState
            // zeroes accumulators + clears the engine active-drag guides; clear m_dragMode +
            // ReleaseCapture so the eventual LBUTTONUP doesn't commit a phantom dirty/registry
            // write. Set m_dragMode=NONE BEFORE ReleaseCapture so the WM_CAPTURECHANGED it posts
            // sees NONE and no-ops. Reuses the unit-tested freeze/lock predicate (RefLock.h).
            if (!RefLockResolveSelected(engine->IsReferenceObjectSelected(),
                                        engine->IsReferenceLocked()))
            {
                ResetManipDragState();
                m_dragMode = DragMode::NONE;
                ReleaseCapture();
                return 0;
            }
            const float factor = (wp & MK_SHIFT) ? 0.2f : 1.0f;   // read wParam, NOT GetKeyState
            bool moved = false;
            if (m_manipKind == Engine::ManipHandle::TRANSLATE)
            {
                float tNow;
                if (engine->ManipulatorAxisParam((short)mx, (short)my, m_manipAxis,
                                                 m_manipStartPos, tNow))
                {
                    m_manipAccumT += (tNow - m_manipPrevT) * factor;   // precision-scaled per-move delta
                    m_manipPrevT   = tNow;
                    const D3DXVECTOR3 ax(m_manipAxis == 0 ? 1.0f : 0.0f,
                                         m_manipAxis == 1 ? 1.0f : 0.0f,
                                         m_manipAxis == 2 ? 1.0f : 0.0f);
                    D3DXVECTOR3 newPos = m_manipStartPos + ax * m_manipAccumT;   // note: NOT const now
                    if (engine->GetSnapEnabled())                  // snap X/Y to grid; Z (height) free
                    {
                        const float step = engine->GetGridSpacing() * factor;    // finer step when Shift held
                        if (step > 0.0f) { newPos.x = roundf(newPos.x / step) * step; newPos.y = roundf(newPos.y / step) * step; }
                    }
                    // Capture the pre-drag transform ONCE, on the
                    // first move that ACTUALLY changes the position, BEFORE
                    // mutating — the engine still holds the grab-time transform
                    // here, so this is the PRE state. Gating on a real change
                    // (not just a successful projection) avoids a phantom undo
                    // step from a zero-delta first move — notably under snap,
                    // where a tiny move can round back to the grab point.
                    if (!m_manipUndoCaptured && newPos != m_manipStartPos) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
                    }
                    engine->SetReferenceObjectTransform(newPos, m_manipStartRot);
                    m_readoutKind = "translate";
                    m_readoutLabels[0] = manipreadout::AxisName(m_manipAxis);
                    m_readoutValues[0] = (&newPos.x)[m_manipAxis];
                    m_readoutN = 1; m_readoutDecimals = 1;
                    moved = true;
                }
            }
            else if (m_manipKind == Engine::ManipHandle::PLANE)
            {
                float uNow, vNow;
                // Anchor to the FIXED grab position (m_manipStartPos), NOT the live object
                // origin -- decomposing against the moving object fed its own motion back
                // into the delta and oscillated the position (mirrors the arrow's
                // ManipulatorAxisParam, which also anchors to m_manipStartPos).
                if (engine->ManipulatorPlaneOffset((short)mx, (short)my, m_manipAxis, m_manipStartPos, uNow, vNow))
                {
                    m_manipAccumU += (uNow - m_manipPrevU) * factor;   // precision-scaled per-move
                    m_manipAccumV += (vNow - m_manipPrevV) * factor;
                    m_manipPrevU = uNow;  m_manipPrevV = vNow;
                    // Compose via the unit-tested pure helper (basis = (normal+1,normal+2);
                    // ground normal 2 -> (X,Y); Z stays == start, structurally).
                    float np[3];
                    planehandle::ComposePlanePos(&m_manipStartPos.x, m_manipAxis,
                                                 m_manipAccumU, m_manipAccumV, np);
                    D3DXVECTOR3 newPos(np[0], np[1], np[2]);
                    if (engine->GetSnapEnabled())
                    {
                        // NOTE: snapping .x/.y is correct ONLY for the ground (normal-Z)
                        // plane, whose free axes ARE X and Y. A future YZ/ZX plane would
                        // need to snap ITS in-plane components -- revisit snap when adding
                        // those (YZ/ZX deferred).
                        const float step = engine->GetGridSpacing() * factor;
                        if (step > 0.0f) { newPos.x = roundf(newPos.x / step) * step;
                                           newPos.y = roundf(newPos.y / step) * step; }
                    }
                    if (!m_manipUndoCaptured && newPos != m_manipStartPos) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
#ifndef NDEBUG
                        Log("[Plane] grab-capture axis=%d accumUV=(%.3f,%.3f) newPos=(%.3f,%.3f,%.3f)\n",
                            m_manipAxis, m_manipAccumU, m_manipAccumV, newPos.x, newPos.y, newPos.z);
#endif
                    }
                    engine->SetReferenceObjectTransform(newPos, m_manipStartRot);
                    { int pu, pv; manipreadout::InPlaneAxes(m_manipAxis, pu, pv);
                      m_readoutKind = "plane";
                      m_readoutLabels[0] = manipreadout::AxisName(pu); m_readoutValues[0] = (&newPos.x)[pu];
                      m_readoutLabels[1] = manipreadout::AxisName(pv); m_readoutValues[1] = (&newPos.x)[pv];
                      m_readoutN = 2; m_readoutDecimals = 1; }
                    // active-drag (which drives the dim + faint X/Y guides) was set once at
                    // grab and never changes for a plane drag -- no per-move re-set needed
                    // (matches the TRANSLATE branch; only ROTATE re-sets, to feed live angles).
                    moved = true;
                }
            }
            else   // ROTATE
            {
                float aNow;
                if (engine->ManipulatorRingAngle((short)mx, (short)my, m_manipAxis, aNow))
                {
                    auto wrapPi = [](float a) {
                        const float twoPi = 2.0f * D3DX_PI;
                        while (a >   D3DX_PI) a -= twoPi;
                        while (a <= -D3DX_PI) a += twoPi;
                        return a;
                    };
                    m_manipAccumAngle += wrapPi(aNow - m_manipPrevAngle) * factor;   // precision
                    m_manipPrevAngle   = aNow;
                    // Euler component this ring drives (m_referenceRotation = [yaw=Z,
                    // pitch=X, roll=Y]): ring axis 2(Z)->yaw(.x), 0(X)->pitch(.y),
                    // 1(Y)->roll(.z).
                    const int comp = (m_manipAxis == 2) ? 0 : (m_manipAxis == 0) ? 1 : 2;
                    D3DXVECTOR3 newRot = m_manipStartRot;
                    (&newRot.x)[comp] += m_manipAccumAngle * (180.0f / D3DX_PI);
                    if (engine->GetSnapEnabled())                  // snap rotation to 15deg (3deg w/ Shift)
                    {
                        const float s = 15.0f * factor;
                        (&newRot.x)[comp] = roundf((&newRot.x)[comp] / s) * s;
                    }
                    // Capture the pre-drag transform ONCE, on the
                    // first move that ACTUALLY changes the rotation, BEFORE
                    // mutating (engine still at the grab-time transform → PRE
                    // state). Gating on a real change avoids a phantom undo step
                    // from a zero-delta first move (e.g. snap rounding back).
                    if (!m_manipUndoCaptured && newRot != m_manipStartRot) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
                    }
                    engine->SetReferenceObjectTransform(m_manipStartPos, newRot);
                    m_readoutKind = "rotate";
                    // Label = the WORLD AXIS the ring spins about (X/Y/Z); the value is the
                    // rotation about that axis = the Euler component RingComp(axis) selects.
                    m_readoutLabels[0] = manipreadout::AxisName(m_manipAxis);
                    m_readoutValues[0] = (&newRot.x)[comp];
                    m_readoutN = 1; m_readoutDecimals = 0;
                    // Push the active-drag AFTER snap so the rotate sweep's "applied" radial
                    // tracks the orientation the object ACTUALLY shows (snapped / precision-scaled),
                    // not the raw accumulator -- under snap the two would diverge by up to the snap
                    // step. Derive the applied angle from the final Euler delta in this ring's plane;
                    // with snap off this reduces to grab + m_manipAccumAngle (unchanged behavior).
                    Engine::ManipHandle activeH; activeH.kind = m_manipKind; activeH.axis = m_manipAxis;
                    const float appliedRad = m_manipGrabAngle
                        + ((&newRot.x)[comp] - (&m_manipStartRot.x)[comp]) * (D3DX_PI / 180.0f);
                    if (engine) engine->SetManipulatorActiveDrag(activeH, m_manipGrabAngle, appliedRad);
                    moved = true;
                }
            }
            if (moved && dispatcher)
            {
                const DWORD now = GetTickCount();
                // The HEAVY full-snapshot (drives the picker's numeric spinners)
                // stays throttled to ~30 Hz so a fast drag doesn't flood the bridge;
                // the final exact transform emits on release (commit).
                if ((now - m_lastManipEmitTick) >= 33)
                {
                    dispatcher->EmitEngineStateChanged();
                    m_lastManipEmitTick = now;
                }
                // The readout pill payload is tiny (nx/ny + a few values). It looked
                // "laggy / stuttery" because it was chained to the 30 Hz snapshot gate
                // above, while the object + gizmo move at frame rate -- so the chip
                // lagged and stepped. Emit it EVERY move instead: WM_MOUSEMOVE is
                // already coalesced to input cadence by Windows, so this glides with
                // the object without flooding the heavy path. (After the drag-time
                // ease bypass, GetReferencePosition() == the drawn position, so the
                // projected pill sits on the object.)
                int vx, vy, vw, vh;
                manipreadout::ViewportPoint vp{0,0,false};
                if (engine->GetSceneViewport(vx, vy, vw, vh))
                    vp = manipreadout::ProjectToViewport(engine->GetReferencePosition(),
                                                         engine->GetViewProjection(), vw, vh);
                nlohmann::json vals = nlohmann::json::array(), labels = nlohmann::json::array();
                for (int i = 0; i < m_readoutN; ++i) { vals.push_back(m_readoutValues[i]); labels.push_back(m_readoutLabels[i]); }
                dispatcher->EmitManipulatorDrag({
                    {"active", true}, {"kind", m_readoutKind},
                    {"nx", vp.nx}, {"ny", vp.ny}, {"visible", vp.visible},
                    {"labels", labels}, {"values", vals}, {"decimals", m_readoutDecimals},
                });
            }
            return 0;
        }

        // Hover feedback: when idle (not dragging), highlight the
        // handle under the cursor. PickManipulatorHandle returns NONE unless an object
        // is selected, so this is a cheap no-op when there's nothing to hover.
        if (m_dragMode == DragMode::NONE)
            engine->SetManipulatorHover(engine->PickManipulatorHandle((short)mx, (short)my));

        // Legacy parity: in OBJECT_Z drag (placing a cursor-bound preview),
        // only Z tracks the drag. X/Y stay frozen at the click position so
        // the user can rake the mouse vertically to set height without the
        // preview sliding sideways. Matches the legacy editor.
        if (m_dragMode == DragMode::OBJECT_Z)
        {
            long y = my - m_dragStartY;
            D3DXVECTOR3 diff = m_dragStartCam.Target - m_dragStartCam.Position;
            float len = D3DXVec3Length(&diff);
            D3DXVECTOR3 pos = m_mouseCursor.GetPosition();
            pos.z = -static_cast<float>(y) * len / 1000.0f;
            m_mouseCursor.SetPosition(pos);
            return 0;
        }

        // shift-click-to-spawn: always-update cursor block, regardless
        // of (non-OBJECT_Z) drag mode. Mirrors the legacy editor
        // — without this, the attached ParticleSystemInstance (parented to
        // m_mouseCursor via Object3D) wouldn't track the mouse during
        // Shift-hold. Cache the (x,y) so WM_KEYDOWN can use it for the
        // spawn coords (WM_KEYDOWN's lParam is NOT mouse coords; a
        // legacy editor bug).
        D3DXVECTOR3 cursorWorld;
        GetCursorPos3D(engine.get(), (short)mx, (short)my, cursorWorld);
        m_mouseCursor.SetPosition(cursorWorld);

        // Push the world-space cursor to the React
        // status bar, throttled. 33 ms ≈ 30 Hz — fast enough to read,
        // slow enough that the bridge channel doesn't bottleneck.
        const DWORD now = GetTickCount();
        if (dispatcher && (now - m_lastCursorEmitTick) >= 33u)
        {
            m_lastCursorEmitTick = now;
            dispatcher->EmitCursorPosition3D(cursorWorld.x, cursorWorld.y, cursorWorld.z);
#ifndef NDEBUG
            // Throttled diagnostic for the
            // cursor-unproject path. Piggybacks on the bridge-emit gate
            // so the cadence is ~30 Hz (rather than per-WM_MOUSEMOVE,
            // which is 60+ Hz and would flood host.log). `mode` names
            // which viewport GetCursorPos3D used — `scene` under
            // composition mode, `full-rt` under legacy
            // mode (or pre-scene-rect-dispatch boot).
            int dx, dy, dw, dh;
            const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
            Log("[cursor-unproject] in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
                mx, my,
                dscene ? "scene" : "full-rt",
                dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
                cursorWorld.x, cursorWorld.y, cursorWorld.z);
#endif
        }

        if (m_dragMode == DragMode::NONE) return 0;

        long x = mx - m_dragStartX;
        long y = my - m_dragStartY;

        Engine::Camera camera = m_dragStartCam;
        D3DXVECTOR3    orthVec;
        D3DXVECTOR3    diff = m_dragStartCam.Position - m_dragStartCam.Target;

        // Orthogonal vector in the camera plane (as in the legacy editor).
        D3DXVec3Cross(&orthVec, &diff, &camera.Up);
        D3DXVec3Normalize(&orthVec, &orthVec);

        if (m_dragMode == DragMode::ROTATE)
        {
            // Orbit Position around Target. Z rotation around camera-up
            // axis (horizontal drag); XY rotation around orthVec
            // (vertical drag). /2.0f keeps a full-window drag at ~180°.
            D3DXMATRIX rotateXY, rotateZ, rotate;
            D3DXMatrixRotationZ(&rotateZ, -D3DXToRadian(x / 2.0f));
            D3DXMatrixRotationAxis(&rotateXY, &orthVec, D3DXToRadian(y / 2.0f));
            D3DXMatrixMultiply(&rotate, &rotateXY, &rotateZ);
            D3DXVec3TransformCoord(&camera.Position, &diff, &rotate);
            camera.Position += camera.Target;
        }
        else if (m_dragMode == DragMode::MOVE)
        {
            // Translate Target (Position rides along). Multiplier scales
            // with distance so a far camera moves proportionally faster —
            // legacy comment: "Large distance: move a lot, small
            // distance: move a little".
            D3DXVECTOR3 Up;
            D3DXVec3Cross(&Up, &orthVec, &diff);
            D3DXVec3Normalize(&Up, &Up);

            float multiplier = D3DXVec3Length(&diff) / 1000;

            camera.Target  += (float)x * multiplier * orthVec;
            camera.Target  += (float)y * multiplier * Up;
            camera.Position = diff + camera.Target;
        }
        else if (m_dragMode == DragMode::ZOOM)
        {
            // Scale (Position - Target) by a sqrt(distance)-based
            // factor. Floor at 1.0f to prevent flipping through the
            // target. -y so dragging up zooms in (matches legacy).
            // olddist > 0 guard: a coincident eye/target would
            // divide by zero and push a NaN camera into the engine.
            float olddist = D3DXVec3Length(&diff);
            if (olddist > 0.0f)
            {
                float newdist = max(1.0f, olddist - sqrtf(olddist) * (float)-y);
                D3DXVec3Scale(&camera.Position, &diff, newdist / olddist);
                camera.Position += camera.Target;
            }
        }

        engine->SetCamera(camera);
        if (dispatcher) dispatcher->EmitEngineStateChanged();
        return 0;
    }
    // -----------------------------------------------------------------
    // shift-click-to-spawn — cursor-bound particle system.
    //
    // Hold Shift over the viewport to spawn an instance of the active
    // ParticleSystem parented to m_mouseCursor. Drag the mouse to fling
    // it around; release Shift to kill it. Matches the legacy editor.
    //
    // Cursor-coords-on-KEYDOWN: WM_KEYDOWN's lParam is repeat-count +
    // scan-code + flags — NOT mouse coords. Legacy reads `LOWORD(lParam),
    // HIWORD(lParam)` and gets garbage; instead we use m_lastCursorX/Y
    // cached from WM_MOUSEMOVE. Fallback (cache stale or zero at boot):
    // GetCursorPos + ScreenToClient.
    // -----------------------------------------------------------------
    case WM_KEYDOWN:
    {
        if (wp != VK_SHIFT || !engine) break;
        // Filter auto-repeats. WM_KEYDOWN sets bit 30 of lParam on
        // repeat presses; clear bit 30 means initial press. Legacy
        // `(~lParam & 0x40000000)` test.
        if (lp & 0x40000000) return 0;
        // Spawn precondition: a non-empty ParticleSystem and no
        // attached instance already. Empty-system guard goes beyond
        // legacy's `particleSystem != NULL` to also require >= 1
        // root emitter — SpawnParticleSystem on an emitter-less system
        // misbehaves.
        if (LiveAttachedSystem() != nullptr) return 0;
        if (!particleSystem || particleSystem->getEmitters().empty()) return 0;

        // Resolve cursor coords. Prefer the cached MOUSEMOVE position;
        // fall back to GetCursorPos+ScreenToClient if the cache hasn't
        // been seeded (e.g. user pressed Shift before moving the mouse
        // over the viewport at all).
        int cx = m_lastCursorX;
        int cy = m_lastCursorY;
        if (cx == 0 && cy == 0)
        {
            POINT pt = {};
            if (GetCursorPos(&pt))
            {
                ScreenToClient(hwnd, &pt);
                cx = pt.x;
                cy = pt.y;
            }
        }

        D3DXVECTOR3 pos;
        GetCursorPos3D(engine.get(), (short)cx, (short)cy, pos);
        m_mouseCursor.SetPosition(pos);
        m_attachedParticleSystem = engine->MakeInstanceHandle(
            engine->SpawnParticleSystem(*particleSystem, &m_mouseCursor));
#ifndef NDEBUG
        // One-shot diagnostic at the actual
        // spawn site so a misplaced spawn can be tied to the input
        // coords + viewport in host.log without re-running with a
        // breakpoint. Per-Shift-press, not per-frame, so untrottled.
        int dx, dy, dw, dh;
        const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
        Log("[cursor-unproject] SPAWN in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
            cx, cy,
            dscene ? "scene" : "full-rt",
            dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
            pos.x, pos.y, pos.z);
#endif
        return 0;
    }
    case WM_KEYUP:
    {
        if (wp != VK_SHIFT) break;
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_KEYUP VK_SHIFT killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem.Reset();
        }
        return 0;
    }
    case WM_KILLFOCUS:
    {
        // End an in-flight gizmo drag on focus loss (archC routes Alt-Tab here as window.blur;
        // WM_CAPTURECHANGED may not fire for the hidden popup). Commit the moved transform so it isn't
        // lost, then clear drag + active-guide state. A spurious archC focus-churn mid-drag would also end
        // the drag, but commit preserves the position (accepted tradeoff -- a captured drag rarely churns).
        if (m_dragMode == DragMode::MANIPULATE) {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode  = DragMode::NONE;
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        }
        // Defensive: if the viewport loses focus while Shift is held
        // (Alt-Tab away, foreign focus steal), WM_KEYUP may never arrive
        // and the attached instance leaks. Drop it here.
        //
        // The viewport popup is hidden and never genuinely
        // owns focus, but receives spurious WM_KILLFOCUS from Win32 focus
        // churn whenever ANY focus assignment touches it (other apps
        // activating, modal dialogs, etc.). Treating those as user-Alt-Tab
        // triggers and killing the cursor-bound spawn is a regression, so we
        // suppress the OS-driven kill here. (The legitimate blur case is handled
        // renderer-side: window.blur → viewport/input { type:"blur" } → the
        // private WM_APP_VIEWPORT_BLUR message below, which DOES end the spawn.)
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_KILLFOCUS suppressed (attached=%p preserved)\n",
                static_cast<void*>(attached));
        }
        return 0;
    }
    case WM_APP_VIEWPORT_BLUR:
    {
        // Genuine renderer viewport blur (window.blur via InputDispatcher) -- end
        // any cursor-bound Shift spawn. Distinct from the OS WM_KILLFOCUS above
        // (suppressed for Win32 focus churn) so a real blur can't leak the
        // attached preview. Tear down an in-flight OBJECT_Z
        // placement drag first: m_dragMode = NONE BEFORE ReleaseCapture (the gizmo
        // teardown order). No-op when nothing is attached / no drag.
        //
        // Preserve the WM_KILLFOCUS MANIPULATE behavior the renderer blur used to
        // trigger (it previously routed through WM_KILLFOCUS): commit an in-flight
        // gizmo drag so its moved transform isn't lost. Idempotent — if the OS
        // WM_KILLFOCUS also fires, whichever runs first sets m_dragMode=NONE and
        // the other skips.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode = DragMode::NONE;
            ResetManipDragState();
        }
        if (m_dragMode == DragMode::OBJECT_Z)
        {
            m_dragMode = DragMode::NONE;
            ReleaseCapture();
        }
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_APP_VIEWPORT_BLUR killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem.Reset();
        }
        return 0;
    }
    case WM_DESTROY:
    {
        // Viewport HWND is going away. Defensively drop any attached
        // instance before the Engine tears down (Engine reset happens
        // on the main window's WM_DESTROY which fires after this).
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_DESTROY killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem.Reset();
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        // Wheel-zoom only when no drag is in progress (as in the legacy editor).
        // wParam high word is the wheel delta in WHEEL_DELTA units (120).
        if (m_dragMode != DragMode::NONE || !engine) return 0;

        Engine::Camera camera = engine->GetCamera();
        D3DXVECTOR3    diff   = camera.Position - camera.Target;

        float olddist = D3DXVec3Length(&diff);
        // Same olddist > 0 guard as the drag-zoom path.
        if (!(olddist > 0.0f)) return 0;
        float wheel   = (float)((SHORT)HIWORD(wp)) / (float)WHEEL_DELTA;
        float newdist = max(1.0f, olddist - sqrtf(olddist) * wheel);
        D3DXVec3Scale(&camera.Position, &diff, newdist / olddist);
        camera.Position += camera.Target;

        engine->SetCamera(camera);
        if (dispatcher) dispatcher->EmitEngineStateChanged();
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace host
