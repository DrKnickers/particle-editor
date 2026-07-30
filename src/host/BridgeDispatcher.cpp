#include "BridgeDispatcher.h"

#include "AcceleratorBridge.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "InputDispatcher.h"
#include "LayoutBroker.h"
#include "WindowCapture.h"
#include "HostMessages.h"   // WM_APP_QUIT_CONFIRMED
#include "StringConv.h"   // host::Utf8ToWide / WideToUtf8 (consolidated)
#include "PerfTrace.h"
#include "third_party/nlohmann/json.hpp"

#include "../engine.h"
#include "../files.h"
#include "../ChunkFile.h"
#include "../LinkGroup.h"
#include "../ModManager.h"
#include "../ParticleSystem.h"
#include "../UI/TexturePalette.h"
#include "../ParticleSystemIO.h"
#include "../Rescale.h"
#include "../RefTransformUndoKey.h"
#include "../ResourceLimits.h"   // kMaxEmitterTreeDepth (BuildEmitterTreeNode backstop)
#include "../SpawnerDriver.h"
#include "../UndoStack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// DWM immersive dark-mode caption attribute. Defined in dwmapi.h on
// Windows SDK 10.0.18985+, but guard it so older SDKs still compile.
// Value 20 is the post-Win10-2004 attribute; the editor targets modern
// Windows (WebView2 + DComp), so the older 19 fallback isn't needed.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using nlohmann::json;

