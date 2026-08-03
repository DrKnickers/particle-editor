#pragma once
#include <d3dx9math.h>
// Pure builder for the reference-object world matrix, extracted from
// Engine::ReferenceObjectWorldFrom so the scale/rotation math is unit-tested
// headlessly (the GizmoSizing.h / ManipReadout.h / PlaneHandle.h precedent).
//
// Engine is Z-UP: yaw->Z, pitch->X, roll->Y, with yaw applied LAST (outermost)
// so it turns the already-tilted object about world up. rotDeg is [yaw,pitch,roll]
// in degrees (the wire convention: schema + BridgeDispatcher).
//
// scaleFactor is the per-object <Scale_Factor> render multiplier (1.0 = native).
// It is applied LEFTMOST in the row-vector (v*M) product -- i.e. FIRST, to model
// space, about the object origin -- so the model grows in place and the scale
// rides through hardpoint mounts (composed sub.placement * boneMatrix * objectWorld)
// and the skinned palette unchanged. See the design spec.
inline D3DXMATRIX ReferenceObjectWorldMatrix(const D3DXVECTOR3& pos,
                                             const D3DXVECTOR3& rotDeg,
                                             float scaleFactor)
{
    const float deg2rad = D3DX_PI / 180.0f;
    D3DXMATRIX s, mYaw, mPitch, mRoll, trans;
    D3DXMatrixScaling(&s, scaleFactor, scaleFactor, scaleFactor);
    D3DXMatrixRotationZ(&mYaw,   rotDeg.x * deg2rad);   // yaw   = heading about world up (Z)
    D3DXMatrixRotationX(&mPitch, rotDeg.y * deg2rad);   // pitch = tilt about X
    D3DXMatrixRotationY(&mRoll,  rotDeg.z * deg2rad);   // roll  = bank about Y
    D3DXMatrixTranslation(&trans, pos.x, pos.y, pos.z);
    return s * mRoll * mPitch * mYaw * trans;   // s LEFTMOST = applied first (row-vector v*M)
}
