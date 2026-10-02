#include "RestoredSettings.h"

#include "LightingSettings.h"
#include "SettingsRegistry.h"

#include <cwchar>

namespace host {
namespace {

// Mirrors Engine::kGroundTextureCount, Engine::kSkydomeSlotCount, and
// Engine::kSkydomeFirstCustomSlot. Keep these tiny engine-free reader bounds
// in lockstep with the Engine constants.
constexpr int kGroundTextureCount = 8;
constexpr int kSkydomeSlotCount = 12;
constexpr int kSkydomeFirstCustomSlot = 9;

} // namespace

RestoredSettings ReadRestoredSettings(HKEY hKey, bool inCaptureMode)
{
    RestoredSettings s = {};
    DWORD dw = 0;

    if (ReadRegDword(hKey, L"BloomEnabled", dw))
        s.bloomEnabled = (dw != 0);

    // REG_BINARY float; reject NaN/Inf so a corrupt blob can't
    // drive bloom into a silly state (matches legacy's check).
    float f = 0.0f;
    if (ReadRegFloat(hKey, L"BloomStrength", f)) s.bloomStrength = f;
    if (ReadRegFloat(hKey, L"BloomCutoff", f))   s.bloomCutoff = f;
    if (ReadRegFloat(hKey, L"BloomSize", f))     s.bloomSize = f;

    // [view-settings-restore, session 11] Mirror legacy
    // main.cpp's startup restore so the
    // new-UI viewport opens with the user's persisted
    // background / ground / skydome instead of engine ctor
    // defaults. Same value names/types legacy reads, so
    // settings round-trip between the two UIs. Same
    // !useTestHost gate as bloom: the a11y goldens (e.g.
    // dialog-lighting's "Show ground" toggle) must see
    // deterministic ctor defaults. GroundZ is intentionally
    // NOT restored — legacy resets it to 0 each launch by
    // design.
    if (ReadRegDword(hKey, L"BackgroundColor", dw))
        s.backgroundColor = static_cast<COLORREF>(dw);
    if (ReadRegDword(hKey, L"ShowGround", dw))
        s.showGround = (dw != 0);

    // Ground texture: per-slot custom paths BEFORE the
    // selected index, so SetGroundTexture can find the right
    // source for a custom slot (ordering is load-bearing).
    for (int slot = 0; slot < kGroundTextureCount; ++slot)
    {
        wchar_t name[32];
        swprintf_s(name, L"GroundTextureSlot%d", slot);
        std::wstring path = ReadRegSz(hKey, name);
        if (!path.empty())
            s.groundSlotPaths.push_back({ slot, path });
    }
    if (ReadRegDword(hKey, L"GroundSolidColor", dw))
        s.groundSolidColor = static_cast<COLORREF>(dw);
    if (ReadRegDword(hKey, L"GroundTexture", dw)
        && dw < static_cast<DWORD>(kGroundTextureCount))
        s.groundTexture = static_cast<int>(dw);

    // Skydome: custom paths first so SetSkydomeSlot can reload
    // a previously-active custom slot.
    for (int slot = kSkydomeFirstCustomSlot; slot < kSkydomeSlotCount; ++slot)
    {
        wchar_t name[64];
        swprintf_s(name, L"SkydomeCustomSlot%d", slot);
        s.skydomeCustomPaths.push_back({ slot, ReadRegSz(hKey, name) });
    }
    if (ReadRegDword(hKey, L"SkydomeIndex", dw)
        && static_cast<int>(dw) < kSkydomeSlotCount)
        s.skydomeSlot = static_cast<int>(dw);

    // Game-dome environment: battle context + the two chosen
    // GameObject Names. This restore block runs after the device is
    // up, so SetSkydomeEnvironment resolves + uploads the meshes now
    // (the only place the new UI re-resolves a name-based selection).
    s.skydomePrimaryName = ReadRegSz(hKey, L"SkydomePrimaryName");
    s.skydomeSecondaryName = ReadRegSz(hKey, L"SkydomeSecondaryName");
    if (!s.skydomePrimaryName.empty() || !s.skydomeSecondaryName.empty())
    {
        s.hasSkydomeEnv = true;
        ReadRegDword(hKey, L"SkydomeContext", s.skydomeContextRaw);
    }

    // Imported reference object + unit grid. At
    // startup the catalog isn't built yet, so SetReferenceObject DEFERS:
    // it kicks the off-thread catalog build and the mesh resolves/uploads
    // a frame or more later, once Update() harvests the catalog and reruns
    // the deferred rebuild (so a restored object isn't on the first frame).
    // Transform / grid spacing are REG_BINARY floats; visibility / grid
    // toggle / snap toggle are REG_DWORD.
    std::array<float, 6> xform = {};
    // Reject NaN/Inf so a corrupt blob can't poison the transform matrix
    // (mirrors the bloom readF guard above).
    if (ReadRegFloatArray(hKey, L"ReferenceObjectTransform", xform.data(),
                          static_cast<DWORD>(xform.size())))
        s.refTransform = xform;
    if (ReadRegDword(hKey, L"ReferenceObjectVisible", dw))
        s.refVisible = (dw != 0);
    if (ReadRegDword(hKey, L"GridVisible", dw))
        s.gridVisible = (dw != 0);
    if (ReadRegFloat(hKey, L"GridSpacing", f))
        s.gridSpacing = f;
    // Persistent gizmo snap toggle (REG_DWORD, like GridVisible).
    if (ReadRegDword(hKey, L"SnapEnabled", dw))
        s.snapEnabled = (dw != 0);
    // Restore the persisted lock so a frozen
    // object comes back frozen. (Ordering vs. the Name read isn't
    // load-bearing: the silent restore force-deselects below
    // regardless, so the object lands deselected either way — the
    // lock flag just needs to be set before the user can interact.)
    if (ReadRegDword(hKey, L"ReferenceObjectLocked", dw))
        s.refLocked = (dw != 0);
    // Name LAST so the mesh loads once with the transform in
    // place; guard on non-empty so an unset selection doesn't
    // clobber a debug ALO_LT7_TEST_OBJECT env-hook mesh.
    //
    // In headless --capture mode NEVER restore the persisted
    // reference object: the capture supplies its own object (the
    // ALO_LT7_TEST_OBJECT env hook, or --capture-ref via
    // SetReferenceObject below), and restoring here would both
    // clobber that mesh AND force the capture script to mutate the
    // registry to suppress it — which, if the script is interrupted,
    // wipes the user's saved selection. Skipping makes captures
    // registry-inert and crash-safe by construction.
    if (!inCaptureMode)
    {
        std::wstring name = ReadRegSz(hKey, L"ReferenceObjectName");
        if (!name.empty())
            s.refName = std::move(name);
    }

    // [lighting-restore, session 12] Restore the persisted
    // lighting (sun / fill1 / fill2 angles + colours +
    // intensities, ambient, shadow) so the new-UI viewport
    // opens with the user's saved lights instead of engine
    // ctor defaults. Mirrors the legacy `PushLightingToEngine`
    // (native Win32 UI, since removed) field-for-field, including the
    // Force-Align fill-angle computation: when the
    // LightingForceFillAlignment flag is ON the fill azimuths
    // are derived from the sun (sun.z + 120° / + 210°, both at
    // -10° tilt); when OFF the persisted free-edit angles feed
    // the engine directly. Floats are REG_BINARY (readF),
    // colours + the flag are REG_DWORD. Same !useTestHost gate
    // as the rest of this block (the engine snapshot the
    // dialog-lighting a11y golden seeds from must show ctor
    // defaults under --test-host). Intensity is folded into the
    // diffuse/specular channels exactly as the legacy `MakeLight`
    // (native Win32 UI, since removed) did; fills pass specular=black.
    const LightingValues light = ReadLightingSettings(hKey);
    s.sunIntensity = light.sunIntensity;
    s.sunZ = light.sunZ;
    s.sunTilt = light.sunTilt;
    s.sunAmbient = light.sunAmbient;
    s.sunSpecular = light.sunSpecular;
    s.sunDiffuse = light.sunDiffuse;
    s.sunShadow = light.sunShadow;
    s.forceAlign = light.forceAlign;
    s.fill1Intensity = light.fill1Intensity;
    s.fill1Zp = light.fill1Z;
    s.fill1Tiltp = light.fill1Tilt;
    s.fill1Diffuse = light.fill1Diffuse;
    s.fill2Intensity = light.fill2Intensity;
    s.fill2Zp = light.fill2Z;
    s.fill2Tiltp = light.fill2Tilt;
    s.fill2Diffuse = light.fill2Diffuse;

    return s;
}

LightingValues ReadLightingSettings(HKEY hKey)
{
    LightingValues v;
    ReadRegFloat(hKey, kLightSunIntensity, v.sunIntensity);
    ReadRegFloat(hKey, kLightSunZAngle, v.sunZ);
    ReadRegFloat(hKey, kLightSunTilt, v.sunTilt);
    ReadRegDword(hKey, kLightSunAmbientColor, v.sunAmbient);
    ReadRegDword(hKey, kLightSunSpecularColor, v.sunSpecular);
    ReadRegDword(hKey, kLightSunDiffuseColor, v.sunDiffuse);
    ReadRegDword(hKey, kLightSunShadowColor, v.sunShadow);
    DWORD forceAlign = v.forceAlign ? 1u : 0u;
    if (ReadRegDword(hKey, kLightForceFillAlignment, forceAlign))
        v.forceAlign = (forceAlign != 0);
    ReadRegFloat(hKey, kLightFill1Intensity, v.fill1Intensity);
    ReadRegFloat(hKey, kLightFill1ZAngle, v.fill1Z);
    ReadRegFloat(hKey, kLightFill1Tilt, v.fill1Tilt);
    ReadRegDword(hKey, kLightFill1DiffuseColor, v.fill1Diffuse);
    ReadRegFloat(hKey, kLightFill2Intensity, v.fill2Intensity);
    ReadRegFloat(hKey, kLightFill2ZAngle, v.fill2Z);
    ReadRegFloat(hKey, kLightFill2Tilt, v.fill2Tilt);
    ReadRegDword(hKey, kLightFill2DiffuseColor, v.fill2Diffuse);
    return v;
}

void WriteLightingSettings(HKEY hKey, const LightingValues& v)
{
    // Floats persist as REG_BINARY, colours + the flag as REG_DWORD, matching
    // the reads above.
    auto writeF = [&](const wchar_t* name, float f) {
        RegSetValueExW(hKey, name, 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&f), sizeof(f));
    };
    auto writeDw = [&](const wchar_t* name, DWORD dw) {
        RegSetValueExW(hKey, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&dw), sizeof(dw));
    };
    writeF(kLightSunIntensity,      v.sunIntensity);
    writeF(kLightSunZAngle,         v.sunZ);
    writeF(kLightSunTilt,           v.sunTilt);
    writeDw(kLightSunDiffuseColor,  v.sunDiffuse);
    writeDw(kLightSunSpecularColor, v.sunSpecular);
    writeDw(kLightSunAmbientColor,  v.sunAmbient);
    writeDw(kLightSunShadowColor,   v.sunShadow);

    writeF(kLightFill1Intensity,     v.fill1Intensity);
    writeF(kLightFill1ZAngle,        v.fill1Z);
    writeF(kLightFill1Tilt,          v.fill1Tilt);
    writeDw(kLightFill1DiffuseColor, v.fill1Diffuse);

    writeF(kLightFill2Intensity,     v.fill2Intensity);
    writeF(kLightFill2ZAngle,        v.fill2Z);
    writeF(kLightFill2Tilt,          v.fill2Tilt);
    writeDw(kLightFill2DiffuseColor, v.fill2Diffuse);

    writeDw(kLightForceFillAlignment, v.forceAlign ? 1u : 0u);
}

} // namespace host