// parse a CSS colour string from getComputedStyle
// ("#rrggbb", "#rgb", or "rgb(r, g, b)" / "rgba(r, g, b, a)") into a
// COLORREF for the host/backing-color handler. Returns true on success.
// Defensive — unknown formats return false so the handler answers
// ok:false rather than guessing a colour.
bool ParseCssColorToColorRef(const std::string& in, COLORREF& out)
{
    // Trim ASCII whitespace (no locale, no <cctype> surprises).
    size_t a = 0, b = in.size();
    while (a < b && static_cast<unsigned char>(in[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(in[b - 1]) <= ' ') --b;
    const std::string s = in.substr(a, b - a);
    if (s.empty()) return false;

    int r = -1, g = -1, bl = -1;
    if (s[0] == '#')
    {
        std::string hex = s.substr(1);
        if (hex.size() == 3)  // short form #rgb → #rrggbb
            hex = std::string{ hex[0], hex[0], hex[1], hex[1], hex[2], hex[2] };
        if (hex.size() != 6 ||
            hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return false;
        r  = static_cast<int>(strtol(hex.substr(0, 2).c_str(), nullptr, 16));
        g  = static_cast<int>(strtol(hex.substr(2, 2).c_str(), nullptr, 16));
        bl = static_cast<int>(strtol(hex.substr(4, 2).c_str(), nullptr, 16));
    }
    else if (s.rfind("rgb", 0) == 0)
    {
        const size_t lp = s.find('(');
        if (lp == std::string::npos) return false;
        // " %d , %d , %d" tolerates "r,g,b" and "r , g , b"; a trailing
        // alpha (rgba) is simply ignored.
        if (sscanf(s.c_str() + lp + 1, " %d , %d , %d", &r, &g, &bl) != 3)
            return false;
    }
    else
    {
        return false;
    }

    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    out = RGB(clamp(r), clamp(g), clamp(bl));
    return true;
}

// Defined in src/main.cpp (relocated there when the legacy EmitterList.cpp
// was removed); reused by the host's emitter-mutation handlers.
// Declared extern here so the dispatcher can link against it without a
// header dependency on main.cpp.
extern std::string GenerateDuplicateName(const ParticleSystem* system,
                                          const std::string&     sourceName);

namespace host {

// The helpers below (through BuildEngineStateSnapshot) were an anonymous
// namespace; now external-linkage with declarations in BridgeDispatchShared.h
// so the per-domain dispatch TUs (BridgeDispatch_*.cpp) can call them.
// Definitions stay HERE — one definition each, never per-TU copies.

// Build a `res` envelope with ok:true and given data payload.
std::string BuildOkResponse(const std::string& id, const json& data)
{
    json env = {
        {"type", "res"},
        {"id",   id},
        {"ok",   true},
        {"data", data},
    };
    return env.dump();
}

// Build a `res` envelope with ok:false and given error string.
std::string BuildErrResponse(const std::string& id, const std::string& error)
{
    json env = {
        {"type",  "res"},
        {"id",    id},
        {"ok",    false},
        {"error", error},
    };
    return env.dump();
}

// Serialise a D3DXVECTOR3/4 / Engine::Camera / Engine::Light into the
// EngineStateDto-compatible JSON shape.
json Vec3ToJson(const D3DXVECTOR3& v)
{
    return json::array({v.x, v.y, v.z});
}

json Vec4ToJson(const D3DXVECTOR4& v)
{
    return json::array({v.x, v.y, v.z, v.w});
}

json CameraToJson(const Engine::Camera& c)
{
    return json{
        {"position", Vec3ToJson(c.Position)},
        {"target",   Vec3ToJson(c.Target)},
        {"up",       Vec3ToJson(c.Up)},
    };
}

json LightToJson(const Engine::Light& l)
{
    return json{
        {"diffuse",   Vec4ToJson(l.Diffuse)},
        {"specular",  Vec4ToJson(l.Specular)},
        {"position",  Vec4ToJson(l.Position)},
        {"direction", Vec4ToJson(l.Direction)},
    };
}

// Parse a JSON array of 3 numbers into a D3DXVECTOR3. Defaults to zero
// on malformed input (better than crashing on a stray non-array param).
D3DXVECTOR3 JsonToVec3(const json& j)
{
    if (j.is_array() && j.size() >= 3)
        return D3DXVECTOR3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    return D3DXVECTOR3(0, 0, 0);
}

D3DXVECTOR4 JsonToVec4(const json& j)
{
    if (j.is_array() && j.size() >= 4)
        return D3DXVECTOR4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
    return D3DXVECTOR4(0, 0, 0, 0);
}

Engine::LightType ParseLightWhich(const std::string& s)
{
    if (s == "fill1") return Engine::LT_FILL1;
    if (s == "fill2") return Engine::LT_FILL2;
    return Engine::LT_SUN;  // default / "sun"
}

// Persist the skydome selection to HKCU\Software\AloParticleEditor
// using the SAME value names/types as the legacy `WriteSkydomeIndex` /
// `WriteSkydomeCustomPath` (legacy Win32 UI, since removed), so a dome chosen in the new
// UI survives restart AND round-trips with the legacy picker. The new-UI
// skydome handlers previously only marked the doc dirty (no registry write),
// so a daily-driver selection was silently lost on restart — these close that
// gap. Callers gate the write on m_testHost/m_settingsLive (mirroring
// settings/lighting-force-align/set) so the a11y harness never mutates the
// dev box registry.
void PersistSkydomeIndex(int value)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = static_cast<DWORD>(value);
        RegSetValueExW(hKey, L"SkydomeIndex", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

void PersistSkydomeCustomPath(int slot, const std::wstring& path)
{
    if (slot < Engine::kSkydomeFirstCustomSlot || slot >= Engine::kSkydomeSlotCount)
        return;
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) == ERROR_SUCCESS)
    {
        wchar_t name[64];
        swprintf_s(name, L"SkydomeCustomSlot%d", slot);
        if (path.empty())
            RegDeleteValueW(hKey, name);   // mirror legacy: clearing a slot deletes the value
        else
            RegSetValueExW(hKey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(path.c_str()),
                           static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// Persist the game-dome environment selection (context + the two chosen
// GameObject Names) under the same hive. New REG keys (legacy has no equivalent);
// the new-UI startup restore reads them back. Empty name => delete the value.
void PersistSkydomeEnvironment(int context, const std::wstring& primaryName,
                                      const std::wstring& secondaryName)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD ctx = static_cast<DWORD>(context);
    RegSetValueExW(hKey, L"SkydomeContext", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&ctx), sizeof(ctx));
    const struct { const wchar_t* name; const std::wstring& val; } kv[] = {
        { L"SkydomePrimaryName",   primaryName   },
        { L"SkydomeSecondaryName", secondaryName },
    };
    for (const auto& e : kv)
    {
        if (e.val.empty())
            RegDeleteValueW(hKey, e.name);
        else
            RegSetValueExW(hKey, e.name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(e.val.c_str()),
                           static_cast<DWORD>((e.val.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(hKey);
}

// Persist the solid-colour background (same BackgroundColor REG_DWORD as
// legacy WriteBackgroundColor). The solid-colour option lives
// in the same Background picker as the skydome and had the identical new-UI gap.
void PersistBackgroundColor(COLORREF color)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = static_cast<DWORD>(color);
        RegSetValueExW(hKey, L"BackgroundColor", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

// Persist the ground-plane visibility (ShowGround REG_DWORD 0/1) under the same
// hive. Ground is a GLOBAL VIEW preference — restored on launch by HostWindow's
// [view-restore] path and honored headless by CaptureRunner; it is NOT part of
// the .alo document. The new-UI ground handler previously only called SetGround
// (no registry write), so a toggled-off ground silently reverted to the ctor
// default (on) every restart (issue #617). Callers gate on
// m_testHost/m_settingsLive like the sibling settings so the a11y harness never
// mutates the dev-box registry.
void PersistShowGround(bool enabled)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = enabled ? 1u : 0u;
        RegSetValueExW(hKey, L"ShowGround", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

// Persist the imported reference object (selected Name) + its visibility +
// transform, plus the unit-grid toggle/spacing, under the same hive. New REG
// keys; the new-UI startup restore reads them back. Empty Name => delete.
// Transform + grid spacing are REG_BINARY floats (6 and 1 respectively).
void PersistReferenceObjectName(const std::wstring& name)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    if (name.empty())
        RegDeleteValueW(hKey, L"ReferenceObjectName");
    else
        RegSetValueExW(hKey, L"ReferenceObjectName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(name.c_str()),
                       static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

void PersistReferenceObjectVisible(bool visible)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = visible ? 1u : 0u;
        RegSetValueExW(hKey, L"ReferenceObjectVisible", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

void PersistReferenceObjectLock(bool locked)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = locked ? 1u : 0u;
        RegSetValueExW(hKey, L"ReferenceObjectLocked", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(hKey);
    }
}

void PersistReferenceObjectTransform(const D3DXVECTOR3& pos, const D3DXVECTOR3& rot)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        const float xform[6] = { pos.x, pos.y, pos.z, rot.x, rot.y, rot.z };
        RegSetValueExW(hKey, L"ReferenceObjectTransform", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(xform), sizeof(xform));
        RegCloseKey(hKey);
    }
}

void PersistGrid(bool visible, float spacing)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD vis = visible ? 1u : 0u;
        RegSetValueExW(hKey, L"GridVisible", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&vis), sizeof(vis));
        RegSetValueExW(hKey, L"GridSpacing", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&spacing), sizeof(spacing));
        RegCloseKey(hKey);
    }
}

// Persist the gizmo snap toggle (single REG_DWORD, mirrors PersistGrid's
// GridVisible write). The drag-time apply that reads it is a separate task.
void PersistSnap(bool enabled)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        DWORD en = enabled ? 1u : 0u;
        RegSetValueExW(hKey, L"SnapEnabled", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&en), sizeof(en));
        RegCloseKey(hKey);
    }
}

// ReferenceObjectStatus -> wire string for the snapshot DTO.
const char* RefStatusToString(ReferenceObjectStatus s)
{
    switch (s)
    {
        case ReferenceObjectStatus::Ok:           return "ok";
        case ReferenceObjectStatus::Skinned:      return "skinned";
        case ReferenceObjectStatus::LoadFailed:   return "load-failed";
        case ReferenceObjectStatus::ModelMissing: return "model-missing";
        default:                                  return "none";
    }
}

// SkydomeSlotStatus -> wire string for the snapshot DTO. Mirrors
// RefStatusToString so the picker can flag a chosen-but-unloadable dome.
const char* SkyStatusToString(SkydomeSlotStatus s)
{
    switch (s)
    {
        case SkydomeSlotStatus::Ok:         return "ok";
        case SkydomeSlotStatus::LoadFailed: return "load-failed";
        default:                            return "none";
    }
}

// host-state plumbing — JSON ↔ SpawnerConfig converters. The
// schema's SpawnerParamsDto (web/packages/bridge-schema/src/index.ts:60)
// is value-for-value compatible with the native SpawnerConfig (lines in
// src/SpawnerDriver.h:18) except for the `mode` field which is a
// string in JSON but an enum on the native side.

SpawnerConfig JsonToSpawnerConfig(const json& j)
{
    SpawnerConfig cfg;
    if (!j.is_object()) return cfg;

    std::string mode = j.value("mode", std::string("auto"));
    cfg.mode = (mode == "manual")
        ? SpawnerConfig::Mode::Manual
        : SpawnerConfig::Mode::Auto;

    cfg.enabled        = j.value("enabled", false);
    cfg.burstSize      = j.value("burstSize", 1);
    cfg.spacingSec     = j.value("spacingSec", 0.0f);
    cfg.intervalSec    = j.value("intervalSec", 10.0f);
    cfg.position       = JsonToVec3(j.value("position", json::array()));
    cfg.velocity       = JsonToVec3(j.value("velocity", json::array()));
    cfg.maxLifetimeSec = j.value("maxLifetimeSec", 5.0f);
    cfg.jitterPosition    = JsonToVec3(j.value("jitterPosition", json::array()));
    cfg.acceleration      = JsonToVec3(j.value("acceleration", json::array()));
    cfg.squiggleAmplitude = JsonToVec3(j.value("squiggleAmplitude", json::array()));
    cfg.squiggleFrequency = j.value("squiggleFrequency", 1.0f);
    return cfg;
}

json SpawnerConfigToJson(const SpawnerConfig& cfg)
{
    return json{
        {"mode",           cfg.mode == SpawnerConfig::Mode::Manual ? "manual" : "auto"},
        {"enabled",        cfg.enabled},
        {"burstSize",      cfg.burstSize},
        {"spacingSec",     cfg.spacingSec},
        {"intervalSec",    cfg.intervalSec},
        {"position",       Vec3ToJson(cfg.position)},
        {"velocity",       Vec3ToJson(cfg.velocity)},
        {"maxLifetimeSec", cfg.maxLifetimeSec},
        {"jitterPosition",    Vec3ToJson(cfg.jitterPosition)},
        {"acceleration",      Vec3ToJson(cfg.acceleration)},
        {"squiggleAmplitude", Vec3ToJson(cfg.squiggleAmplitude)},
        {"squiggleFrequency", cfg.squiggleFrequency},
    };
}

// Wire-name ↔ LinkExemptFlags::member mapping.
// Mirrors the legacy field table at [src/UI/EmitterList.cpp:2381]
// (kLinkSettingsFields), but uses camelCase field names that match the
// schema's wire surface. Excludes `name` (intrinsically exempt;
// settings dialog doesn't display it) and the `unknownXX` set (no
// inspector representation). The dispatcher converts the wire's
// `string[]` of exempt field names into a LinkExemptFlags bitfield by
// looking each name up in this table.
struct LinkFieldEntry
{
    const char*               name;
    bool LinkExemptFlags::*   flag;
};

static const LinkFieldEntry kLinkFieldTable[] = {
    // Textures.
    { "colorTexture",            &LinkExemptFlags::colorTexture },
    { "normalTexture",           &LinkExemptFlags::normalTexture },
    // Curves.
    { "trackIndex",              &LinkExemptFlags::trackIndex },
    { "trackRed",                &LinkExemptFlags::trackRed },
    { "trackGreen",              &LinkExemptFlags::trackGreen },
    { "trackBlue",               &LinkExemptFlags::trackBlue },
    { "trackAlpha",              &LinkExemptFlags::trackAlpha },
    { "trackScale",              &LinkExemptFlags::trackScale },
    { "trackRotationSpeed",      &LinkExemptFlags::trackRotationSpeed },
    // Lifetime / spawning.
    { "lifetime",                &LinkExemptFlags::lifetime },
    { "initialDelay",            &LinkExemptFlags::initialDelay },
    { "burstDelay",              &LinkExemptFlags::burstDelay },
    { "nBursts",                 &LinkExemptFlags::nBursts },
    { "nParticlesPerBurst",      &LinkExemptFlags::nParticlesPerBurst },
    { "nParticlesPerSecond",     &LinkExemptFlags::nParticlesPerSecond },
    { "useBursts",               &LinkExemptFlags::useBursts },
    // Physics.
    { "gravity",                 &LinkExemptFlags::gravity },
    { "acceleration",            &LinkExemptFlags::acceleration },
    { "inwardSpeed",             &LinkExemptFlags::inwardSpeed },
    { "inwardAcceleration",      &LinkExemptFlags::inwardAcceleration },
    { "bounciness",              &LinkExemptFlags::bounciness },
    { "groundBehavior",          &LinkExemptFlags::groundBehavior },
    { "objectSpaceAcceleration", &LinkExemptFlags::objectSpaceAcceleration },
    { "affectedByWind",          &LinkExemptFlags::affectedByWind },
    // Appearance.
    { "blendMode",               &LinkExemptFlags::blendMode },
    { "textureSize",             &LinkExemptFlags::textureSize },
    { "nTriangles",              &LinkExemptFlags::nTriangles },
    { "randomScalePerc",         &LinkExemptFlags::randomScalePerc },
    { "randomLifetimePerc",      &LinkExemptFlags::randomLifetimePerc },
    { "hasTail",                 &LinkExemptFlags::hasTail },
    { "tailSize",                &LinkExemptFlags::tailSize },
    { "noDepthTest",             &LinkExemptFlags::noDepthTest },
    { "randomColors",            &LinkExemptFlags::randomColors },
    // Weather.
    { "isWeatherParticle",       &LinkExemptFlags::isWeatherParticle },
    { "weatherCubeSize",         &LinkExemptFlags::weatherCubeSize },
    { "weatherCubeDistance",     &LinkExemptFlags::weatherCubeDistance },
    { "weatherFadeoutDistance",  &LinkExemptFlags::weatherFadeoutDistance },
    // Rotation.
    { "randomRotation",          &LinkExemptFlags::randomRotation },
    { "randomRotationDirection", &LinkExemptFlags::randomRotationDirection },
    { "randomRotationAverage",   &LinkExemptFlags::randomRotationAverage },
    { "randomRotationVariance",  &LinkExemptFlags::randomRotationVariance },
    // Misc.
    { "linkToSystem",            &LinkExemptFlags::linkToSystem },
    { "parentLinkStrength",      &LinkExemptFlags::parentLinkStrength },
    { "doColorAddGrayscale",     &LinkExemptFlags::doColorAddGrayscale },
    { "isHeatParticle",          &LinkExemptFlags::isHeatParticle },
    { "isWorldOriented",         &LinkExemptFlags::isWorldOriented },
    { "freezeTime",              &LinkExemptFlags::freezeTime },
    { "skipTime",                &LinkExemptFlags::skipTime },
    { "emitFromMesh",            &LinkExemptFlags::emitFromMesh },
    { "emitFromMeshOffset",      &LinkExemptFlags::emitFromMeshOffset },
    { "groupSpeed",              &LinkExemptFlags::groupSpeed },
    { "groupLifetime",           &LinkExemptFlags::groupLifetime },
    { "groupPosition",           &LinkExemptFlags::groupPosition },
};

constexpr size_t kLinkFieldCount =
    sizeof(kLinkFieldTable) / sizeof(kLinkFieldTable[0]);

// Translate a LinkExemptFlags bitfield to the wire's `string[]` of
// exempt field names.
json LinkExemptFlagsToJsonArray(const LinkExemptFlags& flags)
{
    json arr = json::array();
    for (size_t i = 0; i < kLinkFieldCount; ++i)
    {
        if (flags.*(kLinkFieldTable[i].flag))
            arr.push_back(kLinkFieldTable[i].name);
    }
    return arr;
}

// Translate a `string[]` wire payload to a LinkExemptFlags bitfield.
// Unknown names are silently ignored (forward-compat with newer
// schemas adding fields the host doesn't yet recognise).
LinkExemptFlags LinkExemptFlagsFromJsonArray(const json& arr)
{
    LinkExemptFlags out;  // default-constructed = v1 defaults
    // Clear the table-mapped fields; we want to honor only what the
    // wire specifies. `name` + unknowns are left at their defaults
    // (intrinsic per-emitter for name; defaults for the unknowns).
    for (size_t i = 0; i < kLinkFieldCount; ++i)
        out.*(kLinkFieldTable[i].flag) = false;

    if (!arr.is_array()) return out;
    for (const auto& v : arr)
    {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        for (size_t i = 0; i < kLinkFieldCount; ++i)
        {
            if (s == kLinkFieldTable[i].name)
            {
                out.*(kLinkFieldTable[i].flag) = true;
                break;
            }
        }
    }
    return out;
}

// LNK settings surface: build a LinkExemptFlags "diff mask" that marks every
// field exempt EXCEPT those transitioning exempt(old)→shared(proposed).
// DiffNonExemptParams / copySharedParamsFrom then act on exactly the
// newly-shared fields — the precise set the legacy settings-OK warned about
// and resolved (EmitterList.cpp:2841). `name` is forced exempt (never shared).
LinkExemptFlags MakeNewlySharedMask(const LinkExemptFlags& oldFlags,
                                    const LinkExemptFlags& proposed)
{
    LinkExemptFlags mask;
    for (size_t k = 0; k < kLinkFieldCount; ++k)
    {
        bool LinkExemptFlags::* f = kLinkFieldTable[k].flag;
        mask.*f = !((oldFlags.*f) && !(proposed.*f)); // false only if newly shared
    }
    mask.name = true;
    return mask;
}

// walk a ParticleSystem and build an EmitterTreeNode-shaped JSON
// tree. Mirrors the schema definition at
// web/packages/bridge-schema/src/index.ts:91. Children are computed
// from each emitter's `spawnDuringLife` / `spawnOnDeath` indices in
// the same order as legacy `ImportEmitters_AddTreeItem` (during-life
// before on-death) so the import dialog tree matches.
//
// extended with `role` / `linkGroup` / `visible`.
// `role` is derived from how this emitter is attached to its parent's
// spawn slot (lifetime vs death); top-level emitters return "root".
// The sentinel for "no spawn child" is `(size_t)-1` — matches the
// legacy EmitterList.cpp usage at e.g. [src/UI/EmitterList.cpp:1349].
json BuildEmitterTreeNode(const ParticleSystem* sys, size_t idx, size_t depth)
{
    if (sys == nullptr || idx >= sys->getEmitters().size()) return json::object();
    const ParticleSystem::Emitter& emit = sys->getEmitter(idx);
    json children = json::array();
    // Recursion backstop (2026-07 audit, an-audit-finding). ValidateEmitterGraph caps chain
    // depth on load and import, but it is NOT called from any bridge mutation
    // path, so this walk cannot assume the cap holds. Emit the node without its
    // children rather than descending -- a truncated subtree is a visible,
    // recoverable UI defect; a blown stack takes the process with it.
    if (depth >= kMaxEmitterTreeDepth)
    {
        printf("[Bridge] emitter tree exceeds depth cap %lu at emitter %zu; "
               "truncating subtree\n", kMaxEmitterTreeDepth, idx); fflush(stdout);
    }
    else
    {
        if (emit.spawnDuringLife != static_cast<size_t>(-1))
            children.push_back(BuildEmitterTreeNode(sys, emit.spawnDuringLife, depth + 1));
        if (emit.spawnOnDeath != static_cast<size_t>(-1))
            children.push_back(BuildEmitterTreeNode(sys, emit.spawnOnDeath, depth + 1));
    }

    // Role: walk to parent's spawn slots. If parent is null we're a
    // top-level root. Otherwise check whether parent's lifetime slot
    // or death slot points at our index. Default to "lifetime" if
    // somehow neither matches (shouldn't happen — every non-root must
    // be referenced by exactly one slot — but treats the case as the
    // less-disruptive fallback).
    const char* role = "root";
    if (emit.parent != nullptr)
    {
        if (emit.parent->spawnOnDeath == idx)         role = "death";
        else if (emit.parent->spawnDuringLife == idx) role = "lifetime";
        else                                          role = "lifetime";
    }

    return json{
        {"id",        static_cast<int>(idx)},
        {"stableId",  emit.stableId},
        {"name",      emit.name},
        {"role",      role},
        {"linkGroup", static_cast<unsigned int>(emit.linkGroup)},
        {"visible",   emit.visible},
        // Chain-load warning: spawn-quantity params mirrored onto
        // every tree node so the React side can estimate per-chain alive
        // counts without N get-properties round-trips. Field names match
        // EmitterPropertiesDto / SpawnParamsDto in bridge-schema.
        {"spawn", json{
            {"lifetime",            emit.lifetime},
            {"useBursts",           emit.useBursts},
            {"nBursts",             static_cast<unsigned int>(emit.nBursts)},
            {"burstDelay",          emit.burstDelay},
            {"nParticlesPerSecond", static_cast<unsigned int>(emit.nParticlesPerSecond)},
            {"nParticlesPerBurst",  static_cast<unsigned int>(emit.nParticlesPerBurst)},
        }},
        {"children",  children},
    };
}

// Default spawner-config JSON. Mirrors the
// `SpawnerConfig()` initialiser at [src/SpawnerDriver.h:18]. Used to
// seed the dispatcher's cached config on construction so the first
// snapshot returns a populated `spawner` field even before any
// `spawner/start` has been dispatched.
json DefaultSpawnerConfigJson()
{
    return json{
        {"mode",            "auto"},
        {"enabled",         false},
        {"burstSize",       1},
        {"spacingSec",      0.0},
        {"intervalSec",     10.0},
        {"position",        json::array({0.0, 0.0, 0.0})},
        {"velocity",        json::array({0.0, 0.0, 0.0})},
        {"maxLifetimeSec",  5.0},
        {"jitterPosition",    json::array({0.0, 0.0, 0.0})},
        {"acceleration",      json::array({0.0, 0.0, 0.0})},
        {"squiggleAmplitude", json::array({0.0, 0.0, 0.0})},
        {"squiggleFrequency", 1.0},
    };
}

// Forward declaration so BuildEngineStateSnapshot can read editor-level
// state from the dispatcher when serialising the snapshot.

// Reads every getter on Engine into a JSON object whose shape matches
// `EngineStateDto` in web/packages/bridge-schema/src/index.ts.
//
// Coupling note: any new field added to `EngineStateDto` MUST also be
// added here, otherwise the React UI will read `undefined` for it.
json BuildEngineStateSnapshot(Engine* engine,
                              const std::wstring& currentFilePath,
                              bool dirty,
                              const json& spawnerConfig,
                              int selectedEmitterId,
                              const std::wstring& activeModPath,
                              bool leaveParticles,
                              bool canUndo,
                              bool canRedo)
{
    if (!engine) return json::object();

    // Ground slot custom paths — kGroundTextureCount entries.
    json groundPaths = json::array();
    for (int i = 0; i < Engine::kGroundTextureCount; ++i)
        groundPaths.push_back(WideToUtf8(engine->GetGroundSlotCustomPath(i)));

    // Ground slot availability — kGroundTextureCount entries. True = the slot
    // can render right now: Dirt/Solid Color always; Grass/Sand/Snow only when
    // the user's EaW/FoC install resolves their texture; Custom when a path is
    // set. The picker greys out slots whose entry is false.
    json groundAvail = json::array();
    for (int i = 0; i < Engine::kGroundTextureCount; ++i)
        groundAvail.push_back(engine->IsGroundSlotAvailable(i));

    // Skydome custom paths — only slots 9..11 are user-customisable.
    // The DTO exposes them as a flat array indexed 0..2.
    json skyPaths = json::array();
    for (int i = Engine::kSkydomeFirstCustomSlot; i < Engine::kSkydomeSlotCount; ++i)
        skyPaths.push_back(WideToUtf8(engine->GetSkydomeCustomPath(i)));

    json lights = {
        {"sun",   LightToJson(engine->GetLight(Engine::LT_SUN))},
        {"fill1", LightToJson(engine->GetLight(Engine::LT_FILL1))},
        {"fill2", LightToJson(engine->GetLight(Engine::LT_FILL2))},
    };

    // currentFilePath as JSON: null when untitled, string otherwise.
    // JSON null is correct semantically — the schema's
    // `currentFilePath: string | null` discriminates by presence.
    json filePathField = currentFilePath.empty()
        ? json(nullptr)
        : json(WideToUtf8(currentFilePath));

    return json{
        // Editor-level state.
        {"currentFilePath",       filePathField},
        {"dirty",                 dirty},

        // Ground
        {"ground",                engine->GetGround()},
        {"groundZ",               engine->GetGroundZ()},
        {"groundTexture",         engine->GetGroundTexture()},
        {"groundSolidColor",      static_cast<unsigned int>(engine->GetGroundSolidColor())},
        {"groundColor",           static_cast<unsigned int>(engine->GetGroundColor())},
        {"groundSlotCustomPaths", groundPaths},
        {"groundSlotAvailable",   groundAvail},

        // Skydome
        {"skydomeSlot",           engine->GetSkydomeSlot()},
        {"skydomeCustomPaths",    skyPaths},
        // game-dome environment selection
        {"skydomeContext",        engine->GetSkydomeContext() == SkydomeContext::Land ? "land" : "space"},
        {"skydomePrimaryName",    engine->GetSkydomePrimaryName()},
        {"skydomeSecondaryName",  engine->GetSkydomeSecondaryName()},
        // per-slot load outcome so the picker surfaces a chosen-but-
        // unloadable dome instead of silently showing the solid background.
        {"skydomePrimaryStatus",   SkyStatusToString(engine->GetSkydomePrimaryStatus())},
        {"skydomeSecondaryStatus", SkyStatusToString(engine->GetSkydomeSecondaryStatus())},
        // GPU-resource truth, not bookkeeping (2026-07 audit, an-audit-finding). Every field
        // above reports what was SELECTED or what the reader THOUGHT resolved, so
        // deleting the mesh-creation call left them all correct and the viewport
        // black. These two are read straight off the live VB/IB handles, so they
        // cannot be true unless the device really holds the dome — which is what
        // makes that deletion observable to drive-smoke's assert-state.
        {"skydomeMeshGpuReady",      engine->SkydomeMeshHasGpuBuffers()},
        {"skydomePrimaryGpuReady",   engine->SkydomePrimaryHasGpuBuffers()},
        {"skydomeSecondaryGpuReady", engine->SkydomeSecondaryHasGpuBuffers()},

        // imported reference object + unit grid
        {"referenceObjectName",     engine->GetReferenceObjectName()},
        {"referenceObjectVisible",  engine->GetReferenceObjectVisible()},
        {"referenceObjectLocked",   engine->IsReferenceLocked()},
        {"referenceObjectPosition", Vec3ToJson(engine->GetReferencePosition())},
        {"referenceObjectRotation", Vec3ToJson(engine->GetReferenceRotation())},
        {"referenceObjectStatus",   RefStatusToString(engine->GetReferenceObjectStatus())},
        // True while the units/structures catalog is (re)building off the UI thread;
        // the picker shows "Loading objects…" and re-queries its list on the true->false
        // transition (catalog ready / mod-switch rebuild) -- not on every state change.
        {"referenceCatalogBuilding", engine->IsReferenceCatalogBuilding()},
        {"gridVisible",             engine->GetGridVisible()},
        {"gridSpacing",             engine->GetGridSpacing()},
        {"snapEnabled",             engine->GetSnapEnabled()},
        // Background (COLORREF; low byte = blue)
        {"background",            static_cast<unsigned int>(engine->GetBackground())},

        // Lights / ambient / shadow
        {"lights",                lights},
        {"ambient",               Vec4ToJson(engine->GetAmbient())},
        {"shadow",                Vec4ToJson(engine->GetShadow())},

        // Bloom
        {"bloom",                 engine->GetBloom()},
        {"bloomAvailable",        engine->IsBloomAvailable()},
        {"bloomStrength",         engine->GetBloomStrength()},
        {"bloomCutoff",           engine->GetBloomCutoff()},
        {"bloomSize",             engine->GetBloomSize()},

        // Task 2.7 — leave particles after instance death. Read from
        // the active ParticleSystem (passed in by caller); defaults true
        // when no system is bound.
        {"leaveParticles",        leaveParticles},

        // Debug
        {"heatDebug",             engine->GetHeatDebug()},

        // View state (preview clock). IsPreviewPaused() is a free
        // function declared in engine.h — not a method on Engine.
        {"paused",                IsPreviewPaused()},

        // Camera
        {"camera",                CameraToJson(engine->GetCamera())},

        // Wind / gravity — read-only via DTO for now (no setter binding).
        {"wind",                  Vec3ToJson(engine->GetWind())},
        {"gravity",               Vec3ToJson(engine->GetGravity())},

        // Spawner — cached on the dispatcher.
        // Host doesn't yet own a SpawnerDriver*; the cache is what
        // spawner/start writes into and what subsequent snapshots read
        // back. Empty object when never set (shouldn't happen because
        // the constructor seeds it from DefaultSpawnerConfigJson()).
        {"spawner",               spawnerConfig.is_null() || spawnerConfig.empty()
                                      ? DefaultSpawnerConfigJson()
                                      : spawnerConfig},

        // Selected emitter (editor state). Serialise
        // -1 as JSON null so the schema's `number | null` discriminator
        // works without a sentinel-aware client.
        {"selectedEmitterId",     selectedEmitterId < 0
                                      ? json(nullptr)
                                      : json(selectedEmitterId)},

        // active mod path. Empty wstring serialises as JSON
        // null so the React side treats "Unmodded" and "no mod state
        // yet" the same way. ModManager owns the canonical state;
        // BridgeDispatcher reads through and serialises here.
        {"activeModPath",         activeModPath.empty()
                                      ? json(nullptr)
                                      : json(WideToUtf8(activeModPath))},

        // Undo/redo availability — drives the Edit menu enable-state.
        // `canRedo` mirrors UndoStack::CanRedo. `canUndo` is computed
        // by the caller to account for undo/perform's head-of-history
        // auto-cap: when cursor==depth and depth>=1, the auto-cap
        // makes the subsequent Undo() succeed even though UndoStack's
        // own CanUndo (which requires cursor>=2) would report false.
        // See undo/perform's comment block for the full design.
        {"canUndo",               canUndo},
        {"canRedo",               canRedo},
    };
}

BridgeDispatcher::BridgeDispatcher(Engine* engine, LayoutBroker& layout,
                                    AcceleratorBridge& accel, EmitFn emit,
                                    bool useTestHost, bool ephemeral)
    : m_engine(engine), m_layout(layout), m_accel(accel), m_emit(std::move(emit))
    , m_testHost(useTestHost)
    , m_ephemeral(ephemeral)
{
    // Test seam (ALO_SETTINGS_LIVE=1): lift the --test-host settings gate so
    // a CDP test can exercise the real registry round-trip. The a11y harness
    // never sets this, so its plain --test-host launch stays deterministic.
    {
        wchar_t buf[8] = {};
        DWORD n = GetEnvironmentVariableW(L"ALO_SETTINGS_LIVE", buf, 8);
        m_settingsLive = (n > 0 && n < 8 && buf[0] == L'1');
    }

    // Seed the recent-files list from the registry at construction so
    // the first React-side `file/recent/list` request already has data
    // (avoids the React menu rendering "(none)" momentarily on first
    // mount even when the user has prior history).
    m_recentFiles = ReadRecentFiles();

    // Seed the spawner-config cache from the
    // shared default JSON so the first snapshot returns the same struct
    // a freshly-constructed `SpawnerConfig()` would. Subsequent
    // spawner/start requests overwrite this in DispatchInternal.
    m_spawnerConfig = DefaultSpawnerConfigJson();
}

// Defensive envelope for any json::exception that escapes
// DispatchInternal. The outer try/catch in Dispatch and DispatchSync wraps
// only json::parse; per-handler `.get<T>()` / `.value(...)` / `is_T()`
// calls inside DispatchInternal can still throw nlohmann::json::type_error
// when callers send malformed payloads. Pre-fix those propagated uncaught
// into the WebView2 callback or COM dispatch path; post-fix this builds
// a well-formed error envelope that the JS side can parse.
static json BuildDispatchExceptionEnvelope(const json& parsed, const char* what)
{
    json res = {
        {"type",  "res"},
        {"ok",    false},
        {"error", std::string("dispatch exception: ") + (what ? what : "(no message)")},
    };
    // Preserve correlation id if the request had one.
    if (auto it = parsed.find("id"); it != parsed.end() && it->is_string())
    {
        res["id"] = it->get<std::string>();
    }
    else
    {
        res["id"] = nullptr;
    }
    return res;
}

static std::string JsonStringField(const json& parsed, const char* name)
{
    if (auto it = parsed.find(name); it != parsed.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

static void EndDispatchSpan(host::perf::Span* span, const json& res)
{
    if (!span) return;
    const bool ok = res.value("ok", false);
    std::string error;
    if (!ok) error = res.value("error", std::string());
    span->End(ok ? "ok" : "error", error);
}

void BridgeDispatcher::SetEngine(Engine* engine)
{
    m_engine = engine;
    // [guard-config] Reapply the cached overload-guard config so a
    // recreated engine never silently reverts to defaults. Today the
    // engine is constructed once per process (HostWindow startup) and
    // this is a no-op safety net; if a future change recreates the
    // engine, the user's setting follows automatically.
    if (m_engine && m_overloadGuardCached)
        m_engine->SetOverloadGuard(m_overloadGuardEnabled, m_overloadGuardMaxParticles);
    // [hard-guard] Reapply the cached estimate the same way, for the same
    // engine-recreation safety net.
    if (m_engine && m_estimatedLoadCached)
        m_engine->SetEstimatedLoad(m_estimatedLoadPerInstance);
}

void BridgeDispatcher::Dispatch(const std::string& jsonRequest)
{
    // [B1] Any coalesced trailing broadcast lands BEFORE this request's
    // response — preserving the bridge-contract ordering (state events
    // never arrive after a response that post-dates them).
    FlushPendingEmits();
    json parsed;
    try
    {
        parsed = json::parse(jsonRequest);
    }
    catch (const json::exception& e)
    {
        // No correlation id — log and drop. React will time out on its
        // side; better than emitting a malformed envelope.
        fprintf(stderr, "[host] BridgeDispatcher: parse error: %s\n", e.what());
        return;
    }

    // We only handle `type: "req"` from React in this dispatcher. Events
    // and responses originating from React aren't part of the contract.
    const auto typeIt = parsed.find("type");
    if (typeIt == parsed.end() || !typeIt->is_string() || typeIt->get<std::string>() != "req")
    {
        return;
    }

    std::unique_ptr<host::perf::Span> span;
    if (host::perf::Enabled()) {
        span = std::make_unique<host::perf::Span>("bridge.dispatch", nlohmann::json{
            {"bridgeMode", "async"},
            {"bridgeKind", JsonStringField(parsed, "kind")},
            {"requestId", JsonStringField(parsed, "id")}
        });
    }

    // Catch json::exception escaping DispatchInternal.
    json res;
    try
    {
        res = DispatchInternal(parsed);
        EndDispatchSpan(span.get(), res);
    }
    catch (const json::exception& e)
    {
        fprintf(stderr, "[host] BridgeDispatcher::Dispatch: type/conversion exception: %s\n", e.what());
        if (span) span->End("error", e.what());
        res = BuildDispatchExceptionEnvelope(parsed, e.what());
    }
    // Drop responses that have no id (malformed request, can't correlate).
    if (m_emit && res.contains("id") && res["id"].is_string())
    {
        m_emit(res.dump());
    }
}

std::string BridgeDispatcher::DispatchSync(const std::string& jsonRequest)
{
    // [B1] See Dispatch — pending evt before the next response.
    FlushPendingEmits();
    json parsed;
    try
    {
        parsed = json::parse(jsonRequest);
    }
    catch (const json::exception& e)
    {
        // Host-object channel — return a well-formed error envelope so
        // the JS side can JSON.parse it without throwing.
        json err = {
            {"type",  "res"},
            {"id",    nullptr},
            {"ok",    false},
            {"error", std::string("parse error: ") + e.what()},
        };
        return err.dump();
    }

    const auto typeIt = parsed.find("type");
    if (typeIt == parsed.end() || !typeIt->is_string() || typeIt->get<std::string>() != "req")
    {
        json err = {
            {"type",  "res"},
            {"id",    nullptr},
            {"ok",    false},
            {"error", "expected type: \"req\""},
        };
        return err.dump();
    }

    std::unique_ptr<host::perf::Span> span;
    if (host::perf::Enabled()) {
        span = std::make_unique<host::perf::Span>("bridge.dispatch", nlohmann::json{
            {"bridgeMode", "sync"},
            {"bridgeKind", JsonStringField(parsed, "kind")},
            {"requestId", JsonStringField(parsed, "id")}
        });
    }

    // Catch json::exception escaping DispatchInternal.
    try
    {
        json res = DispatchInternal(parsed);
        EndDispatchSpan(span.get(), res);
        return res.dump();
    }
    catch (const json::exception& e)
    {
        fprintf(stderr, "[host] BridgeDispatcher::DispatchSync: type/conversion exception: %s\n", e.what());
        if (span) span->End("error", e.what());
        return BuildDispatchExceptionEnvelope(parsed, e.what()).dump();
    }
}

// ---- BridgeRequestContext members needing BridgeDispatcher privates ----
// == the requireEngine lambda (verbatim).
bool BridgeRequestContext::RequireEngine(const char* what)
{
    if (self.m_engine) return true;
    SendErr(std::string("engine not constructed (") + what + ")");
    return false;
}
// == the markDirty lambda (verbatim). SetDirty debounces internally.
void BridgeRequestContext::MarkDirty() { self.SetDirty(true); }

// ---- Promoted DispatchInternal helpers ----
// Formerly lambdas defined mid-ladder inside DispatchInternal (between the
// undo and emitters/get-properties handlers); promoted verbatim so the
// emitter/linkGroups handlers can move to a per-domain TU.

// Lookup helper: find an emitter pointer by integer index. Returns
// nullptr on out-of-range / no-system / null-slot. Used by all
// emitter-mutation handlers.
ParticleSystem::Emitter* BridgeDispatcher::getEmitterById(int id)
{
    if (id < 0) return nullptr;
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return nullptr;
    const auto& emitters = (*m_pParticleSystem)->getEmitters();
    if (static_cast<size_t>(id) >= emitters.size()) return nullptr;
    return emitters[id];
}

unsigned int BridgeDispatcher::selectedEmitterStableId() const
{
    if (m_selectedEmitterId < 0) return 0;
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return 0;
    const auto& emitters = (*m_pParticleSystem)->getEmitters();
    if (static_cast<size_t>(m_selectedEmitterId) >= emitters.size()) return 0;
    const ParticleSystem::Emitter* selected = emitters[m_selectedEmitterId];
    return selected != nullptr ? selected->stableId : 0;
}

void BridgeDispatcher::reconcileSelectionAfterDeletion(unsigned int stableId)
{
    int nextId = -1;
    if (stableId != 0 && m_pParticleSystem != nullptr && *m_pParticleSystem)
    {
        const auto& emitters = (*m_pParticleSystem)->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr && emitters[i]->stableId == stableId)
            {
                nextId = static_cast<int>(i);
                break;
            }
        }
    }
    if (nextId == m_selectedEmitterId) return;

    m_selectedEmitterId = nextId;
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "emitters/selected"},
        {"payload", json{{"id", m_selectedEmitterId < 0
                                   ? json(nullptr)
                                   : json(m_selectedEmitterId)}}},
    };
    m_emit(env.dump());
}

