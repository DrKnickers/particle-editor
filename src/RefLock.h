#pragma once
// freeze/lock] The single rule for whether the reference object is SELECTED
// (gizmo shown + grabbable). A locked object is never selected, regardless of what
// any call site requests. Every selection path computes through this one function --
// the viewport click-select setter, the auto-select-on-load, and SetReferenceLocked --
// so the unit-tested rule IS the shipped rule (the GizmoSizing.h / PlaneHandle.h
// precedent) and an auto-select path can't silently re-show the gizmo on a locked object.
inline bool RefLockResolveSelected(bool wantSelected, bool locked)
{
    return wantSelected && !locked;
}
