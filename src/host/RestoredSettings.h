#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace host {

struct RestoredSettings {
    // Bloom
    std::optional<bool>  bloomEnabled;
    std::optional<float> bloomStrength, bloomCutoff, bloomSize;
    // View
    std::optional<COLORREF> backgroundColor;
    std::optional<bool>     showGround;
    // Ground — apply custom paths BEFORE the index (ordering is load-bearing).
    // Only non-empty slots, in ascending slot order.
    std::vector<std::pair<int, std::wstring>> groundSlotPaths;
    std::optional<COLORREF> groundSolidColor;
    std::optional<int>      groundTexture;        // already range-validated
    // Skydome — custom paths applied UNCONDITIONALLY per slot (incl. empties, to
    // clear), in ascending slot order, before the index.
    std::vector<std::pair<int, std::wstring>> skydomeCustomPaths;
    std::optional<int>      skydomeSlot;          // already range-validated
    // Skydome game-dome environment
    bool         hasSkydomeEnv = false;
    DWORD        skydomeContextRaw = 1;           // 0 = Land, 1 = Space (default)
    std::wstring skydomePrimaryName, skydomeSecondaryName;
    // Reference object + grid
    std::optional<std::array<float, 6>> refTransform;
    std::optional<bool>  refVisible, gridVisible, snapEnabled, refLocked;
    std::optional<float> gridSpacing;
    std::optional<std::wstring> refName;          // nullopt in capture mode
    // Lighting — always populated (defaults from LightingSettings.h). Raw
    // persisted fill angles; force-align is resolved in the apply phase.
    float    sunIntensity, sunZ, sunTilt;
    COLORREF sunAmbient, sunSpecular, sunDiffuse, sunShadow;
    bool     forceAlign;
    float    fill1Intensity, fill1Zp, fill1Tiltp;
    COLORREF fill1Diffuse;
    float    fill2Intensity, fill2Zp, fill2Tiltp;
    COLORREF fill2Diffuse;
};

// Reads the persisted settings key into a plain struct. hKey may be nullptr
// (first run) — every read then reports absent and lighting takes its defaults.
// inCaptureMode==true skips the ReferenceObjectName read (capture supplies its
// own object; see the apply-phase comment).
RestoredSettings ReadRestoredSettings(HKEY hKey, bool inCaptureMode);

} // namespace host