// Capture-undo helper. Wraps the dispatcher's m_undo with the
// current selection index so a future undo restores the
// pre-mutation state. coalesceKey defaults to 0 = never coalesce
// (structural ops must never fold across an add/delete/move). A
// non-zero key routes to CapturePreCoalesced so rapid same-key edits
// (a wheel-spun spinner / held arrow on one emitter) fold into a
// single undo step within the time window — see legacy EP_CHANGE
// coalescing (main.cpp ~2682).
// Forwarder to the member chokepoint — the member also folds
// in the engine's current reference-object transform as snapshot side-band
// aux (engine-guarded) so undo/redo carries the ref transform on the same
// timeline. Behavior for the ~30 particle-edit call sites is unchanged.
void BridgeDispatcher::captureUndo(DWORD coalesceKey) { CaptureUndoPoint(coalesceKey); }

// F4: link-group propagation. After a shared (non-exempt) field is
// edited on a linked emitter, copy its non-exempt params to every
// group sibling — the new-UI equivalent of the legacy post-edit
// chokepoint in CaptureUndo (src/main.cpp). MUST be called AFTER the
// mutation; the pre-mutation captureUndo() snapshots the whole
// system, so a single Ctrl+Z restores the entire group atomically.
// No-op for unlinked emitters (linkGroup == 0).
void BridgeDispatcher::propagateLinkGroup(ParticleSystem::Emitter* edited)
{
    if (edited == nullptr || edited->linkGroup == 0) return;
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return;
    ParticleSystem* sys = m_pParticleSystem->get();
    std::vector<ParticleSystem::Emitter*> members =
        GetLinkGroupMembers(*sys, edited->linkGroup);
    const LinkExemptFlags& exempt =
        sys->getLinkExemptFlags(edited->linkGroup);
    bool copied = false;
    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i] != edited)
        {
            members[i]->copySharedParamsFrom(*edited, exempt);
            copied = true;
        }
    }
    // copySharedParamsFrom REASSIGNS each sibling's non-exempt
    // track multisets — invalidating any live particle's cached cursor
    // iterators into them, across ALL non-exempt tracks (not just the
    // one the user edited). The callers reseat only the edited track,
    // which leaves a sibling's OTHER track cursors dangling → the next
    // Engine::Update derefs a singular iterator (xtree:181 assert).
    // Reseat every instance's cursors here, at the single choke point
    // where the orphaning happens, so no caller can forget. Only fires
    // for a linked emitter with ≥1 sibling (unlinked edits keep their
    // own cheaper per-track reseat).
    if (copied && m_engine != nullptr)
        m_engine->OnParticleSystemChanged(-1);
}

