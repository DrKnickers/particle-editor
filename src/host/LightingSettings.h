#pragma once

#include <windows.h>

namespace host {

// On-disk registry value names for the persisted lighting split. Floats are
// REG_BINARY; colours + the force-align flag are REG_DWORD. ONE definition —
// consumed by the HostWindow WM_CREATE restore and by both BridgeDispatch_Spawner
// lighting handlers (get + set). The matching TS defaults live in
// web/apps/editor/src/screens/LightingPanel.tsx (kept in lockstep by hand).
constexpr const wchar_t* kLightSunIntensity       = L"LightSunIntensity";
constexpr const wchar_t* kLightSunZAngle          = L"LightSunZAngle";
constexpr const wchar_t* kLightSunTilt            = L"LightSunTilt";
constexpr const wchar_t* kLightSunAmbientColor    = L"LightSunAmbientColor";
constexpr const wchar_t* kLightSunSpecularColor   = L"LightSunSpecularColor";
constexpr const wchar_t* kLightSunDiffuseColor    = L"LightSunDiffuseColor";
constexpr const wchar_t* kLightSunShadowColor     = L"LightSunShadowColor";
constexpr const wchar_t* kLightForceFillAlignment = L"LightingForceFillAlignment";
constexpr const wchar_t* kLightFill1Intensity     = L"LightFill1Intensity";
constexpr const wchar_t* kLightFill1ZAngle        = L"LightFill1ZAngle";
constexpr const wchar_t* kLightFill1Tilt          = L"LightFill1Tilt";
constexpr const wchar_t* kLightFill1DiffuseColor  = L"LightFill1DiffuseColor";
constexpr const wchar_t* kLightFill2Intensity     = L"LightFill2Intensity";
constexpr const wchar_t* kLightFill2ZAngle        = L"LightFill2ZAngle";
constexpr const wchar_t* kLightFill2Tilt          = L"LightFill2Tilt";
constexpr const wchar_t* kLightFill2DiffuseColor  = L"LightFill2DiffuseColor";

// Defaults (verbatim from the legacy Win32 dialog).
constexpr float kSunIntensityDefault   = 0.50f;
constexpr float kSunZAngleDefault      = 0.0f;
constexpr float kSunTiltDefault        = 45.0f;
constexpr float kFill1IntensityDefault = 0.50f;
constexpr float kFill1ZAngleDefault    = 120.0f;
constexpr float kFill1TiltDefault      = -10.0f;
constexpr float kFill2IntensityDefault = 0.50f;
constexpr float kFill2ZAngleDefault    = 210.0f;
constexpr float kFill2TiltDefault      = -10.0f;
constexpr bool  kForceAlignDefault     = true;
inline COLORREF SunAmbientColorDefault()   { return RGB(40, 40, 50); }
inline COLORREF SunSpecularColorDefault()  { return RGB(190, 190, 200); }
inline COLORREF SunDiffuseColorDefault()   { return RGB(180, 180, 190); }
inline COLORREF SunShadowColorDefault()    { return RGB(100, 100, 110); }
inline COLORREF Fill1DiffuseColorDefault() { return RGB(60, 80, 160); }
inline COLORREF Fill2DiffuseColorDefault() { return RGB(60, 80, 160); }

// Force-align fill azimuths (verbatim from the legacy dialog): when ON the fill
// angles derive from the sun (sun.z + 120° / + 210°, both at -10° tilt); the
// offsets intentionally equal the fill default azimuths. When OFF the persisted
// free-edit angles pass through unchanged.
struct FillAngles { float fill1Z, fill1Tilt, fill2Z, fill2Tilt; };
inline FillAngles ForceAlignFillAngles(bool forceAlign, float sunZ,
    float fill1Zp, float fill1Tiltp, float fill2Zp, float fill2Tiltp)
{
    if (forceAlign) return { sunZ + 120.0f, -10.0f, sunZ + 210.0f, -10.0f };
    return { fill1Zp, fill1Tiltp, fill2Zp, fill2Tiltp };
}

} // namespace host