json BridgeDispatcher::DispatchInternal(const nlohmann::json& parsed)
{
    std::string id;
    if (auto it = parsed.find("id"); it != parsed.end() && it->is_string())
    {
        id = it->get<std::string>();
    }
    std::string kind;
    if (auto it = parsed.find("kind"); it != parsed.end() && it->is_string())
    {
        kind = it->get<std::string>();
    }
    const json params = parsed.value("params", json::object());

    // One request context serves the whole dispatch. Its envelope helpers
    // (SetRes with id-patched-last, SendOk/SendErr, RequireEngine, MarkDirty)
    // are the former ladder lambdas verbatim — see BridgeRequestContext.h.
    BridgeRequestContext ctx{*this, id, kind, params};

    if (kind.empty())
    {
        ctx.SendErr("missing kind");
        return ctx.res;
    }

    // Per-domain handlers (BridgeDispatch_*.cpp — the Phase A split of the
    // former 4,800-line kind ladder; tasks/2026-07-06-heavyweight-refactor-
    // plan.md). Kinds are exact-match and mutually exclusive, so the call
    // order carries no semantics.
    if (TryDispatchEngine(ctx, kind) || TryDispatchEmitters(ctx, kind) ||
        TryDispatchFile(ctx, kind)   || TryDispatchAssets(ctx, kind)   ||
        TryDispatchShell(ctx, kind)  || TryDispatchSpawner(ctx, kind))
    {
        return ctx.res;
    }

    ctx.SendErr("not implemented yet");
    return ctx.res;
}

void BridgeDispatcher::EmitAcceleratorPressed(const std::string& combo)
{
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "accelerator/pressed"},
        {"payload", {{"combo", combo}}},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitCursorPosition3D(float x, float y, float z)
{
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "cursor/position-3d"},
        {"payload", {{"x", x}, {"y", y}, {"z", z}}},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitManipulatorDrag(const json& payload)
{
    if (!m_emit) return;
    json env = { {"type", "evt"}, {"kind", "engine/manipulator/drag"}, {"payload", payload} };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitEmittersTreeChanged()
{
    if (!m_emit) return;
    const unsigned long long now = GetTickCount64();
    // [#510] Record-only coalesce: skip building + pushing the tree if the last
    // push was < kRecordEmitThrottleMs ago, so a record's rapid host-side edits
    // don't flood the web with re-fetches. Leading-edge; continuous edits still
    // deliver ~30 Hz.
    if (m_recordEmitThrottle) {
        if (now - m_lastTreeEmitTick < kRecordEmitThrottleMs) return;
    }
    // [B1] Live trailing coalesce (see the header field block). Drive
    // (m_ephemeral) is exempt: asserts must see every change.
    else if (!m_ephemeral && now - m_lastTreeEmitTick < kEmitCoalesceMs) {
        m_treeEmitPending = true;
        return;
    }
    m_lastTreeEmitTick = now;
    m_treeEmitPending  = false;
    EmitEmittersTreeChangedNow();
}

void BridgeDispatcher::ResetSelectionAndEmitDocumentChanged()
{
    // Root (index 0) when the new document has emitters, matching file/new's
    // legacy-parity behaviour of opening with the first emitter selected;
    // otherwise nothing is selected.
    const bool hasEmitters = (m_pParticleSystem != nullptr && *m_pParticleSystem
                              && !(*m_pParticleSystem)->getEmitters().empty());
    m_selectedEmitterId = hasEmitters ? 0 : -1;

    // A document swap is one atomic notification, not a high-frequency edit.
    // Drop any pending old-document broadcasts and bypass live coalescing so
    // the observable order cannot become selection -> tree -> state.
    const unsigned long long now = GetTickCount64();
    m_stateEmitPending = false;
    m_treeEmitPending  = false;
    if (m_emit && m_engine)
    {
        m_lastStateEmitTick = now;
        EmitEngineStateChangedNow();
    }
    if (m_emit)
    {
        m_lastTreeEmitTick = now;
        EmitEmittersTreeChangedNow();
    }

    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "emitters/selected"},
        {"payload", json{{"id", m_selectedEmitterId < 0 ? json(nullptr)
                                                        : json(m_selectedEmitterId)}}},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitEmittersTreeChangedNow()
{
    // Build the synthetic root + per-actual-root children, matching the
    // shape returned by `emitters/list`.
    json children = json::array();
    if (m_pParticleSystem != nullptr && *m_pParticleSystem)
    {
        const ParticleSystem* sys = m_pParticleSystem->get();
        const auto& emitters = sys->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr && emitters[i]->parent == nullptr)
            {
                children.push_back(BuildEmitterTreeNode(sys, i));
            }
        }
    }
    json tree = {
        {"id",        -1},
        {"name",      ""},
        {"role",      "root"},
        {"linkGroup", 0},
        {"visible",   true},
        {"spawn", json{
            {"lifetime", 0.0}, {"useBursts", false}, {"nBursts", 0},
            {"burstDelay", 0.0}, {"nParticlesPerSecond", 0}, {"nParticlesPerBurst", 0},
        }},
        {"children",  children},
    };
    json env = {
        {"type",    "evt"},
        {"kind",    "emitters/tree/changed"},
        {"payload", json{{"root", tree}}},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::CommitReferenceObjectTransform()
{
    if (!m_engine) return;
    const D3DXVECTOR3 pos = m_engine->GetReferencePosition();
    const D3DXVECTOR3 rot = m_engine->GetReferenceRotation();
    if (!m_ephemeral && !(m_testHost && !m_settingsLive))
        PersistReferenceObjectTransform(pos, rot);
    SetDirty(true);   // markDirty() in DispatchSync is a local lambda over this
    EmitEngineStateChanged();
}

// See the header. PRE-mutation capture chokepoint: live PS +
// selection + (engine-guarded) current reference-object transform as aux.
void BridgeDispatcher::CaptureUndoPoint(
    DWORD coalesceKey, UndoStack::BudgetRetention retention)
{
    if (m_undo == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem) return;
    const ParticleSystem* sys = m_pParticleSystem->get();
    size_t selIdx = SIZE_MAX;
    if (m_selectedEmitterId >= 0
        && static_cast<size_t>(m_selectedEmitterId) < sys->getEmitters().size())
    {
        selIdx = static_cast<size_t>(m_selectedEmitterId);
    }
    UndoStack::EditorAux aux;   // defaults to {0,0,0}
    if (m_engine)               // nullable when D3D init failed — never deref unguarded
    {
        const D3DXVECTOR3 p = m_engine->GetReferencePosition();
        const D3DXVECTOR3 r = m_engine->GetReferenceRotation();
        aux.refPos[0] = p.x; aux.refPos[1] = p.y; aux.refPos[2] = p.z;
        aux.refRot[0] = r.x; aux.refRot[1] = r.y; aux.refRot[2] = r.z;
        aux.refName = m_engine->GetReferenceObjectName();   // gate the restore on identity
    }
    // Preserve both capture paths (matches the lambda this replaced + the
    // auto-cap's Capture(...,0)): non-zero key coalesces, zero never does.
    if (coalesceKey != 0)
        m_undo->CapturePreCoalesced(*sys, selIdx, coalesceKey, aux);
    else
        m_undo->Capture(*sys, selIdx, 0, aux, retention);
}

// Host-facing wrapper for a gizmo gesture — one non-coalescing
// undo point. Called on the first real per-move mutation of a drag.
void BridgeDispatcher::CaptureReferenceTransformUndoPoint()
{
    CaptureUndoPoint(0);
}

void BridgeDispatcher::EmitEngineStateChanged()
{
    if (!m_emit || !m_engine) return;
    const unsigned long long now = GetTickCount64();
    // [#510] Record-only coalesce (see EmitEmittersTreeChanged).
    if (m_recordEmitThrottle) {
        if (now - m_lastStateEmitTick < kRecordEmitThrottleMs) return;
    }
    // [B1] Live trailing coalesce (see the header field block). Drive
    // (m_ephemeral) is exempt: asserts must see every change.
    else if (!m_ephemeral && now - m_lastStateEmitTick < kEmitCoalesceMs) {
        m_stateEmitPending = true;
        return;
    }
    m_lastStateEmitTick = now;
    m_stateEmitPending  = false;
    EmitEngineStateChangedNow();
}

void BridgeDispatcher::EmitEngineStateChangedNow()
{
    json spawnerJson = m_spawnerDriver
        ? SpawnerConfigToJson(m_spawnerDriver->GetConfig())
        : m_spawnerConfig;
    const std::wstring activeModPath = m_modManager ? m_modManager->GetPrimaryLayerPath() : std::wstring();
    const bool leaveParticles = (m_pParticleSystem != nullptr && *m_pParticleSystem)
        ? (*m_pParticleSystem)->getLeaveParticles()
        : true;
    const bool canUndo = ComputeCanUndo();
    const bool canRedo = m_undo ? m_undo->CanRedo() : false;
    json env = {
        {"type",    "evt"},
        {"kind",    "engine/state/changed"},
        {"payload", BuildEngineStateSnapshot(m_engine, m_currentFilePath, m_dirty, spawnerJson, m_selectedEmitterId, activeModPath, leaveParticles, canUndo, canRedo)},
    };
    m_emit(env.dump());
}

// [C3] Insert (or overwrite) a preview cache entry at MRU; evict at cap.
void BridgeDispatcher::PreviewCachePut(const std::string& key, PreviewCacheEntry entry)
{
    if (auto it = m_previewLruIdx.find(key); it != m_previewLruIdx.end())
    {
        it->second->second = std::move(entry);
        m_previewLru.splice(m_previewLru.begin(), m_previewLru, it->second);
        return;
    }
    m_previewLru.emplace_front(key, std::move(entry));
    m_previewLruIdx[key] = m_previewLru.begin();
    while (m_previewLru.size() > kPreviewLruCap)
    {
        m_previewLruIdx.erase(m_previewLru.back().first);
        m_previewLru.pop_back();
    }
}

// [C3] UI thread, under the WM_APP_PREVIEW_READY handler: cache each
// finished encode and tell the web to refetch. Stale-epoch results are
// dropped (mod switched while the encode was in flight).
void BridgeDispatcher::DrainPreviewResults()
{
    if (!m_previewWorker) return;
    for (auto& r : m_previewWorker->TakeFinished())
    {
        if (r.epoch != m_previewEpoch) continue;
        m_previewInFlight.erase(r.key);
        PreviewCachePut(r.key, PreviewCacheEntry{r.status, std::move(r.dataUri),
                                                 r.srcW, r.srcH});
        if (m_emit)
        {
            json env = {
                {"type", "evt"},
                {"kind", "textures/preview-ready"},
                {"payload", {{"filename", WideToUtf8(r.filename)},
                             {"flattenAlpha", r.flattenAlpha},
                             {"status", r.status}}},
            };
            m_emit(env.dump());
        }
    }
}

void BridgeDispatcher::FlushPendingEmits()
{
    // Trailing edge of the [B1] live coalesce. Emit order matches the
    // paired-broadcast order every mutating handler uses (tree first,
    // then state), so a web listener that refetches on tree/changed sees
    // the settled state snapshot arrive after it, same as a direct emit.
    if (m_treeEmitPending && m_emit)
    {
        m_treeEmitPending  = false;
        m_lastTreeEmitTick = GetTickCount64();
        EmitEmittersTreeChangedNow();
    }
    if (m_stateEmitPending && m_emit && m_engine)
    {
        m_stateEmitPending  = false;
        m_lastStateEmitTick = GetTickCount64();
        EmitEngineStateChangedNow();
    }
}

bool BridgeDispatcher::ComputeCanUndo() const
{
    // Auto-cap-aware: undo/perform inserts a snapshot of the current
    // live state when cursor==depth AND live is skewed ahead of the tip
    // (IsLiveAhead), then calls Undo(). With the auto-cap, Undo() needs
    // a post-cap cursor>=2, i.e. depth>=1 (one captureUndo-bearing
    // mutation has run). WITHOUT the auto-cap (cursor==depth but live is
    // in sync, e.g. right after a Redo()), Undo()'s own CanUndo (cursor>=2)
    // gates the call, so canUndo needs depth>=2. Mid-redo-branch
    // (cursor<depth) no auto-cap fires and cursor>=2 gates as well.
    if (m_undo == nullptr) return false;
    const size_t cursor = m_undo->Cursor();
    const size_t depth  = m_undo->Depth();
    if (cursor == depth) return m_undo->IsLiveAhead() ? (depth >= 1) : (depth >= 2);
    return cursor >= 2;
}

void BridgeDispatcher::ResetSavedBaseline()
{
    if (m_pParticleSystem && *m_pParticleSystem)
        m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
    else
        m_savedSnapshot.clear();
}

void BridgeDispatcher::SetDirty(bool dirty)
{
    if (m_dirty == dirty) return;  // debounce — no-op if already in target state
    m_dirty = dirty;
    EmitDirtyChanged();
    // Don't broadcast engine/state/changed here. Callers that
    // SetDirty(true) at the END of an engine setter already emitted a
    // state/changed for the parameter change; the dirty bit ride-alongs
    // via the dedicated dirty/changed event channel + the next
    // snapshot read. Callers that SetDirty(false) (file/new, file/open,
    // file/save success) emit their own state/changed.
}

void BridgeDispatcher::EmitDirtyChanged()
{
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "dirty/changed"},
        {"payload", {{"dirty", m_dirty}}},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitCloseRequested()
{
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "app/close-requested"},
        {"payload", json::object()},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitRecentChanged()
{
    if (!m_emit) return;
    json paths = json::array();
    for (const auto& w : m_recentFiles)
    {
        paths.push_back(WideToUtf8(w));
    }
    json env = {
        {"type",    "evt"},
        {"kind",    "recent/changed"},
        {"payload", {{"paths", paths}}},
    };
    m_emit(env.dump());
}

// Deserialize a ParticleSystem snapshot from an UndoStack entry and
// swap it into the host-owned slot. Teardown order mirrors file/open
// at BridgeDispatcher.cpp's file/open handler: kill any cursor-bound
// attached ParticleSystemInstance first (else KillParticleSystem on
// the about-to-be-freed system crashes), then Engine::Clear (drops
// cached per-instance state), then swap the unique_ptr (deletes old
// PS), then OnParticleSystemChanged(-1) + ReloadTextures so the
// engine re-binds + re-acquires textures for the restored system.
// Wrapped in UndoStack::BeginApplying/EndApplying so the swap doesn't
// recursively trigger a Capture(). Cross-reference legacy
// RestoreFromSnapshot at src/main.cpp:916 for the original pattern.
void BridgeDispatcher::ApplyUndoSnapshot(const std::vector<char>& buf,
                                         size_t selIdx,
                                         const UndoStack::EditorAux& aux)
{
    if (m_undo == nullptr) return;
    if (m_pParticleSystem == nullptr) return;

    m_undo->BeginApplying();

    ParticleSystem* sys = nullptr;
    try
    {
        sys = UndoStack::Deserialize(buf);
    }
    catch (...)
    {
        m_undo->EndApplying();
        return;
    }
    if (sys == nullptr)
    {
        m_undo->EndApplying();
        return;
    }

    if (m_ppAttachedParticleSystem
        && *m_ppAttachedParticleSystem
        && m_engine)
    {
        m_engine->KillParticleSystem(*m_ppAttachedParticleSystem);
        *m_ppAttachedParticleSystem = nullptr;
    }
    if (m_engine) m_engine->Clear();

    *m_pParticleSystem = std::unique_ptr<ParticleSystem>(sys);

    if (m_engine)
    {
        m_engine->OnParticleSystemChanged(-1);
        m_engine->ReloadTextures();
    }

    if (selIdx != SIZE_MAX
        && selIdx < (*m_pParticleSystem)->getEmitters().size())
    {
        m_selectedEmitterId = static_cast<int>(selIdx);
    }
    else
    {
        m_selectedEmitterId = -1;
    }
    if (m_emit)
    {
        json env = {
            {"type",    "evt"},
            {"kind",    "emitters/selected"},
            {"payload", json{{"id", m_selectedEmitterId < 0
                                       ? json(nullptr)
                                       : json(m_selectedEmitterId)}}},
        };
        m_emit(env.dump());
    }

    // Restore the reference-object transform that rode with this
    // snapshot, and keep the registry in step. Engine-guarded (null when D3D
    // init failed); applied AFTER the PS swap/Clear so nothing clobbers it.
    // The undo/perform caller emits engine/state/changed next, which re-reads
    // these getters → the React Transform spinners refresh (no schema change).
    // Stays in lockstep with CaptureUndoPoint's matching `if (m_engine)` guard:
    // with no engine there's no gizmo, so aux is always {0,0,0} on these
    // entries and skipping the restore drops nothing.
    // Restore the captured transform ONLY when the snapshot's reference object is
    // still the one shown. With per-object transform memory a swap leaves each
    // object at its own placement; re-applying object A's captured transform onto a
    // swapped-to object B would teleport B (the emitter-edit snapshot still undoes
    // correctly -- only the side-band transform restore is gated). A case-
    // insensitive compare mirrors the catalog's name folding.
    if (m_engine && _stricmp(aux.refName.c_str(), m_engine->GetReferenceObjectName().c_str()) == 0)
    {
        const D3DXVECTOR3 p(aux.refPos[0], aux.refPos[1], aux.refPos[2]);
        const D3DXVECTOR3 r(aux.refRot[0], aux.refRot[1], aux.refRot[2]);
        m_engine->SetReferenceObjectTransform(p, r);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistReferenceObjectTransform(p, r);
    }

    // Dirty bit follows content-equality against the saved baseline.
    // `buf` IS the serialized form of the just-restored state (no
    // engine call between Deserialize and here mutates PS content),
    // so a direct byte compare against m_savedSnapshot is sufficient
    // and faster than re-serializing. m_savedSnapshot is refreshed
    // on file/new + file/open + file/save success; empty means
    // "no saved baseline" (boot before any file action) and the
    // compare always reports dirty.
    SetDirty(buf != m_savedSnapshot);

    m_undo->EndApplying();
}

// Demote single-member link groups to linkGroup=0 so the data
// layer matches the render layer's existing filter at
// computeLinkGroupBrackets (web/apps/editor/src/lib/link-group-colors.ts).
// Called from emitters/delete + linkGroups/set-membership (the two
// mutation paths that can leave a group with exactly one member —
// see the ROADMAP for the enumeration) AND from file/open
// after binding a loaded ParticleSystem (so older saved files
// with singletons self-correct on load). Callers handle their own
// captureUndo() / SetDirty() — the sweep itself is silent on both.
void BridgeDispatcher::EnforceSingleMemberLinkGroups()
{
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return;
    ParticleSystem* sys = m_pParticleSystem->get();
    const auto& emitters = sys->getEmitters();

    // Pass 1: count members per positive linkGroup. uint32_t key
    // matches Emitter::linkGroup's type; std::map keeps the surface
    // header-light (no extra include needed for unordered_map; map
    // is already transitively visible via ParticleSystem.h).
    std::map<uint32_t, int> counts;
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        if (emitters[i] == nullptr) continue;
        uint32_t g = emitters[i]->linkGroup;
        if (g == 0) continue;
        ++counts[g];
    }

    // Pass 2: demote the lone member of every singleton group. Null-
    // checked iteration matches the existing groupId==-1 scan pattern
    // at BridgeDispatcher.cpp's linkGroups/set-membership handler.
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        if (emitters[i] == nullptr) continue;
        uint32_t g = emitters[i]->linkGroup;
        if (g == 0) continue;
        auto it = counts.find(g);
        if (it != counts.end() && it->second == 1)
        {
            emitters[i]->linkGroup = 0;
        }
    }
}

void BridgeDispatcher::EmitStatsTick(float fps, int emitters,
                                     int particles, int instances,
                                     bool overload)
{
    if (!m_emit) return;
    // [hard-guard] Poll the engine's one-shot spawn-refusal record on the
    // same 4 Hz cadence (this is EmitStatsTick's only caller — the stats
    // timer). Done BEFORE the stats-freeze gate so a refusal is never
    // swallowed by the test-only freeze knob.
    {
        Engine::SpawnRefusal refusal;
        if (m_engine && m_engine->TakeSpawnRefusal(&refusal))
        {
            // Surface the refusal to the web (transient banner).
            json env = {
                {"type",    "evt"},
                {"kind",    "engine/overload/refused"},
                {"payload", {
                    {"estimated",      refusal.estimated},
                    {"cap",            refusal.cap},
                    {"attemptedCount", refusal.attemptedCount},
                }},
            };
            m_emit(env.dump());
            // The engine's SpawnerDriver self-disabled on the refusal
            // (enabled=false), but the cached spawner config + the web's
            // panel toggle don't know. Mirror spawner/stop: update the
            // live driver first (EmitEngineStateChanged reads it), then
            // sync the JSON cache, then broadcast so the panel toggle
            // reflects the disabled state. CancelPending also covers
            // refusals recorded OUTSIDE SpawnerDriver::Tick (the
            // edit-time SetEstimatedLoad clear), whose armed/queued
            // manual bursts the Tick refusal branch never sees. Residual
            // window: a Tick can fire a queued burst before this ≤250 ms
            // poll runs — safe, since the spawn-time gate re-checks the
            // cap; worst case is a duplicate banner, never an over-cap
            // placement.
            if (m_spawnerDriver)
            {
                m_spawnerDriver->CancelPending();
                SpawnerConfig cfg = m_spawnerDriver->GetConfig();
                cfg.enabled = false;
                m_spawnerDriver->SetConfig(cfg);
            }
            if (m_spawnerConfig.is_object())
            {
                m_spawnerConfig["enabled"] = false;
            }
            EmitEngineStateChanged();
        }
    }
    // Gate test-driven freeze. When frozen, the React
    // StatusBar has already cleared its state (via stats/frozen-
    // changed) and is rendering placeholders; emitting would
    // re-populate it with non-deterministic per-frame values.
    if (m_statsFrozen) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "stats/tick"},
        {"payload", {
            {"fps",       fps},
            {"emitters",  emitters},
            {"particles", particles},
            {"instances", instances},
            // Latched preview overload flag — spawning is suppressed
            // while true (Engine::IsSpawnOverloadActive).
            {"overload",  overload},
        }},
    };
    m_emit(env.dump());
}

void BridgeDispatcher::EmitSpawnerActiveCount(int count)
{
    if (!m_emit) return;
    json env = {
        {"type",    "evt"},
        {"kind",    "spawner/active-count"},
        {"payload", {{"count", count}}},
    };
    m_emit(env.dump());
}

bool BridgeDispatcher::EmitWindowState(bool maximized)
{
    if (!m_emit) return false;   // web not wired yet — caller replays on app/ready
    json env = {
        {"type",    "evt"},
        {"kind",    "window/state"},
        {"payload", {{"maximized", maximized}}},
    };
    m_emit(env.dump());
    return true;
}

bool BridgeDispatcher::EmitAutosaveHealth(bool healthy)
{
    if (!m_emit) return false;   // web not wired yet — caller replays on app/ready
    json env = {
        {"type",    "evt"},
        {"kind",    "autosave/health"},
        {"payload", {{"healthy", healthy}}},
    };
    m_emit(env.dump());
    return true;
}

} // namespace host
