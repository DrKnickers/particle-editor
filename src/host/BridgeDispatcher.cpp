#include "BridgeDispatcher.h"

#include "AcceleratorBridge.h"
#include "InputDispatcher.h"
#include "LayoutBroker.h"
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
#include "../SpawnerDriver.h"
#include "../UndoStack.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
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

// (session 3): parse a CSS colour string from getComputedStyle
// ("#rrggbb", "#rgb", or "rgb(r, g, b)" / "rgba(r, g, b, a)") into a
// COLORREF for the host/backing-color handler. Returns true on success.
// Defensive — unknown formats return false so the handler answers
// ok:false rather than guessing a colour.
static bool ParseCssColorToColorRef(const std::string& in, COLORREF& out)
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

// Defined in src/UI/EmitterList.cpp; reused by the new-UI Phase 3
// Screen 4 Batch B1 emitter-mutation handlers. Stays non-static so the
// host's dispatcher can link against it without a header dependency on
// EmitterList.cpp (which still belongs to the legacy --legacy-ui chrome).
extern std::string GenerateDuplicateName(const ParticleSystem* system,
                                          const std::string&     sourceName);

namespace host {

namespace {

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

// UTF-8 ↔ UTF-16 helpers — kept local because the bridge only needs them
// for the handful of `engine/*/custom-path` setters and the snapshot's
// path arrays. Mirrors the equivalents in HostWindow.cpp.
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
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

// Recent-files registry helpers. Phase 3 Screen 8 Batch 3.
//
// Storage layout matches legacy's `AddToHistory` / `GetHistory` in
// src/main.cpp:650-768 — values under `HKCU\Software\AloParticleEditor`
// keyed by filename, with the FILETIME payload encoded as REG_BINARY.
// The list is ordered most-recent-first by reading the FILETIME values
// and sorting descending. Cap of 9 matches `NUM_HISTORY_ITEMS` at
// src/main.cpp:47. Legacy and new-UI share the same registry path, so
// the recent-files menu stays consistent across both UI modes.

constexpr int kMaxRecentFiles = 9;
constexpr const wchar_t* kRegistryKeyPath = L"Software\\AloParticleEditor";

// Read the registry-backed history into a vector ordered most-recent
// first. Mirrors the loop in legacy GetHistory. Silently returns an
// empty vector when the key does not exist (first-run case).
std::vector<std::wstring> ReadRecentFiles()
{
    std::vector<std::pair<ULONGLONG, std::wstring>> entries;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return {};
    }

    for (int i = 0;; ++i)
    {
        wchar_t name[1024] = {};
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        DWORD type = 0, size = 0;
        LONG err = RegEnumValueW(hKey, i, name, &nameLen, nullptr,
                                 &type, nullptr, &size);
        if (err != ERROR_SUCCESS) break;

        if (type == REG_BINARY && size == sizeof(FILETIME))
        {
            FILETIME ft = {};
            DWORD sz = sizeof(ft);
            if (RegQueryValueExW(hKey, name, nullptr, &type,
                                 reinterpret_cast<BYTE*>(&ft),
                                 &sz) == ERROR_SUCCESS)
            {
                ULARGE_INTEGER ull;
                ull.LowPart  = ft.dwLowDateTime;
                ull.HighPart = ft.dwHighDateTime;
                entries.emplace_back(ull.QuadPart, std::wstring(name));
            }
        }
    }
    RegCloseKey(hKey);

    // Sort by timestamp descending (most recent first).
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::wstring> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.second));
    // Cap to kMaxRecentFiles.
    if (out.size() > static_cast<size_t>(kMaxRecentFiles))
    {
        out.resize(kMaxRecentFiles);
    }
    return out;
}

// Add (or move-to-top) `path` in the registry. Mirrors legacy
// AddToHistory: writes the FILETIME for "now" under the path-as-key,
// then trims entries beyond the cap by deleting the oldest. Returns
// the resulting list (most-recent-first).
std::vector<std::wstring> WriteRecentFile(const std::wstring& path)
{
    FILETIME ft;
    SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        // Set the value — if the key already exists, this updates the
        // FILETIME so the path moves to the top of the most-recent list.
        RegSetValueExW(hKey, path.c_str(), 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&ft), sizeof(ft));
        RegCloseKey(hKey);
    }

    // Re-read and trim. Legacy does this lazily inside GetHistory; we
    // do it eagerly here so the React side sees the post-trim list.
    auto list = ReadRecentFiles();

    // Trim by deleting any entries that fall off the end of the cap.
    if (list.size() > static_cast<size_t>(kMaxRecentFiles))
    {
        HKEY hTrim;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                          KEY_READ | KEY_WRITE, &hTrim) == ERROR_SUCCESS)
        {
            for (size_t i = kMaxRecentFiles; i < list.size(); ++i)
            {
                RegDeleteValueW(hTrim, list[i].c_str());
            }
            RegCloseKey(hTrim);
        }
        list.resize(kMaxRecentFiles);
    }
    return list;
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

// Screen 4 Batch B1 — wire-name ↔ LinkExemptFlags::member mapping.
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

//: walk a ParticleSystem and build an EmitterTreeNode-shaped JSON
// tree. Mirrors the schema definition at
// web/packages/bridge-schema/src/index.ts:91. Children are computed
// from each emitter's `spawnDuringLife` / `spawnOnDeath` indices in
// the same order as legacy `ImportEmitters_AddTreeItem` (during-life
// before on-death) so the import dialog tree matches.
//
// Screen 4 Batch A: extended with `role` / `linkGroup` / `visible`.
// `role` is derived from how this emitter is attached to its parent's
// spawn slot (lifetime vs death); top-level emitters return "root".
// The sentinel for "no spawn child" is `(size_t)-1` — matches the
// legacy EmitterList.cpp usage at e.g. [src/UI/EmitterList.cpp:1349].
json BuildEmitterTreeNode(const ParticleSystem* sys, size_t idx)
{
    if (sys == nullptr || idx >= sys->getEmitters().size()) return json::object();
    const ParticleSystem::Emitter& emit = sys->getEmitter(idx);
    json children = json::array();
    if (emit.spawnDuringLife != static_cast<size_t>(-1))
        children.push_back(BuildEmitterTreeNode(sys, emit.spawnDuringLife));
    if (emit.spawnOnDeath != static_cast<size_t>(-1))
        children.push_back(BuildEmitterTreeNode(sys, emit.spawnOnDeath));

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
        // chain-load warning: spawn-quantity params mirrored onto
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

// Phase 3 Screen 8 Batch 4 — default spawner-config JSON. Mirrors the
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
        // Editor-level state (Screen 8 Batch 3).
        {"currentFilePath",       filePathField},
        {"dirty",                 dirty},

        // Ground
        {"ground",                engine->GetGround()},
        {"groundZ",               engine->GetGroundZ()},
        {"groundTexture",         engine->GetGroundTexture()},
        {"groundSolidColor",      static_cast<unsigned int>(engine->GetGroundSolidColor())},
        {"groundSlotCustomPaths", groundPaths},

        // Skydome
        {"skydomeSlot",           engine->GetSkydomeSlot()},
        {"skydomeCustomPaths",    skyPaths},

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

        // Spawner (Phase 3 Screen 8 Batch 4) — cached on the dispatcher.
        // Host doesn't yet own a SpawnerDriver*; the cache is what
        // spawner/start writes into and what subsequent snapshots read
        // back. Empty object when never set (shouldn't happen because
        // the constructor seeds it from DefaultSpawnerConfigJson()).
        {"spawner",               spawnerConfig.is_null() || spawnerConfig.empty()
                                      ? DefaultSpawnerConfigJson()
                                      : spawnerConfig},

        // Screen 4 Batch A — selected emitter (editor state). Serialise
        // -1 as JSON null so the schema's `number | null` discriminator
        // works without a sentinel-aware client.
        {"selectedEmitterId",     selectedEmitterId < 0
                                      ? json(nullptr)
                                      : json(selectedEmitterId)},

        // D6 — active mod path. Empty wstring serialises as JSON
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

} // namespace

BridgeDispatcher::BridgeDispatcher(Engine* engine, LayoutBroker& layout,
                                    AcceleratorBridge& accel, EmitFn emit,
                                    bool useTestHost)
    : m_engine(engine), m_layout(layout), m_accel(accel), m_emit(std::move(emit))
    , m_testHost(useTestHost)
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

    // Phase 3 Screen 8 Batch 4: seed the spawner-config cache from the
    // shared default JSON so the first snapshot returns the same struct
    // a freshly-constructed `SpawnerConfig()` would. Subsequent
    // spawner/start requests overwrite this in DispatchInternal.
    m_spawnerConfig = DefaultSpawnerConfigJson();
}

// G2: defensive envelope for any json::exception that escapes
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

void BridgeDispatcher::SetEngine(Engine* engine)
{
    m_engine = engine;
    // [guard-config §2] Reapply the cached overload-guard config so a
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

    // G2: catch json::exception escaping DispatchInternal.
    json res;
    try
    {
        res = DispatchInternal(parsed);
    }
    catch (const json::exception& e)
    {
        fprintf(stderr, "[host] BridgeDispatcher::Dispatch: type/conversion exception: %s\n", e.what());
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

    // G2: catch json::exception escaping DispatchInternal.
    try
    {
        return DispatchInternal(parsed).dump();
    }
    catch (const json::exception& e)
    {
        fprintf(stderr, "[host] BridgeDispatcher::DispatchSync: type/conversion exception: %s\n", e.what());
        return BuildDispatchExceptionEnvelope(parsed, e.what()).dump();
    }
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

    // Response envelope built by the kind-handler ladder. Setting
    // `responseSet` causes the ladder to fall through to return; the
    // default (no kind matched) becomes the "not implemented" branch.
    // Response envelope built by the kind-handler ladder. `sendOk` /
    // `sendErr` write into `res`, then handlers `return res`.
    json res;

    auto setRes = [&](json env)
    {
        res = std::move(env);
        if (!id.empty()) res["id"] = id; else res["id"] = nullptr;
    };
    auto sendOk = [&](const json& data) {
        setRes(json{{"type","res"},{"ok",true},{"data",data}});
    };
    auto sendErr = [&](const std::string& msg) {
        setRes(json{{"type","res"},{"ok",false},{"error",msg}});
    };

    if (kind.empty())
    {
        sendErr("missing kind");
        return res;
    }

    // Every engine/* handler routes through this guard: if the engine
    // isn't yet constructed (or has been torn down), refuse the request
    // with a structured error instead of crashing on a null deref.
    auto requireEngine = [&](const char* what) -> bool {
        if (m_engine) return true;
        sendErr(std::string("engine not constructed (") + what + ")");
        return false;
    };

    // Phase 3 Screen 8 Batch 3: every mutating engine/set/* and
    // engine/action/* (the destructive ones) ends with a SetDirty(true)
    // so the dirty flag + dirty/changed event fire after the parameter
    // change. SetDirty itself debounces (no-op when already dirty), so
    // repeated mutations don't spam the event channel.
    auto markDirty = [&]() { SetDirty(true); };

    // -------- layout/viewport-rect --------
    if (kind == "layout/viewport-rect")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int w = params.value("w", 0);
        int h = params.value("h", 0);
        m_layout.Apply(x, y, w, h);
        sendOk(json::object());
        return res;
    }

    // -------- layout/scene-rect --------
    // B1.4 T4c. Under the popup-spans-window architecture the
    // popup HWND covers the WebView's main-row area at all times.
    // This message updates the SUB-RECT inside the popup where the
    // rendered scene should be visible — AlphaCompositor stamps
    // alpha=0 outside this rect, so UI panels behind those bands
    // show through (and receive their own mouse events, courtesy of
    // WS_EX_LAYERED + ULW_ALPHA hit-test semantics).
    //
    // Engine code is unchanged — camera frustum still uses the full
    // popup backbuffer aspect (popup-rect aspect per the T4c
    // questionnaire). This message is alpha-mask-only; it does NOT
    // trigger Engine::Reset. Splitter drag can fire this per frame
    // without stacking expensive D3D9 device resets.
    //
    // [resize-perf hygiene] In PRODUCTION no web code sends
    // layout/viewport-rect at all (the handler above is exercised only
    // by tests/POCs, e.g. viewport-resize.spec.ts) — the live
    // window-resize reset driver is the NATIVE path:
    // WM_WINDOWPOSCHANGED → LayoutBroker::PredictAndApply →
    // Engine::Reset (deferred to gesture settle while in sizemove,
    // fix A). Don't chase this bridge binding when profiling resets.
    if (kind == "layout/scene-rect")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int w = params.value("w", 0);
        int h = params.value("h", 0);
        m_layout.SetSceneRect(x, y, w, h);
        sendOk(json::object());
        return res;
    }

    // -------- animate-scene-rect --------
    // [Item 3] One-shot dock-slide animation. Instead of the per-frame
    // layout/scene-rect stream (which the uncapped render loop samples at
    // irregular Δt → a juddering viewport edge), the web sends ONE of these at
    // the dock toggle; LayoutBroker then re-renders the engine at a wall-clock-
    // lerped rect every frame, synced to the CSS flex-grow tween. only
    // (StartSceneAnim is a no-op when no DComp compositor is attached). `from`/
    // `to` are scene rects in main-client device px; `msElapsedAtSend` back-dates
    // the host clock to the CSS origin across this IPC hop. `easing` is ignored
    // here — the host hardcodes the matching CSS `ease` cubic-bezier and the web
    // only ever sends "ease"; the field stays in the schema for forward-compat.
    if (kind == "animate-scene-rect")
    {
        const json from = params.value("from", json::object());
        const json to   = params.value("to",   json::object());
        m_layout.StartSceneAnim(
            from.value("x", 0), from.value("y", 0), from.value("w", 0), from.value("h", 0),
            to.value("x", 0),   to.value("y", 0),   to.value("w", 0),   to.value("h", 0),
            params.value("durationMs", 0.0),
            params.value("msElapsedAtSend", 0.0));
        sendOk(json::object());
        return res;
    }

    // -------- host/backing-color --------
    // (session 3): recolour the DComp composition backing to the
    // app-shell --bg so every transparent DOM region outside the scene
    // rect (panel gaps, splitter seams, rounded-corner wedges) blends
    // into the shell instead of showing the black host backing. `color`
    // is a CSS string from getComputedStyle ("#rrggbb" / "rgb(r,g,b)").
    // Unparseable strings answer ok:false and leave the backing as-is —
    // never throw into the dispatch path. No-op in legacy (arch-A) mode
    // where LayoutBroker has no DComp compositor attached.
    if (kind == "host/backing-color")
    {
        const std::string colorStr = params.value("color", std::string{});
        COLORREF c = 0;
        if (!ParseCssColorToColorRef(colorStr, c))
        {
            sendErr("host/backing-color: unparseable color '" + colorStr + "'");
            return res;
        }
        m_layout.SetBackingColor(c);

        // Theme the native title bar to match the app shell. The backing
        // colour IS the resolved `--bg`, pushed on mount + every theme
        // toggle, so deriving the caption's dark-mode from its luminance
        // makes the Win32 caption follow the in-app theme for free (no new
        // bridge surface). Perceived luminance (Rec.601); < 128 ⇒ dark.
        if (m_hostHwnd)
        {
            const int luma = (GetRValue(c) * 299 + GetGValue(c) * 587 +
                              GetBValue(c) * 114) / 1000;
            BOOL dark = (luma < 128) ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hostHwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &dark, sizeof(dark));
        }

        printf("[backing] color '%s' -> RGB(%u,%u,%u)\n",
               colorStr.c_str(),
               GetRValue(c), GetGValue(c), GetBValue(c));
        fflush(stdout);
        sendOk(json::object());
        return res;
    }

    // -------- viewport/occlude --------
    // Register/clear a chrome-rectangle that overlaps the viewport
    // popup.: LayoutBroker translates the main-client rect to
    // popup-client coords and forwards to AlphaCompositor, which
    // per-frame stamps a smoothstep-feathered alpha hole into the
    // layered popup's DIB before UpdateLayeredWindow. Chrome HTML
    // (menus, panels) underneath shows through with soft edges.
    if (kind == "viewport/occlude")
    {
        const std::string id = params.value("id", std::string{});
        if (id.empty())
        {
            sendErr("viewport/occlude requires a non-empty id");
            return res;
        }
        if (params.contains("rect") && params["rect"].is_object())
        {
            const auto& r = params["rect"];
            int x = r.value("x", 0);
            int y = r.value("y", 0);
            int w = r.value("w", 0);
            int h = r.value("h", 0);
            int feather = params.value("feather", 0);
            printf("[Occlude] SET id=%s rect=(%d,%d,%d,%d) feather=%d\n",
                   id.c_str(), x, y, w, h, feather); fflush(stdout);
            m_layout.SetOcclusion(id, x, y, w, h, feather);
        }
        else
        {
            printf("[Occlude] CLEAR id=%s\n", id.c_str()); fflush(stdout);
            // rect=null or missing → remove the occlusion.
            m_layout.RemoveOcclusion(id);
        }
        sendOk(json::object());
        return res;
    }

    // -------- viewport/capture-snapshot --------
    // B1.3.1.1: React's Modal primitive calls this on open to grab a
    // frozen image of the engine viewport. It then renders the PNG as
    // an <img> portaled into the WebView2 viewport DOM and full-occludes
    // the engine popup — so Dialog.Overlay's `backdrop-blur-sm` can
    // blur engine + panels uniformly without any popup-boundary seam.
    // The compositor caches the most-recent pre-stamp frame, so the
    // capture sees the raw engine output without chrome cut-outs and
    // without any modal-mask dim that might already be active.
    //
    // Returns `{ imageBase64, w, h }` — base64 of a JPEG (the
    // backdrop is shown blurred, so lossy is invisible and far cheaper to
    // encode + transmit than PNG). When the compositor has no frame yet
    // (engine never composited, device just reset), returns an empty string
    // + zero dims so the React side can short-circuit its <img> render.
    if (kind == "viewport/capture-snapshot")
    {
        std::string imageBase64;
        int w = 0;
        int h = 0;
        if (m_layout.CaptureSnapshotPng(imageBase64, w, h))
        {
            sendOk(json{{"imageBase64", std::move(imageBase64)}, {"w", w}, {"h", h}});
        }
        else
        {
            sendOk(json{{"imageBase64", ""}, {"w", 0}, {"h", 0}});
        }
        return res;
    }

    // -------- viewport/input (Phase 2) --------
    //
    // DOM mouse/wheel/key events on the in-DOM <canvas> arrive here
    // and are forwarded to InputDispatcher, which PostMessages the
    // synthesized Win32 message to the (hidden) viewport popup HWND.
    // When InputDispatcher is null (legacy-popup mode) the request is
    // a silent no-op — the renderer shouldn't be dispatching these at
    // all in that mode, but ack rather than error so a stray event
    // doesn't surface as a noisy reject.
    if (kind == "viewport/input")
    {
        if (m_input) m_input->Dispatch(params);
        sendOk(json::object());
        return res;
    }

    // -------- register-accelerators --------
    if (kind == "register-accelerators")
    {
        auto combos = params.value("combos", std::vector<std::string>{});
        m_accel.RegisterCombos(combos);
        fprintf(stderr, "[host] AcceleratorBridge registered %zu combo(s)\n", combos.size());
        sendOk(json::object());
        return res;
    }

    // -------- app/quit -----------------------------------------------
    //
    // (Group D): React File → Exit dispatches this after the
    // dirty-prompt clears. PostMessage WM_CLOSE so the existing
    // DefWindowProc → DestroyWindow → WM_DESTROY shutdown chain
    // (compositor + engine teardown, WM_QUIT post) runs unchanged.
    // Using PostMessage (not SendMessage) so the response envelope
    // gets emitted before the message pump processes WM_CLOSE.
    if (kind == "app/quit")
    {
        sendOk(json::object());
        if (m_hostHwnd != nullptr)
        {
            PostMessage(m_hostHwnd, WM_CLOSE, 0, 0);
        }
        return res;
    }

    // -------- mods/list, mods/select, mods/refresh (D6) --------
    //
    // Three thin wrappers around ModManager. ModManager owns the
    // canonical state (mods vector + selectedModPath) and the
    // side-effect chain on selection (FileManager swap, registry
    // persist, palette swap, thumbnail cache clear, engine shader/
    // texture reload). The dispatcher's job is JSON in / JSON out plus
    // a single engine/state/changed emit on `select` so subscribed
    // React components see the new activeModPath without a separate
    // request.
    //
    // Helper closure for serialising a mods/list payload (used by both
    // mods/list and mods/refresh — same response shape).
    auto buildModsListPayload = [this]() -> json {
        json arr = json::array();
        const auto& mods = m_modManager->GetMods();
        for (const auto& m : mods)
        {
            arr.push_back(json{
                {"path",       WideToUtf8(m.path)},
                {"folderName", WideToUtf8(m.folderName)},
                {"nickname",   WideToUtf8(m.nickname)},
                {"isFoC",      m.isFoC},
            });
        }
        const std::wstring& sel = m_modManager->GetSelectedModPath();
        return json{
            {"mods",       arr},
            {"activePath", sel.empty() ? json(nullptr) : json(WideToUtf8(sel))},
        };
    };

    if (kind == "mods/list")
    {
        if (!m_modManager)
        {
            sendOk(json{
                {"mods", json::array()},
                {"activePath", json(nullptr)},
            });
            return res;
        }
        sendOk(buildModsListPayload());
        return res;
    }

    if (kind == "mods/refresh")
    {
        if (!m_modManager)
        {
            sendOk(json{
                {"mods", json::array()},
                {"activePath", json(nullptr)},
            });
            return res;
        }
        m_modManager->DiscoverMods();
        // ModManager keeps selectedModPath as-is on refresh; if the
        // path no longer exists on disk the React UI will see a
        // "ghost" selection until the user picks something else. This
        // matches the legacy WM_COMMAND ID_MOD_REFRESH branch in
        // main.cpp which has the same behaviour for symmetry.
        sendOk(buildModsListPayload());
        return res;
    }

    if (kind == "mods/select")
    {
        if (!m_modManager)
        {
            sendOk(json{{"ok", false}, {"error", "ModManager not bound"}});
            return res;
        }
        // params.path is `string | null` — JSON null means Unmodded.
        // Treat missing / non-string as Unmodded too (defensive).
        std::wstring path;
        auto pit = params.find("path");
        if (pit != params.end() && pit->is_string())
        {
            path = Utf8ToWide(pit->get<std::string>());
        }
        bool ok = m_modManager->SelectMod(path);
        // Drop cached palette thumbnails so a new mod's same-named textures
        // (e.g. p_smoke_atlas.dds) don't render the previous mod's art.
        // SelectMod already repointed Store::SetActiveMod + the legacy popup
        // cache; this clears the new-UI bridge cache (todo.md Risk 1).
        TexturePalette::ClearBridgeThumbCache();
        // Whether shader-reload failed or not, the FileManager + registry
        // + palette updates have rolled forward — broadcast the new
        // active path so the menu re-ticks even if shaders complain.
        EmitEngineStateChanged();
        const std::wstring& sel = m_modManager->GetSelectedModPath();
        sendOk(json{
            {"ok",         ok},
            {"activePath", sel.empty() ? json(nullptr) : json(WideToUtf8(sel))},
        });
        return res;
    }

    // -------- engine/state/snapshot --------
    if (kind == "engine/state/snapshot")
    {
        if (!requireEngine("snapshot")) return res;
        // Spawner field: prefer the live driver config (
        // host-state plumbing), fall back to the JSON cache from
        // Batch 4 when no driver is bound (e.g. unit tests, partial
        // wiring during construction).
        json spawnerJson = m_spawnerDriver
            ? SpawnerConfigToJson(m_spawnerDriver->GetConfig())
            : m_spawnerConfig;
        const std::wstring& activeModPath = m_modManager ? m_modManager->GetSelectedModPath() : std::wstring();
        const bool leaveParticles = (m_pParticleSystem != nullptr && *m_pParticleSystem)
            ? (*m_pParticleSystem)->getLeaveParticles()
            : true;
        const bool canUndo = ComputeCanUndo();
        const bool canRedo = m_undo ? m_undo->CanRedo() : false;
        sendOk(BuildEngineStateSnapshot(m_engine, m_currentFilePath, m_dirty, spawnerJson, m_selectedEmitterId, activeModPath, leaveParticles, canUndo, canRedo));
        return res;
    }

    // -------- engine/set/* (17 handlers) --------
    if (kind == "engine/set/ground")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetGround(params.value("enabled", false));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/ground-z")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetGroundZ(params.value("z", 0.0f));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/ground-texture")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetGroundTexture(params.value("slot", 0));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/ground-solid-color")
    {
        if (!requireEngine(kind.c_str())) return res;
        unsigned int rgb = params.value("rgb", 0u);
        m_engine->SetGroundSolidColor(static_cast<COLORREF>(rgb));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/ground-slot-custom-path")
    {
        if (!requireEngine(kind.c_str())) return res;
        int slot = params.value("slot", -1);
        std::string p = params.value("path", std::string{});
        m_engine->SetGroundSlotCustomPath(slot, Utf8ToWide(p));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/skydome-slot")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetSkydomeSlot(params.value("slot", 0));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/skydome-custom-path")
    {
        if (!requireEngine(kind.c_str())) return res;
        int slot = params.value("slot", -1);
        std::string p = params.value("path", std::string{});
        m_engine->SetSkydomeCustomPath(slot, Utf8ToWide(p));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/background")
    {
        if (!requireEngine(kind.c_str())) return res;
        unsigned int rgb = params.value("rgb", 0u);
        m_engine->SetBackground(static_cast<COLORREF>(rgb));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/bloom")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetBloom(params.value("enabled", false));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/bloom-strength")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetBloomStrength(params.value("v", 0.0f));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/bloom-cutoff")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetBloomCutoff(params.value("v", 0.0f));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/bloom-size")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetBloomSize(params.value("v", 0.0f));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    // Task 2.7 — leave particles after instance death. Persisted with
    // the ParticleSystem (chunk-serialised at [ParticleSystem.cpp:948])
    // so dirty must flip. Engine::KillParticleSystem honors the flag at
    // [src/engine.cpp:197].
    if (kind == "engine/set/leave-particles")
    {
        bool enabled = params.value("enabled", true);
        if (m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            (*m_pParticleSystem)->setLeaveParticles(enabled);
            sendOk(json::object());
            markDirty();
            EmitEngineStateChanged();
        }
        else
        {
            sendOk(json{{"ok", false}, {"error", "no particle system bound"}});
        }
        return res;
    }
    if (kind == "engine/set/heat-debug")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetHeatDebug(params.value("enabled", false));
        sendOk(json::object());
        //: heat-debug is a view-only debug overlay. Don't mark dirty.
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/camera")
    {
        if (!requireEngine(kind.c_str())) return res;
        Engine::Camera cam;
        cam.Position = JsonToVec3(params.value("position", json::array()));
        cam.Target   = JsonToVec3(params.value("target",   json::array()));
        cam.Up       = JsonToVec3(params.value("up",       json::array()));
        m_engine->SetCamera(cam);
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/light")
    {
        if (!requireEngine(kind.c_str())) return res;
        std::string which = params.value("which", std::string{"sun"});
        Engine::Light l;
        l.Diffuse   = JsonToVec4(params.value("diffuse",   json::array()));
        l.Specular  = JsonToVec4(params.value("specular",  json::array()));
        l.Position  = JsonToVec4(params.value("position",  json::array()));
        l.Direction = JsonToVec4(params.value("direction", json::array()));
        m_engine->SetLight(ParseLightWhich(which), l);
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/ambient")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetAmbient(JsonToVec4(params.value("color", json::array())));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/shadow")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetShadow(JsonToVec4(params.value("color", json::array())));
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    // View state (preview clock). SetPreviewPaused is a free function in
    // engine.h — sibling to IsPreviewPaused/StepPreviewFrames. Doesn't
    // touch Engine state, but the snapshot reads paused so a broadcast
    // keeps any subscriber's mirror in sync.
    if (kind == "engine/set/paused")
    {
        SetPreviewPaused(params.value("paused", false));
        sendOk(json::object());
        //: paused is a view-only toggle (preview clock). Don't mark dirty.
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/set/overload-guard")
    {
        // [guard-config] View-only preview setting (like engine/set/paused):
        // never marks the document dirty. The engine clamps maxParticles
        // defensively; we cache pre-clamp intent for SetEngine reapply
        // (the engine re-clamps on every apply, so the cache needs no
        // clamping of its own).
        const bool enabled      = params.value("enabled", true);
        const int  maxParticles = params.value("maxParticles", 15'000);
        m_overloadGuardCached       = true;
        m_overloadGuardEnabled      = enabled;
        m_overloadGuardMaxParticles = maxParticles;
        if (m_engine) m_engine->SetOverloadGuard(enabled, maxParticles);
        sendOk(json::object());
        return res;
    }
    // -------- engine/set/estimated-load (hard-guard) -----------------
    // Web-computed estimate of alive particles per placed instance
    // (chain-load.ts owns the formula — see the hard-guard spec). Cached
    // and reapplied on SetEngine like the guard config.
    if (kind == "engine/set/estimated-load")
    {
        double perInstance = params.value("perInstance", 0.0);
        if (perInstance < 0.0) perInstance = 0.0;
        m_estimatedLoadCached       = true;
        m_estimatedLoadPerInstance  = perInstance;
        if (m_engine) m_engine->SetEstimatedLoad(perInstance);
        sendOk(json::object());
        return res;
    }
    // T9] stats/set-frozen — test-only knob that suppresses
    // the 4 Hz stats/tick emission AND tells React's StatusBar to
    // clear its local state. Used by a11y spec beforeEach to bring
    // the StatusBar to a deterministic placeholder render before
    // capturing UIA goldens. Default-true for ergonomic test calls;
    // pass {frozen: false} to resume normal emissions.
    if (kind == "stats/set-frozen")
    {
        const bool frozen = params.value("frozen", true);
        m_statsFrozen = frozen;
        if (m_emit)
        {
            json env = {
                {"type",    "evt"},
                {"kind",    "stats/frozen-changed"},
                {"payload", {{"frozen", frozen}}},
            };
            m_emit(env.dump());
        }
        sendOk(json::object());
        return res;
    }

    // -------- engine/action/reset-view-settings ----------------------
    //
    // (Group D): cascade reset for the View → Reset View Settings
    // menu. Mirrors legacy main.cpp:1733-1808: pushes engine defaults
    // for background, ground (visibility + Z + texture), bloom (off +
    // canonical strength/cutoff/size), and skydome (Off slot). Lighting
    // reset rides with D4 (separate handler around Force Align).
    //
    // Defaults match the Engine constructor (engine.cpp:1690-1715) —
    // kept in sync by hand because there's only one canonical value
    // each. Editor state (current path, dirty bit, selection) is left
    // alone since it isn't a "view setting."
    if (kind == "engine/action/reset-view-settings")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->SetBackground(RGB(0x14, 0x08, 0x34));
        m_engine->SetGround(true);
        m_engine->SetGroundZ(0.0f);
        m_engine->SetGroundTexture(0);
        m_engine->SetBloom(false);
        m_engine->SetBloomStrength(0.00f);
        m_engine->SetBloomCutoff (0.90f);
        m_engine->SetBloomSize   (0.10f);
        m_engine->SetSkydomeSlot(0);  // "Off"
        sendOk(json::object());
        // No markDirty — these are view settings, not particle-system
        // mutations. The user-visible cascade is communicated by a
        // single state-changed broadcast.
        EmitEngineStateChanged();
        return res;
    }

    // -------- engine/action/* --------
    if (kind == "engine/action/clear")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->Clear();
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/action/reload-shaders")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->ReloadShaders();
        sendOk(json::object());
        // No dirty: reload-shaders re-reads disk; user state is unchanged.
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/action/reload-textures")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->ReloadTextures();
        sendOk(json::object());
        // No dirty: reload-textures re-reads disk; user state is unchanged.
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "engine/action/on-particle-system-changed")
    {
        if (!requireEngine(kind.c_str())) return res;
        m_engine->OnParticleSystemChanged(params.value("track", 0));
        sendOk(json::object());
        // No engine/state/changed broadcast — engine re-renders next frame.
        return res;
    }
    // Advance the preview clock by N frames. The free function is a
    // no-op when not paused, so the React Toolbar disables the button in
    // that state; no need to guard it here. Don't broadcast a state
    // change — the action emits zero or more state ticks via the normal
    // render loop, and an immediate broadcast would misleadingly suggest
    // a sync-time mutation.
    if (kind == "engine/action/step-frames")
    {
        StepPreviewFrames(params.value("frames", 1));
        sendOk(json::object());
        return res;
    }
    // Rescale the active particle system by a duration / size percentage.
    // host-state plumbing: real implementation. Iterates over every
    // emitter in the live ParticleSystem and applies the helper from
    // src/Rescale.cpp. UndoStack capture is best-effort — Phase 3 emitter
    // work hasn't seeded the stack baseline yet (see undo/perform notes
    // below), so a Capture here lands in an empty stack with no
    // pre-existing redo to clear; that's correct, it just means undo
    // remains a no-op until the broader capture wiring lands.
    if (kind == "engine/action/rescale-system")
    {
        float durPct  = params.value("durationScalePercent", 100.0f);
        float sizePct = params.value("sizeScalePercent",     100.0f);
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        ParticleSystem* sys = m_pParticleSystem->get();
        const float timeScale = durPct  / 100.0f;
        const float sizeScale = sizePct / 100.0f;
        // Walk every emitter, not just roots — DoRescaleEmitter only
        // touches per-emitter scalar fields and doesn't recurse, so we
        // need to iterate the flat list. Mirrors the loop at
        // src/Rescale.cpp:181 used by RescaleParticleSystem.
        auto& emitters = sys->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr)
                DoRescaleEmitter(emitters[i], timeScale, sizeScale);
        }
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        // Emitter parameters changed → notify the React tree so the
        // sidebar re-fetches via emitters/list.
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- engine/query/* --------
    if (kind == "engine/query/ground-slot-empty")
    {
        if (!requireEngine(kind.c_str())) return res;
        sendOk(json(m_engine->IsGroundSlotEmpty(params.value("slot", -1))));
        return res;
    }
    if (kind == "engine/query/skydome-slot-empty")
    {
        if (!requireEngine(kind.c_str())) return res;
        sendOk(json(m_engine->IsSkydomeSlotEmpty(params.value("slot", -1))));
        return res;
    }
    if (kind == "engine/query/bloom-available")
    {
        if (!requireEngine(kind.c_str())) return res;
        sendOk(json(m_engine->IsBloomAvailable()));
        return res;
    }

    // -------- settings/lighting (get) --------------------------------
    //
    // Cross-mode read of the RAW lighting split the LightingPanel seeds
    // its displayed controls from — intensity/colour kept SEPARATE (the
    // engine snapshot only carries the lossy folded Vec4). Mirrors legacy
    // LightingDlgProc's registry reads (src/main.cpp:6479) field-for-field;
    // same value names/types (floats REG_BINARY, colours + the flag
    // REG_DWORD). Colours go on the wire as packed COLORREF ints (`Color`).
    //
    // Gated under --test-host (returns the canonical defaults, NOT the live
    // registry) so the dialog-lighting a11y golden stays deterministic —
    // UNLESS ALO_SETTINGS_LIVE lifts the gate (the CDP test seam).
    if (kind == "settings/lighting")
    {
        // Canonical defaults (src/main.cpp:6180-6195).
        float sunI = 0.50f, sunZ = 0.0f, sunT = 45.0f;
        DWORD sunDiff = RGB(180, 180, 190), sunSpec = RGB(190, 190, 200);
        DWORD ambient = RGB(40, 40, 50), shadow = RGB(100, 100, 110);
        float f1I = 0.50f, f1Z = 120.0f, f1T = -10.0f; DWORD f1Diff = RGB(60, 80, 160);
        float f2I = 0.50f, f2Z = 210.0f, f2T = -10.0f; DWORD f2Diff = RGB(60, 80, 160);
        bool  forceAlign = true;  // kLightForceAlignDefault

        const bool gated = m_testHost && !m_settingsLive;
        if (!gated)
        {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                              KEY_READ, &hKey) == ERROR_SUCCESS)
            {
                auto readF = [&](const wchar_t* name, float& out) {
                    float v = 0.0f; DWORD t = 0, s = sizeof(v);
                    if (RegQueryValueExW(hKey, name, nullptr, &t,
                                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
                        && t == REG_BINARY && s == sizeof(v) && v == v && (v - v) == 0.0f)
                        out = v;
                };
                auto readDw = [&](const wchar_t* name, DWORD& out) {
                    DWORD v = 0, t = 0, s = sizeof(v);
                    if (RegQueryValueExW(hKey, name, nullptr, &t,
                                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
                        && t == REG_DWORD)
                        out = v;
                };
                readF(L"LightSunIntensity", sunI);
                readF(L"LightSunZAngle",    sunZ);
                readF(L"LightSunTilt",      sunT);
                readDw(L"LightSunDiffuseColor",  sunDiff);
                readDw(L"LightSunSpecularColor", sunSpec);
                readDw(L"LightSunAmbientColor",  ambient);
                readDw(L"LightSunShadowColor",   shadow);
                readF(L"LightFill1Intensity", f1I);
                readF(L"LightFill1ZAngle",    f1Z);
                readF(L"LightFill1Tilt",      f1T);
                readDw(L"LightFill1DiffuseColor", f1Diff);
                readF(L"LightFill2Intensity", f2I);
                readF(L"LightFill2ZAngle",    f2Z);
                readF(L"LightFill2Tilt",      f2T);
                readDw(L"LightFill2DiffuseColor", f2Diff);
                DWORD fa = 1;
                readDw(L"LightingForceFillAlignment", fa);
                forceAlign = (fa != 0);
                RegCloseKey(hKey);
            }
        }

        auto lightJson = [](float intensity, float az, float alt,
                            DWORD diffuse, DWORD specular) {
            return json{
                {"intensity", intensity}, {"az", az}, {"alt", alt},
                {"diffuse",  static_cast<int>(diffuse)},
                {"specular", static_cast<int>(specular)},
            };
        };
        sendOk(json{
            {"sun",   lightJson(sunI, sunZ, sunT, sunDiff, sunSpec)},
            {"fill1", lightJson(f1I, f1Z, f1T, f1Diff, 0)},
            {"fill2", lightJson(f2I, f2Z, f2T, f2Diff, 0)},
            {"ambient",    static_cast<int>(ambient)},
            {"shadow",     static_cast<int>(shadow)},
            {"forceAlign", forceAlign},
        });
        return res;
    }

    // -------- settings/lighting-force-align/set ----------------------
    //
    // Cross-mode write of the `LightingForceFillAlignment` REG_DWORD
    // (WriteLightingBool, src/main.cpp:6328) so a toggle in the new UI is
    // seen by legacy. No-op under --test-host (so the a11y harness never
    // mutates the dev box's registry) UNLESS ALO_SETTINGS_LIVE lifts the
    // gate for the CDP test seam.
    if (kind == "settings/lighting-force-align/set")
    {
        const bool enabled = params.value("enabled", true);
        const bool gated = m_testHost && !m_settingsLive;
        if (!gated)
        {
            HKEY hKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                &hKey, nullptr) == ERROR_SUCCESS)
            {
                DWORD v = enabled ? 1 : 0;
                RegSetValueExW(hKey, L"LightingForceFillAlignment", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&v), sizeof(v));
                RegCloseKey(hKey);
            }
        }
        sendOk(json::object());
        return res;
    }

    // -------- undo/perform --------
    //
    // New-UI mutation handlers call captureUndo() PRE-mutation, so the
    // snapshot at entries[cursor-1] represents the state BEFORE the
    // most recent mutation — not the current live state. UndoStack's
    // Undo() is built around the legacy POST-mutation convention
    // (cursor-- ; return entries[cursor-1] = the previous live state).
    // To reconcile: at the head of history (cursor == size) AND only
    // when live is genuinely skewed ahead of the tip (IsLiveAhead),
    // snapshot the current live state once before stepping back. That
    // auto-capped entry IS the live state, so Undo's math now returns
    // the PRE-mutation snapshot — exactly what Ctrl+Z should restore.
    // The IsLiveAhead() guard matters: cursor == size is ALSO true right
    // after a Redo() (redo to the tip leaves cursor == size), but there
    // live is already IN SYNC with entries[cursor-1]. Auto-capping in
    // that case duplicated the tip and the following Undo() returned the
    // duplicate, silently swallowing the undo (undo→redo→undo lost the
    // second undo). Skip the auto-cap mid-redo-branch (the post-state is
    // already at entries[cursor]) or on an empty stack (CanUndo would
    // be false anyway). See tasks/todo.md §3 for the full trace.
    if (kind == "undo/perform")
    {
        std::string dir = params.value("direction", std::string("undo"));
        bool applied = false;
        if (m_undo && m_pParticleSystem && *m_pParticleSystem)
        {
            if (dir == "undo"
                && m_undo->Cursor() == m_undo->Depth()
                && m_undo->Depth() > 0
                && m_undo->IsLiveAhead())
            {
                const ParticleSystem* sys = m_pParticleSystem->get();
                size_t selIdxNow = SIZE_MAX;
                if (m_selectedEmitterId >= 0
                    && static_cast<size_t>(m_selectedEmitterId)
                           < sys->getEmitters().size())
                {
                    selIdxNow = static_cast<size_t>(m_selectedEmitterId);
                }
                m_undo->Capture(*sys, selIdxNow, 0);
            }

            const std::vector<char>* snap = nullptr;
            size_t selIdx = SIZE_MAX;
            if (dir == "undo" && m_undo->CanUndo())
            {
                applied = m_undo->Undo(&snap, &selIdx);
            }
            else if (dir == "redo" && m_undo->CanRedo())
            {
                applied = m_undo->Redo(&snap, &selIdx);
            }

            if (applied && snap != nullptr)
            {
                ApplyUndoSnapshot(*snap, selIdx);
            }
        }
        sendOk(json{{"applied", applied}});
        if (applied)
        {
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return res;
    }

    // -------- file/* (Phase 3 Screen 8 Batch 3) ----------------------
    //
    // The new-UI host doesn't yet own a ParticleSystem* (emitter / file-
    // load wiring is later-batch work in Phase 3+). So the file handlers
    // perform the *editor-level* side of the operation — currentFilePath
    // tracking, dirty flag, recent-files registry, native picker
    // round-trip — but skip the engine-level ParticleSystem read/write
    // until that pointer exists. Same forward-compatible no-op pattern
    // as engine/action/rescale-system in Batch 1. Legacy `DoNewFile` /
    // `DoOpenFile` / `DoSaveFile` at [src/main.cpp:1289-1393] continue
    // to serve `--legacy-ui` unchanged — they're coupled to
    // `APPLICATION_INFO*` which doesn't exist in --new-ui mode, so
    // calling them directly from here isn't possible.

    // -------- file/new --------
    //: replace the host-owned ParticleSystem with a fresh empty
    // one + one root emitter (mirrors legacy DoNewFile at
    // src/main.cpp:1289). Clear editor path / dirty.
    if (kind == "file/new")
    {
        // shift-click-to-spawn: if the user is mid-Shift-hold when
        // they hit file/new, kill the cursor-bound instance before
        // dropping the ParticleSystem it was spawned from. Mirrors the
        // legacy DoNewFile teardown sequence for `attachedParticleSystem`
        // at src/main.cpp:1289-1305.
        if (m_ppAttachedParticleSystem && *m_ppAttachedParticleSystem && m_engine)
        {
            fprintf(stderr, "[ArchC-kill] file/new killing attached=%p\n",
                    static_cast<void*>(*m_ppAttachedParticleSystem));
            m_engine->KillParticleSystem(*m_ppAttachedParticleSystem);
            *m_ppAttachedParticleSystem = nullptr;
        }
        if (m_pParticleSystem)
        {
            *m_pParticleSystem = std::make_unique<ParticleSystem>();
            (*m_pParticleSystem)->addRootEmitter();
        }
        // render loop: notify Engine that the ParticleSystem pointer
        // it knows about is now stale. Mirrors legacy DoNewFile at
        // src/main.cpp:1207 (Clear() then OnParticleSystemChanged(-1))
        // so the engine drops cached instances + per-emitter state.
        if (m_engine)
        {
            m_engine->Clear();
            m_engine->OnParticleSystemChanged(-1);
        }
        // Reset undo stack — prior session's entries reference the
        // now-freed ParticleSystem and would crash a future restore.
        // Mirrors legacy LoadFile at src/main.cpp:1103.
        if (m_undo) m_undo->Clear();
        // Refresh the "saved" reference snapshot — the fresh-with-
        // one-root state is the new dirty-bit baseline. Mutate + Ctrl+Z
        // back here clears the title-bar asterisk in
        // ApplyUndoSnapshot's content-compare.
        if (m_pParticleSystem && *m_pParticleSystem)
            m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        m_currentFilePath.clear();
        sendOk(json::object());
        SetDirty(false);
        EmitEngineStateChanged();
        // B1.3.1 polish round 3: React's EmitterTree subscribes to
        // emitters/tree/changed; without this emit the tree stays on
        // its pre-file/new state even after the ParticleSystem has
        // been swapped under it.
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- textures/browse --------
    //
    // Host-side native file dialog for an emitter's color/bump texture.
    // Opens in the active mod's texture folder (Data\Art\Textures, with
    // fallbacks), filtered to *.tga;*.dds, and returns the chosen file's
    // basename (or "" if cancelled). The React side commits the result
    // through emitters/set-properties — same path the text input uses.
    // Like file/open, GetOpenFileNameW runs a nested message loop while
    // the JS caller awaits. Mirrors legacy LoadTexture
    // (src/UI/Emitter.cpp:83). feature-parity A]
    if (kind == "textures/browse")
    {
        std::string slot = "color";
        if (auto sit = params.find("slot"); sit != params.end() && sit->is_string())
            slot = sit->get<std::string>();

        // Initial dir: active mod's texture folder → mod root → none
        // (dialog opens at its default if all are unavailable).
        std::wstring initialDir;
        if (m_modManager)
        {
            const std::wstring& mod = m_modManager->GetSelectedModPath();
            if (!mod.empty())
            {
                auto isDir = [](const std::wstring& p) -> bool {
                    DWORD a = GetFileAttributesW(p.c_str());
                    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                };
                const std::wstring texDir = mod + L"\\Data\\Art\\Textures";
                if (isDir(texDir))   initialDir = texDir;
                else if (isDir(mod)) initialDir = mod;
            }
        }

        wchar_t buf[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = m_hostHwnd;
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = MAX_PATH;
        ofn.lpstrFilter     = L"Texture Files (*.tga;*.dds)\0*.tga;*.dds\0All Files (*.*)\0*.*\0\0";
        ofn.lpstrTitle      = (slot == "bump") ? L"Select bump texture" : L"Select color texture";
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&ofn))
        {
            // Cancelled / dialog failure → empty filename (no-op on the
            // React side, which only commits a non-empty string).
            sendOk(json{{"filename", ""}});
            return res;
        }

        // Store only the basename — matches the colorTexture /
        // normalTexture field convention (legacy strips path via strrchr).
        const std::wstring full = buf;
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        sendOk(json{{"filename", WideToUtf8(base)}});
        return res;
    }

    // -------- textures/palette/* --------
    //
    // Expose the per-mod frequently-used texture palette
    // (TexturePalette::Store, already repointed at the active mod by
    // ModManager::SelectMod) to the new UI. sub-feature B]
    if (kind == "textures/palette/list")
    {
        std::string slot = "color";
        if (auto it = params.find("slot"); it != params.end() && it->is_string())
            slot = it->get<std::string>();
        const TexturePalette::SlotMask mask =
            (slot == "bump") ? TexturePalette::SLOT_BUMP : TexturePalette::SLOT_COLOR;

        auto& store = TexturePalette::Store::Instance();
        store.SetActiveFilter(mask);  // slot-aware default (persists per-mod)

        auto toArray = [](const std::vector<TexturePalette::Entry>& entries) {
            json arr = json::array();
            for (const TexturePalette::Entry& e : entries)
                arr.push_back(json{
                    {"filename", WideToUtf8(e.filename)},
                    {"pinned",   e.isPinned},
                    {"slotMask", (int)e.slotMask},
                });
            return arr;
        };

        sendOk(json{
            {"hasMod",  store.HasActiveMod()},
            {"filter",  slot},
            {"pins",    toArray(store.Pins(mask))},
            {"recents", toArray(store.Recents(mask))},
        });
        return res;
    }

    if (kind == "textures/palette/thumbnail")
    {
        std::string filename;
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();

        IDirect3DDevice9* dev = m_engine ? m_engine->GetDevice() : nullptr;
        const TexturePalette::ThumbnailResult t = TexturePalette::GetThumbnail(
            Utf8ToWide(filename), m_fileManager, dev);
        const char* status =
            t.status == TexturePalette::ThumbStatus::Ok      ? "ok"      :
            t.status == TexturePalette::ThumbStatus::Missing ? "missing" : "broken";
        sendOk(json{
            {"dataUri", t.dataUri.empty() ? json(nullptr) : json(t.dataUri)},
            {"status",  status},
        });
        return res;
    }

    if (kind == "textures/palette/toggle-pin")
    {
        std::string filename;
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();

        const std::wstring wf = Utf8ToWide(filename);
        auto& store = TexturePalette::Store::Instance();
        if (!store.TogglePin(wf))
        {
            // The only user-reachable failure (entry exists + mod active) is
            // the pins-full cap; no-mod/malformed never happen from the UI.
            sendOk(json{{"ok", false}, {"reason", "pins-full"}});
            return res;
        }
        // Report the new pinned state (an entry can be pinned for either
        // slot, so check both filters).
        bool pinned = false;
        for (const TexturePalette::Entry& e : store.Pins(TexturePalette::SLOT_COLOR))
            if (e.filename == wf) { pinned = true; break; }
        if (!pinned)
            for (const TexturePalette::Entry& e : store.Pins(TexturePalette::SLOT_BUMP))
                if (e.filename == wf) { pinned = true; break; }
        sendOk(json{{"ok", true}, {"pinned", pinned}});
        return res;
    }

    if (kind == "textures/palette/touch-recent")
    {
        std::string filename;
        std::string slot = "color";
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();
        if (auto it = params.find("slot"); it != params.end() && it->is_string())
            slot = it->get<std::string>();
        const TexturePalette::SlotMask mask =
            (slot == "bump") ? TexturePalette::SLOT_BUMP : TexturePalette::SLOT_COLOR;

        TexturePalette::Store::Instance().TouchRecent(Utf8ToWide(filename), mask);
        sendOk(json{{"ok", true}});
        return res;
    }

    // -------- file/open --------
    //
    // Two modes:
    //   - If `params.path` is provided (Recent Files / drag-drop / Open
    //     dialog already resolved on the React side), use it directly.
    //   - Otherwise pop GetOpenFileNameW. The native dialog runs a
    //     nested message loop; host pump pauses for the dialog's
    //     lifetime, which is fine because the JS caller is awaiting.
    //
    // Both modes commit the path into m_currentFilePath, push to
    // recents, fire recent/changed + engine/state/changed, and clear
    // dirty. Actual ParticleSystem load is forward-deferred.
    if (kind == "file/open")
    {
        std::wstring path;
        if (auto pit = params.find("path"); pit != params.end() && pit->is_string())
        {
            path = Utf8ToWide(pit->get<std::string>());
        }
        // Resolve optional filter discriminator. Default "alo" keeps
        // File→Open / recents / drag-drop behaviour unchanged; the
        // picker panels pass "skydome" / "ground" so the dialog
        // defaults to the texture filter the user actually needs AND
        // the post-pick load path is skipped (textures aren't .alo
        // files).
        std::string filterId = "alo";
        if (auto fit = params.find("filter"); fit != params.end() && fit->is_string())
        {
            filterId = fit->get<std::string>();
        }

        if (path.empty())
        {
            const wchar_t* lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
            const wchar_t* lpstrTitle  = L"Open particle system";
            if (filterId == "skydome")
            {
                lpstrFilter = L"Texture Files (*.dds;*.tga)\0*.dds;*.tga\0All Files (*.*)\0*.*\0\0";
                lpstrTitle  = L"Open skydome texture";
            }
            else if (filterId == "ground")
            {
                lpstrFilter = L"Texture Files (*.dds;*.tga)\0*.dds;*.tga\0All Files (*.*)\0*.*\0\0";
                lpstrTitle  = L"Open ground texture";
            }

            // Default the "Open particle system" picker to the selected
            // mod's Models folder (where .alo models live), mirroring the
            // textures/browse default above. Gated on the .alo case
            // (filterId empty) so the skydome/ground TEXTURE variants are
            // NOT pointed at Models — they keep the dialog's default dir.
            // Covers BOTH File->Open and Import Emitters' Browse (both call
            // file/open with empty params). Fallback: mod root -> default.
            std::wstring initialDir;
            if (filterId.empty() && m_modManager)
            {
                const std::wstring& mod = m_modManager->GetSelectedModPath();
                if (!mod.empty())
                {
                    auto isDir = [](const std::wstring& p) -> bool {
                        DWORD a = GetFileAttributesW(p.c_str());
                        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    };
                    const std::wstring modelsDir = mod + L"\\Data\\Art\\Models";
                    if (isDir(modelsDir)) initialDir = modelsDir;
                    else if (isDir(mod))  initialDir = mod;
                }
            }

            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize     = sizeof(ofn);
            ofn.hwndOwner       = m_hostHwnd;
            ofn.lpstrFile       = buf;
            ofn.nMaxFile        = MAX_PATH;
            ofn.lpstrFilter     = lpstrFilter;
            ofn.lpstrTitle      = lpstrTitle;
            ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
            ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            if (!GetOpenFileNameW(&ofn))
            {
                // User cancelled / dialog failure.
                sendOk(json{{"ok", false}, {"error", "user-cancelled"}});
                return res;
            }
            path = buf;
        }

        // Texture-picker variants short-circuit here: just return the
        // path so the React side can route it through the appropriate
        // engine setter (engine/set/skydome-custom-path or
        // engine/set/ground-slot-custom-path). Don't touch
        // m_currentFilePath / recents / engine state — those belong to
        // the "open particle system" semantics of the .alo path below.
        if (filterId != "alo")
        {
            sendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
            return res;
        }

        //: actually load the .alo into the host-owned slot.
        std::string err;
        std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(path, &err);
        if (!loaded)
        {
            // Don't touch m_currentFilePath / recents on failure —
            // matches legacy LoadFile behaviour (history append only
            // happens after a successful parse).
            sendOk(json{{"ok", false}, {"error", err.empty() ? std::string("load failed") : err}});
            return res;
        }
        // shift-click-to-spawn: kill any cursor-bound instance
        // attached to the about-to-be-replaced ParticleSystem before
        // swapping. Same reasoning as the file/new branch above.
        if (m_ppAttachedParticleSystem && *m_ppAttachedParticleSystem && m_engine)
        {
            fprintf(stderr, "[ArchC-kill] file/open killing attached=%p\n",
                    static_cast<void*>(*m_ppAttachedParticleSystem));
            m_engine->KillParticleSystem(*m_ppAttachedParticleSystem);
            *m_ppAttachedParticleSystem = nullptr;
        }
        if (m_pParticleSystem)
        {
            *m_pParticleSystem = std::move(loaded);
        }
        // Load-time sweep: pre-.alo files may contain
        // single-member link groups. Sweep once after binding so the
        // data layer matches the render-layer filter from frame one.
        // Does NOT call markDirty() — the correction is a
        // normalization, not user-driven mutation; marking dirty
        // would force a save-prompt on every open of a legacy file
        // even when the user makes no further changes. Subsequent
        // mutations re-fire the sweep via the same helper.
        EnforceSingleMemberLinkGroups();
        // Reset undo stack — prior session's entries reference the
        // now-freed ParticleSystem. Mirrors legacy LoadFile at
        // src/main.cpp:1103.
        if (m_undo) m_undo->Clear();
        // Refresh the "saved" reference snapshot — the just-loaded
        // state IS the saved file's content. Captured AFTER the
        // sweep so legacy singleton-link-group .alo files don't show
        // as dirty when the user undoes back to "as loaded".
        if (m_pParticleSystem && *m_pParticleSystem)
            m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        // render loop: same notification sequence as file/new —
        // the engine's cached per-instance / per-emitter state is now
        // stale and must be cleared. Matches legacy DoOpenFile path
        // at src/main.cpp:1341.
        if (m_engine)
        {
            m_engine->Clear();
            m_engine->OnParticleSystemChanged(-1);
            // B1.3.1 polish round 3: legacy DoOpenFile relies on
            // first-render lazy texture binding via per-instance
            // construction; in --new-ui mode the host's WebView2
            // composition timing produces white-fallback particles
            // unless we explicitly invalidate the cache. ReloadTextures
            // is the same operation View → Reload Textures already
            // does on demand; calling it here makes file/open
            // self-sufficient.
            m_engine->ReloadTextures();
        }
        m_currentFilePath = path;
        m_recentFiles = WriteRecentFile(path);
        sendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        EmitRecentChanged();
        EmitEngineStateChanged();
        // B1.3.1 polish round 3: React's EmitterTree subscribes to
        // emitters/tree/changed; without this emit the tree stays on
        // the previous file's emitters even though the engine now
        // holds the new file's ParticleSystem.
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- file/save --------
    //
    // If `params.path` provided, use it. Else if m_currentFilePath is
    // set (named document), save there. Else pop GetSaveFileNameW.
    // Matches legacy `DoSaveFile(info, /*saveas=*/false)`.
    if (kind == "file/save")
    {
        std::wstring path;
        if (auto pit = params.find("path"); pit != params.end() && pit->is_string())
        {
            path = Utf8ToWide(pit->get<std::string>());
        }
        if (path.empty()) path = m_currentFilePath;
        if (path.empty())
        {
            // No remembered path → pop save-as picker.
            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = m_hostHwnd;
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
            ofn.lpstrDefExt = L"alo";
            ofn.lpstrTitle  = L"Save particle system";
            ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

            if (!GetSaveFileNameW(&ofn))
            {
                sendOk(json{{"ok", false}, {"error", "user-cancelled"}});
                return res;
            }
            path = buf;
        }

        //: actually write the host-owned ParticleSystem to disk.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return res;
        }
        std::string err;
        if (!SaveParticleSystem(m_pParticleSystem->get(), path, &err))
        {
            sendOk(json{{"ok", false}, {"error", err.empty() ? std::string("save failed") : err}});
            return res;
        }
        m_currentFilePath = path;
        m_recentFiles = WriteRecentFile(path);
        // Refresh the "saved" reference snapshot — what we just wrote
        // to disk IS the new saved state. ApplyUndoSnapshot uses this
        // to clear the title-bar asterisk when the user undoes back
        // to a content-equal state.
        m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        sendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        //: the work is now on disk — this session's autosave is
        // redundant. Delete it so a clean exit leaves no orphan to prompt
        // for. Further edits re-create it on the next tick. (Mirrors legacy
        // main.cpp DeleteOurSession-after-save.)
        Autosave::DeleteOurSession();
        EmitRecentChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- file/save-as --------
    //
    // ALWAYS pops GetSaveFileNameW. Matches legacy
    // `DoSaveFile(info, /*saveas=*/true)`. Same path-commit / recents /
    // dirty side effects as file/save.
    if (kind == "file/save-as")
    {
        wchar_t buf[MAX_PATH] = {};
        // Seed with the current filename so the dialog opens at the
        // existing path's directory — matches legacy save-as ergonomics.
        if (!m_currentFilePath.empty() &&
            m_currentFilePath.size() < MAX_PATH)
        {
            wcscpy_s(buf, MAX_PATH, m_currentFilePath.c_str());
        }
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = m_hostHwnd;
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
        ofn.lpstrDefExt = L"alo";
        ofn.lpstrTitle  = L"Save particle system as";
        ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&ofn))
        {
            sendOk(json{{"ok", false}, {"error", "user-cancelled"}});
            return res;
        }

        std::wstring path = buf;
        //: actually write to disk.
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return res;
        }
        std::string err;
        if (!SaveParticleSystem(m_pParticleSystem->get(), path, &err))
        {
            sendOk(json{{"ok", false}, {"error", err.empty() ? std::string("save failed") : err}});
            return res;
        }
        m_currentFilePath = path;
        m_recentFiles = WriteRecentFile(path);
        // Refresh the "saved" reference snapshot — see file/save above.
        m_savedSnapshot = UndoStack::Serialize(**m_pParticleSystem);
        sendOk(json{{"ok", true}, {"path", WideToUtf8(path)}});
        SetDirty(false);
        //: see file/save — autosave is redundant once saved to disk.
        Autosave::DeleteOurSession();
        EmitRecentChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- file/recent/list --------
    //
    // Re-reads the registry on every call. Cheap (≤ 9 entries) and
    // means the list stays in lockstep with legacy AppendHistory writes
    // that happen in --legacy-ui sessions.
    if (kind == "file/recent/list")
    {
        m_recentFiles = ReadRecentFiles();
        json paths = json::array();
        for (const auto& w : m_recentFiles) paths.push_back(WideToUtf8(w));
        sendOk(json{{"paths", paths}});
        return res;
    }

    // -------- autosave/check-recovery () --------
    //
    // React calls this once on mount. Scan %TEMP%\AloParticleEditor\ for an
    // orphaned autosave left by a crashed prior session and return it (or
    // null). Suppressed under --test-host (the harness must never get a
    // recovery prompt — it would pollute a11y captures, cf.) and when a
    // document is already loaded (a CLI file / any non-untitled state "wins"
    // over recovery, matching legacy main.cpp:7739). Stash the live
    // OrphanSession so autosave/recover can consume its temp paths w/o re-scan.
    if (kind == "autosave/check-recovery")
    {
        m_hasPendingOrphan = false;
        if (m_testHost || !m_currentFilePath.empty())
        {
            sendOk(json{{"orphan", nullptr}});
            return res;
        }
        Autosave::OrphanSession s;
        if (!Autosave::ScanForOrphan(&s))
        {
            sendOk(json{{"orphan", nullptr}});
            return res;
        }
        m_pendingOrphan    = s;
        m_hasPendingOrphan = true;
#ifndef NDEBUG
        fprintf(stderr, "[autosave] check-recovery: orphan found (recent=%d stable=%d)\n",
                s.recentPath.empty() ? 0 : 1, s.stablePath.empty() ? 0 : 1);
#endif

        // FILETIME (100-ns ticks since 1601) → Unix epoch ms for React.
        auto mtimeMs = [](const FILETIME& ft) -> json {
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            const unsigned long long EPOCH_DIFF_100NS = 116444736000000000ULL;
            if (u.QuadPart < EPOCH_DIFF_100NS) return json(nullptr);
            return json((u.QuadPart - EPOCH_DIFF_100NS) / 10000ULL);
        };
        json orphan = {
            {"originalFilename", WideToUtf8(s.originalFilename)},
            {"recentMtimeMs", s.recentPath.empty() ? json(nullptr) : mtimeMs(s.recentMtime)},
            {"stableMtimeMs", s.stablePath.empty() ? json(nullptr) : mtimeMs(s.stableMtime)},
        };
        sendOk(json{{"orphan", orphan}});
        return res;
    }

    // -------- autosave/recover () --------
    //
    // Apply the recovery choice from the React dialog. For recent/stable,
    // load the chosen temp .alo via the SAME swap+notify sequence file/open
    // uses (so the attached-cursor reseat runs), then present it AS the
    // original filename with dirty=true: an empty saved baseline keeps it
    // dirty until a real save (the temp content matches no on-disk file), and
    // the temp path never enters recents. For discard, leave the boot
    // document untouched. Either way DeleteOrphan consumes the session.
    if (kind == "autosave/recover")
    {
        if (!m_hasPendingOrphan)
        {
            sendOk(json::object());  // no prior check / already consumed
            return res;
        }
        const std::string choice = params.value("choice", std::string("discard"));
        const Autosave::OrphanSession s = m_pendingOrphan;
        m_hasPendingOrphan = false;
        m_pendingOrphan = Autosave::OrphanSession{};

        std::wstring restorePath;
        if (choice == "recent")      restorePath = s.recentPath;
        else if (choice == "stable") restorePath = s.stablePath;
#ifndef NDEBUG
        fprintf(stderr, "[autosave] recover: choice=%s restore=%d\n",
                choice.c_str(), restorePath.empty() ? 0 : 1);
#endif

        if (!restorePath.empty())
        {
            std::string err;
            std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(restorePath, &err);
            if (loaded)
            {
                if (m_ppAttachedParticleSystem && *m_ppAttachedParticleSystem && m_engine)
                {
                    m_engine->KillParticleSystem(*m_ppAttachedParticleSystem);
                    *m_ppAttachedParticleSystem = nullptr;
                }
                if (m_pParticleSystem) *m_pParticleSystem = std::move(loaded);
                EnforceSingleMemberLinkGroups();
                if (m_undo) m_undo->Clear();
                if (m_engine)
                {
                    m_engine->Clear();
                    m_engine->OnParticleSystemChanged(-1);
                    m_engine->ReloadTextures();
                }
                m_currentFilePath = s.originalFilename;  // "" → untitled
                m_savedSnapshot.clear();                 // stays dirty until saved
                SetDirty(true);
                EmitEngineStateChanged();
                EmitEmittersTreeChanged();
            }
            // Load failure → fall through to discard semantics (boot doc
            // stays); the orphan is still consumed below.
        }

        Autosave::DeleteOrphan(s);
        sendOk(json::object());
        return res;
    }

    // -------- spawner/* (Phase 3 Screen 8 Batch 4) -------------------
    //
    // The new-UI host doesn't yet own a SpawnerDriver* (matches Batch 3
    // for ParticleSystem*). The handlers do the *editor-level* side of
    // the work: cache the incoming config in m_spawnerConfig so a
    // subsequent engine/state/snapshot returns it, log the request for
    // diagnostics, and broadcast engine/state/changed so React sees the
    // updated config land. When SpawnerDriver wiring happens in a later
    // batch, the cached config can be passed directly into
    // `m_spawnerDriver->SetConfig(...)` from these same handlers.
    //
    // Note: spawner config is session state (matches legacy: "never
    // written into the .alo" per SpawnerDriver.h:16). It deliberately
    // does NOT set dirty=true.
    if (kind == "spawner/start")
    {
        //: cache + commit to the real driver. The cache is kept
        // updated so snapshot reads still work when no driver is bound
        // (Vitest / partial-wiring paths).
        m_spawnerConfig = params;
        if (m_spawnerDriver)
        {
            SpawnerConfig cfg = JsonToSpawnerConfig(params);
            ClampSpawnerConfig(cfg);
            m_spawnerDriver->SetConfig(cfg);
        }
        sendOk(json::object());
        EmitEngineStateChanged();
        return res;
    }
    if (kind == "spawner/trigger")
    {
        //: real trigger. Note that without per-frame Tick wiring
        // the burst-state machine doesn't advance — Trigger schedules
        // a burst that won't actually fire instances until a later
        // batch wires SpawnerDriver::Tick into the render loop. That's
        // the documented out-of-scope item for this batch.
        if (m_spawnerDriver
            && m_pParticleSystem
            && *m_pParticleSystem
            && m_engine)
        {
            m_spawnerDriver->Trigger(m_pParticleSystem->get(), m_engine);
        }
        sendOk(json::object());
        return res;
    }
    if (kind == "spawner/stop")
    {
        //: flip enabled=false on the live driver. Auto-mode
        // bursts stop scheduling; manual triggers still work.
        if (m_spawnerDriver)
        {
            SpawnerConfig cfg = m_spawnerDriver->GetConfig();
            cfg.enabled = false;
            m_spawnerDriver->SetConfig(cfg);
        }
        // Keep the JSON cache in sync so snapshots without a bound
        // driver also reflect the stop.
        if (m_spawnerConfig.is_object())
        {
            m_spawnerConfig["enabled"] = false;
        }
        sendOk(json::object());
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/preview-from-file ----------------------------
    //
    //: actually load the .alo into a temporary ParticleSystem and
    // build the EmitterTreeNode tree. The temporary system drops at
    // scope exit. Note we wrap the real roots under a synthetic
    // `id: 0, name: "root"` node to match the MockBridge response
    // shape — the schema's EmitterTreeNode is single-rooted but a
    // ParticleSystem can have multiple root emitters.
    if (kind == "emitters/preview-from-file")
    {
        std::string path8 = params.value("path", std::string{});
        if (path8.empty())
        {
            sendOk(json{{"ok", false}, {"error", "missing path"}});
            return res;
        }
        std::wstring path = Utf8ToWide(path8);
        std::string err;
        std::unique_ptr<ParticleSystem> tmp = LoadParticleSystem(path, &err);
        if (!tmp)
        {
            sendOk(json{
                {"ok",    false},
                {"error", err.empty() ? std::string("could not load file") : err},
            });
            return res;
        }
        // Build the synthetic root + per-actual-root children.
        json children = json::array();
        const auto& emitters = tmp->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr && emitters[i]->parent == nullptr)
            {
                children.push_back(BuildEmitterTreeNode(tmp.get(), i));
            }
        }
        // Synthetic root carries id 0 (legacy convention for preview)
        // but uses the new shape fields (role / linkGroup / visible)
        // so consumers don't see undefined when the schema is read
        // strictly.
        json tree = {
            {"id",        0},
            {"stableId",  0},  // synthetic root: 0 is reserved (real ids start at 1)
            {"name",      "root"},
            {"role",      "root"},
            {"linkGroup", 0},
            {"visible",   true},
            // Zero spawn: synthetic root never warns (= ZERO_SPAWN in
            // bridge-schema, the canonical zero shape).
            {"spawn", json{
                {"lifetime", 0.0}, {"useBursts", false}, {"nBursts", 0},
                {"burstDelay", 0.0}, {"nParticlesPerSecond", 0}, {"nParticlesPerBurst", 0},
            }},
            {"children",  children},
        };
        sendOk(json{{"ok", true}, {"tree", tree}});
        return res;
    }

    // -------- emitters/list -----------------------------------------
    //
    // Screen 4 Batch A — real implementation. Walks the live particle
    // system and returns a synthetic-root wrapper whose children are
    // the real top-level emitters. Returns an empty wrapper if no
    // system is bound (e.g. tests that haven't wired BindHostState).
    if (kind == "emitters/list")
    {
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
            {"stableId",  0},  // synthetic root: 0 is reserved (real ids start at 1)
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
        sendOk(json{{"root", tree}});
        return res;
    }

    // -------- emitters/select ---------------------------------------
    //
    // Screen 4 Batch A — selection state lives on the dispatcher (it's
    // editor state, not engine state). Update the scalar, emit the
    // narrow `emitters/selected` event (subscribed to by EmitterTree)
    // and a follow-up engine/state/changed so any snapshot consumer
    // sees the new selectedEmitterId.
    if (kind == "emitters/select")
    {
        // params.id is `number | null` on the wire. Null deserialises
        // to a JSON null; we store as -1 internally and re-serialise
        // as JSON null on the way out (in BuildEngineStateSnapshot).
        int newId = -1;
        if (params.contains("id") && !params["id"].is_null())
            newId = params["id"].get<int>();

        // Validate against the live tree when bound — selecting an
        // index that isn't a real emitter resets to no-selection. This
        // matches the MockBridge's behaviour and keeps the snapshot
        // honest. When no system is bound we accept the id as-is so
        // tests without BindHostState still round-trip.
        if (newId >= 0 && m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            const auto& emitters = (*m_pParticleSystem)->getEmitters();
            if (static_cast<size_t>(newId) >= emitters.size() || emitters[newId] == nullptr)
                newId = -1;
        }

        m_selectedEmitterId = newId;
        sendOk(json::object());

        // emitters/selected event — narrow payload for components that
        // care only about the selection scalar (EmitterTree).
        if (m_emit)
        {
            json env = {
                {"type",    "evt"},
                {"kind",    "emitters/selected"},
                {"payload", json{{"id", newId < 0 ? json(nullptr) : json(newId)}}},
            };
            m_emit(env.dump());
        }

        // engine/state/changed so the snapshot's selectedEmitterId is
        // observable through the standard snapshot channel too.
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/get-tracks (Screen 6 Batch A) ----------------
    //
    // Read-only. Serialises the named emitter's 7 tracks (Red, Green,
    // Blue, Alpha, Scale, Index, RotationSpeed in fixed order). Each
    // track's `keys` are emitted in ascending-time order (the source
    // `std::multiset<Key>` already orders by `time` via Key::operator<).
    // Interpolation enum maps as documented in
    // `bridge-schema/src/index.ts`:
    //   IT_LINEAR (0) → "linear"
    //   IT_SMOOTH (1) → "smooth"
    //   IT_STEP   (2) → "step"
    // IT_UNKNOWN (-1) is coerced to "linear" before sending so the
    // wire never carries the sentinel.
    //
    // Unknown id (or no system bound) returns 7 empty tracks rather
    // than ok:false — the React panel renders a "no data" stub instead
    // of an error toast on transient mismatches (e.g. selection lands
    // on an id that was just deleted).
    if (kind == "emitters/get-tracks")
    {
        static const char* kTrackNames[ParticleSystem::NUM_TRACKS] = {
            "red", "green", "blue", "alpha",
            "scale", "index", "rotationSpeed",
        };
        auto interpToString = [](ParticleSystem::Emitter::Track::InterpolationType it) -> const char* {
            switch (it)
            {
                case ParticleSystem::Emitter::Track::IT_LINEAR: return "linear";
                case ParticleSystem::Emitter::Track::IT_SMOOTH: return "smooth";
                case ParticleSystem::Emitter::Track::IT_STEP:   return "step";
                default: return "linear";
            }
        };

        int id = params.value("id", -1);
        json tracksArr = json::array();
        const ParticleSystem::Emitter* emit = nullptr;
        if (id >= 0 && m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            const auto& emitters = (*m_pParticleSystem)->getEmitters();
            if (static_cast<size_t>(id) < emitters.size() && emitters[id] != nullptr)
            {
                emit = emitters[id];
            }
        }
        for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
        {
            json keysArr = json::array();
            const char* interp = "linear";
            if (emit != nullptr)
            {
                const ParticleSystem::Emitter::Track* t = emit->tracks[i];
                if (t != nullptr)
                {
                    // KeyMap is `std::multiset<Key>` ordered by time —
                    // a straight iteration emits keys in ascending
                    // time order.
                    for (const auto& k : t->keys)
                    {
                        keysArr.push_back(json{
                            {"time",  k.time},
                            {"value", k.value},
                        });
                    }
                    interp = interpToString(t->interpolation);
                }
            }
            // lockedTo: detect by pointer identity per the legacy
            // model — channel i is locked to channel j when
            // `tracks[i] == &trackContents[j]` (or transitively
            // `tracks[i] == tracks[j]`, which collapses to the same
            // pointer after the engine's file-load consolidation
            // pass). Only RGBA participate; other channels always
            // report null. Self-pointer (tracks[i] == &trackContents[i])
            // means "not locked" and also reports null.
            const char* lockedToName = nullptr;
            if (emit != nullptr && i < 4)
            {
                for (int j = 0; j < 4; j++)
                {
                    if (j == i) continue;
                    if (emit->tracks[i] == &emit->trackContents[j]
                        || emit->tracks[i] == emit->tracks[j])
                    {
                        // Only "earlier channel" locks are valid per
                        // the schema. If we matched a later channel
                        // via transitive equality, skip it — the
                        // earlier channel will be the canonical lock
                        // target when we hit it in this loop.
                        if (j < i)
                        {
                            lockedToName = kTrackNames[j];
                            break;
                        }
                    }
                }
            }
            tracksArr.push_back(json{
                {"name",          kTrackNames[i]},
                {"keys",          keysArr},
                {"interpolation", interp},
                {"lockedTo",      lockedToName == nullptr
                                      ? json(nullptr)
                                      : json(lockedToName)},
            });
        }
        sendOk(json{{"tracks", tracksArr}});
        return res;
    }

    // -------- Screen 4 Batch B1 — emitter mutations -----------------
    //
    // Each handler validates the target emitter, captures a PRE-
    // mutation undo snapshot via captureUndo(), mutates via the
    // ParticleSystem API, then emits `emitters/tree/changed` + dirty.
    // The PRE-mutation timing pairs with undo/perform's head-of-
    // history auto-capture above (see lines ~1396) so Ctrl+Z restores
    // the state right before the mutation ran. link-group sweeps
    // sit BETWEEN the mutation and the next captureUndo — covered by
    // the same snapshot atomically.

    // Lookup helper: find an emitter pointer by integer index. Returns
    // nullptr on out-of-range / no-system / null-slot. Used by all
    // emitter-mutation handlers below.
    auto getEmitterById = [&](int id) -> ParticleSystem::Emitter* {
        if (id < 0) return nullptr;
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return nullptr;
        const auto& emitters = (*m_pParticleSystem)->getEmitters();
        if (static_cast<size_t>(id) >= emitters.size()) return nullptr;
        return emitters[id];
    };

    // Capture-undo helper. Wraps the dispatcher's m_undo with the
    // current selection index so a future undo restores the
    // pre-mutation state. coalesceKey defaults to 0 = never coalesce
    // (structural ops must never fold across an add/delete/move). A
    // non-zero key routes to CapturePreCoalesced so rapid same-key edits
    // (a wheel-spun spinner / held arrow on one emitter) fold into a
    // single undo step within the time window — see legacy EP_CHANGE
    // coalescing (main.cpp ~2682).
    auto captureUndo = [&](DWORD coalesceKey = 0) {
        if (m_undo == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem) return;
        const ParticleSystem* sys = m_pParticleSystem->get();
        size_t selIdx = SIZE_MAX;
        if (m_selectedEmitterId >= 0
            && static_cast<size_t>(m_selectedEmitterId) < sys->getEmitters().size())
        {
            selIdx = static_cast<size_t>(m_selectedEmitterId);
        }
        if (coalesceKey != 0)
            m_undo->CapturePreCoalesced(*sys, selIdx, coalesceKey);
        else
            m_undo->Capture(*sys, selIdx, 0);
    };

    // F4: link-group propagation. After a shared (non-exempt) field is
    // edited on a linked emitter, copy its non-exempt params to every
    // group sibling — the new-UI equivalent of the legacy post-edit
    // chokepoint in CaptureUndo (src/main.cpp). MUST be called AFTER the
    // mutation; the pre-mutation captureUndo() above snapshots the whole
    // system, so a single Ctrl+Z restores the entire group atomically.
    // No-op for unlinked emitters (linkGroup == 0).
    auto propagateLinkGroup = [&](ParticleSystem::Emitter* edited) {
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
        //: copySharedParamsFrom REASSIGNS each sibling's non-exempt
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
    };

    // -------- emitters/get-properties (Phase 4.1 Fix dispatch 1) ----
    //
    // Walks every editable Basic + Appearance + Physics field on the
    // named emitter and serialises into an EmitterPropertiesDto. The
    // `groups: GroupDto[]` field surfaces the 3 Group entries (NUM_GROUPS).
    // Unknown id / no system returns ok:false; the React panel
    // tolerates the failure by rendering the placeholder branch.
    if (kind == "emitters/get-properties")
    {
        int id = params.value("id", -1);
        const ParticleSystem::Emitter* emit = getEmitterById(id);
        if (emit == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        // Helper: pack a Vec3 from three scalars.
        auto vec3 = [](float x, float y, float z) {
            return json::array({x, y, z});
        };

        json groupsArr = json::array();
        for (int g = 0; g < ParticleSystem::NUM_GROUPS; g++)
        {
            const auto& gr = emit->groups[g];
            groupsArr.push_back(json{
                {"type",            static_cast<int>(gr.type)},
                {"min",             vec3(gr.minX, gr.minY, gr.minZ)},
                {"max",             vec3(gr.maxX, gr.maxY, gr.maxZ)},
                {"sideLength",      gr.sideLength},
                {"sphereRadius",    gr.sphereRadius},
                {"sphereEdge",      static_cast<int>(gr.sphereEdge)},
                {"cylinderRadius",  gr.cylinderRadius},
                {"cylinderEdge",    static_cast<int>(gr.cylinderEdge)},
                {"cylinderHeight",  gr.cylinderHeight},
                {"val",             vec3(gr.valX, gr.valY, gr.valZ)},
            });
        }

        json props = {
            // ── Basic ───────────────────────────────────────────────
            {"name",                     emit->name},
            {"lifetime",                 emit->lifetime},
            {"initialDelay",             emit->initialDelay},
            {"useBursts",                emit->useBursts},
            {"nBursts",                  static_cast<int>(emit->nBursts)},
            {"burstDelay",               emit->burstDelay},
            {"nParticlesPerBurst",       static_cast<int>(emit->nParticlesPerBurst)},
            {"nParticlesPerSecond",      static_cast<int>(emit->nParticlesPerSecond)},
            {"randomLifetimePerc",       emit->randomLifetimePerc},
            {"randomScalePerc",          emit->randomScalePerc},
            {"randomRotation",           emit->randomRotation},
            {"randomRotationDirection",  emit->randomRotationDirection},
            {"randomRotationAverage",    emit->randomRotationAverage},
            {"randomRotationVariance",   emit->randomRotationVariance},
            {"freezeTime",               emit->freezeTime},
            {"skipTime",                 emit->skipTime},
            {"linkToSystem",             emit->linkToSystem},
            {"parentLinkStrength",       emit->parentLinkStrength},
            {"index",                    static_cast<int>(emit->index)},

            // ── Appearance ─────────────────────────────────────────
            {"colorTexture",             emit->colorTexture},
            {"normalTexture",            emit->normalTexture},
            {"blendMode",                static_cast<int>(emit->blendMode)},
            {"textureSize",              static_cast<int>(emit->textureSize)},
            {"nTriangles",               static_cast<int>(emit->nTriangles)},
            {"doColorAddGrayscale",      emit->doColorAddGrayscale},
            {"randomColors",             json::array({
                emit->randomColors[0], emit->randomColors[1],
                emit->randomColors[2], emit->randomColors[3],
            })},
            {"hasTail",                  emit->hasTail},
            {"tailSize",                 emit->tailSize},
            {"isHeatParticle",           emit->isHeatParticle},
            {"isWorldOriented",          emit->isWorldOriented},
            {"noDepthTest",              emit->noDepthTest},
            {"affectedByWind",           emit->affectedByWind},

            // ── Physics ────────────────────────────────────────────
            {"acceleration",             vec3(emit->acceleration[0],
                                              emit->acceleration[1],
                                              emit->acceleration[2])},
            {"gravity",                  emit->gravity},
            {"inwardSpeed",              emit->inwardSpeed},
            {"inwardAcceleration",       emit->inwardAcceleration},
            {"objectSpaceAcceleration",  emit->objectSpaceAcceleration},
            {"bounciness",               emit->bounciness},
            {"groundBehavior",           static_cast<int>(emit->groundBehavior)},
            {"emitFromMesh",             emit->emitFromMesh},
            {"emitFromMeshOffset",       emit->emitFromMeshOffset},
            {"isWeatherParticle",        emit->isWeatherParticle},
            {"weatherCubeSize",          emit->weatherCubeSize},
            {"weatherCubeDistance",      emit->weatherCubeDistance},
            {"weatherFadeoutDistance",   emit->weatherFadeoutDistance},

            {"groups",                   groupsArr},
        };
        sendOk(json{{"properties", props}});
        return res;
    }

    // -------- emitters/set-properties (Phase 4.1 Fix dispatch 1) ----
    //
    // Batch patch: iterate over each key present in `patch` and apply
    // it directly to the target emitter's struct field. Captures undo
    // once, emits state/changed + tree/changed once, marks dirty once
    // per call regardless of how many fields the patch touched.
    //
    // Field type guards: nlohmann::json's `.value(key, fallback)` reads
    // through `get<T>`, which throws on type mismatch. Each branch uses
    // `is_*` checks before assignment so a stray null / wrong-type
    // field is a silent skip rather than a hard fault.
    if (kind == "emitters/set-properties")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* emit = getEmitterById(id);
        if (emit == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }
        if (!params.contains("patch") || !params["patch"].is_object())
        {
            sendErr("missing patch");
            return res;
        }
        const json& patch = params["patch"];

        // Coalesce rapid edits to the SAME field(s) on the SAME emitter
        // (scroll-wheel ticks, held arrow) within the time window into one
        // undo step; switching field starts a fresh step. Finer than legacy's
        // per-emitter EP_CHANGE coalescing (main.cpp:2682) — a deliberate
        // choice. Key layout: bit 31 set (never 0 = structural), bits
        // 16..30 an order-independent FNV-1a hash of the patch field names,
        // bits 0..15 the emitter id (so different emitters never fold).
        uint32_t fieldHash = 0;
        for (auto it = patch.begin(); it != patch.end(); ++it)
        {
            uint32_t h = 2166136261u; // FNV-1a offset basis
            for (unsigned char c : it.key()) { h ^= c; h *= 16777619u; }
            fieldHash ^= h; // XOR = order-independent across patch keys
        }
        const DWORD coalesceKey =
            0x80000000u | ((fieldHash & 0x7FFFu) << 16) | (static_cast<DWORD>(id) & 0xFFFFu);
        captureUndo(coalesceKey);

        // Helper macros — keep the per-field branch concise. Each
        // branch reads through `at()` only after a `contains()` check
        // so missing keys are a no-op.
        auto getBool = [&](const char* key, bool fallback) -> bool {
            if (patch.contains(key) && patch.at(key).is_boolean()) return patch.at(key).get<bool>();
            return fallback;
        };
        auto getFloat = [&](const char* key, float fallback) -> float {
            if (patch.contains(key) && patch.at(key).is_number()) return patch.at(key).get<float>();
            return fallback;
        };
        auto getInt = [&](const char* key, int fallback) -> int {
            if (patch.contains(key) && patch.at(key).is_number_integer()) return patch.at(key).get<int>();
            if (patch.contains(key) && patch.at(key).is_number()) return static_cast<int>(patch.at(key).get<double>());
            return fallback;
        };
        auto getString = [&](const char* key, const std::string& fallback) -> std::string {
            if (patch.contains(key) && patch.at(key).is_string()) return patch.at(key).get<std::string>();
            return fallback;
        };

        // ── Basic ───────────────────────────────────────────────────
        if (patch.contains("name"))                    emit->name = getString("name", emit->name);
        if (patch.contains("lifetime"))                emit->lifetime = getFloat("lifetime", emit->lifetime);
        if (patch.contains("initialDelay"))            emit->initialDelay = getFloat("initialDelay", emit->initialDelay);
        if (patch.contains("useBursts"))               emit->useBursts = getBool("useBursts", emit->useBursts);
        if (patch.contains("nBursts"))                 emit->nBursts = static_cast<unsigned long>(getInt("nBursts", static_cast<int>(emit->nBursts)));
        if (patch.contains("burstDelay"))              emit->burstDelay = getFloat("burstDelay", emit->burstDelay);
        if (patch.contains("nParticlesPerBurst"))      emit->nParticlesPerBurst = static_cast<unsigned long>(getInt("nParticlesPerBurst", static_cast<int>(emit->nParticlesPerBurst)));
        if (patch.contains("nParticlesPerSecond"))     emit->nParticlesPerSecond = static_cast<unsigned long>(getInt("nParticlesPerSecond", static_cast<int>(emit->nParticlesPerSecond)));
        if (patch.contains("randomLifetimePerc"))      emit->randomLifetimePerc = getFloat("randomLifetimePerc", emit->randomLifetimePerc);
        if (patch.contains("randomScalePerc"))         emit->randomScalePerc = getFloat("randomScalePerc", emit->randomScalePerc);
        if (patch.contains("randomRotation"))          emit->randomRotation = getBool("randomRotation", emit->randomRotation);
        if (patch.contains("randomRotationDirection")) emit->randomRotationDirection = getBool("randomRotationDirection", emit->randomRotationDirection);
        if (patch.contains("randomRotationAverage"))   emit->randomRotationAverage = getFloat("randomRotationAverage", emit->randomRotationAverage);
        if (patch.contains("randomRotationVariance"))  emit->randomRotationVariance = getFloat("randomRotationVariance", emit->randomRotationVariance);
        if (patch.contains("freezeTime"))              emit->freezeTime = getFloat("freezeTime", emit->freezeTime);
        if (patch.contains("skipTime"))                emit->skipTime = getFloat("skipTime", emit->skipTime);
        if (patch.contains("linkToSystem"))            emit->linkToSystem = getBool("linkToSystem", emit->linkToSystem);
        if (patch.contains("parentLinkStrength"))      emit->parentLinkStrength = getFloat("parentLinkStrength", emit->parentLinkStrength);
        if (patch.contains("index"))                   emit->index = static_cast<size_t>(getInt("index", static_cast<int>(emit->index)));

        // ── Appearance ─────────────────────────────────────────────
        if (patch.contains("colorTexture"))            emit->colorTexture = getString("colorTexture", emit->colorTexture);
        if (patch.contains("normalTexture"))           emit->normalTexture = getString("normalTexture", emit->normalTexture);
        if (patch.contains("blendMode"))               emit->blendMode = static_cast<unsigned long>(getInt("blendMode", static_cast<int>(emit->blendMode)));
        if (patch.contains("textureSize"))             emit->textureSize = static_cast<unsigned long>(getInt("textureSize", static_cast<int>(emit->textureSize)));
        if (patch.contains("nTriangles"))              emit->nTriangles = static_cast<unsigned long>(getInt("nTriangles", static_cast<int>(emit->nTriangles)));
        if (patch.contains("doColorAddGrayscale"))     emit->doColorAddGrayscale = getBool("doColorAddGrayscale", emit->doColorAddGrayscale);
        if (patch.contains("randomColors") && patch.at("randomColors").is_array() && patch.at("randomColors").size() == 4)
        {
            for (int i = 0; i < 4; i++)
            {
                if (patch.at("randomColors")[i].is_number())
                    emit->randomColors[i] = patch.at("randomColors")[i].get<float>();
            }
        }
        if (patch.contains("hasTail"))                 emit->hasTail = getBool("hasTail", emit->hasTail);
        if (patch.contains("tailSize"))                emit->tailSize = getFloat("tailSize", emit->tailSize);
        if (patch.contains("isHeatParticle"))          emit->isHeatParticle = getBool("isHeatParticle", emit->isHeatParticle);
        if (patch.contains("isWorldOriented"))         emit->isWorldOriented = getBool("isWorldOriented", emit->isWorldOriented);
        if (patch.contains("noDepthTest"))             emit->noDepthTest = getBool("noDepthTest", emit->noDepthTest);
        if (patch.contains("affectedByWind"))          emit->affectedByWind = getBool("affectedByWind", emit->affectedByWind);

        // ── Physics ────────────────────────────────────────────────
        if (patch.contains("acceleration") && patch.at("acceleration").is_array() && patch.at("acceleration").size() == 3)
        {
            for (int i = 0; i < 3; i++)
            {
                if (patch.at("acceleration")[i].is_number())
                    emit->acceleration[i] = patch.at("acceleration")[i].get<float>();
            }
        }
        if (patch.contains("gravity"))                 emit->gravity = getFloat("gravity", emit->gravity);
        if (patch.contains("inwardSpeed"))             emit->inwardSpeed = getFloat("inwardSpeed", emit->inwardSpeed);
        if (patch.contains("inwardAcceleration"))      emit->inwardAcceleration = getFloat("inwardAcceleration", emit->inwardAcceleration);
        if (patch.contains("objectSpaceAcceleration")) emit->objectSpaceAcceleration = getBool("objectSpaceAcceleration", emit->objectSpaceAcceleration);
        if (patch.contains("bounciness"))              emit->bounciness = getFloat("bounciness", emit->bounciness);
        if (patch.contains("groundBehavior"))          emit->groundBehavior = static_cast<unsigned long>(getInt("groundBehavior", static_cast<int>(emit->groundBehavior)));
        if (patch.contains("emitFromMesh"))            emit->emitFromMesh = getInt("emitFromMesh", emit->emitFromMesh);
        if (patch.contains("emitFromMeshOffset"))      emit->emitFromMeshOffset = getFloat("emitFromMeshOffset", emit->emitFromMeshOffset);
        if (patch.contains("isWeatherParticle"))       emit->isWeatherParticle = getBool("isWeatherParticle", emit->isWeatherParticle);
        if (patch.contains("weatherCubeSize"))         emit->weatherCubeSize = getFloat("weatherCubeSize", emit->weatherCubeSize);
        if (patch.contains("weatherCubeDistance"))     emit->weatherCubeDistance = getFloat("weatherCubeDistance", emit->weatherCubeDistance);
        if (patch.contains("weatherFadeoutDistance"))  emit->weatherFadeoutDistance = getFloat("weatherFadeoutDistance", emit->weatherFadeoutDistance);

        // ── Groups (NUM_GROUPS=3) ──────────────────────────────────
        if (patch.contains("groups") && patch.at("groups").is_array())
        {
            const json& gs = patch.at("groups");
            const int n = std::min<int>(ParticleSystem::NUM_GROUPS,
                                        static_cast<int>(gs.size()));
            for (int gi = 0; gi < n; gi++)
            {
                const json& g = gs[gi];
                if (!g.is_object()) continue;
                auto& dst = emit->groups[gi];
                if (g.contains("type") && g.at("type").is_number_integer())
                    dst.type = static_cast<unsigned int>(g.at("type").get<int>());
                if (g.contains("min") && g.at("min").is_array() && g.at("min").size() == 3)
                {
                    dst.minX = g.at("min")[0].get<float>();
                    dst.minY = g.at("min")[1].get<float>();
                    dst.minZ = g.at("min")[2].get<float>();
                }
                if (g.contains("max") && g.at("max").is_array() && g.at("max").size() == 3)
                {
                    dst.maxX = g.at("max")[0].get<float>();
                    dst.maxY = g.at("max")[1].get<float>();
                    dst.maxZ = g.at("max")[2].get<float>();
                }
                if (g.contains("sideLength") && g.at("sideLength").is_number())
                    dst.sideLength = g.at("sideLength").get<float>();
                if (g.contains("sphereRadius") && g.at("sphereRadius").is_number())
                    dst.sphereRadius = g.at("sphereRadius").get<float>();
                if (g.contains("sphereEdge") && g.at("sphereEdge").is_number_integer())
                    dst.sphereEdge = static_cast<unsigned int>(g.at("sphereEdge").get<int>());
                if (g.contains("cylinderRadius") && g.at("cylinderRadius").is_number())
                    dst.cylinderRadius = g.at("cylinderRadius").get<float>();
                if (g.contains("cylinderEdge") && g.at("cylinderEdge").is_number_integer())
                    dst.cylinderEdge = static_cast<unsigned int>(g.at("cylinderEdge").get<int>());
                if (g.contains("cylinderHeight") && g.at("cylinderHeight").is_number())
                    dst.cylinderHeight = g.at("cylinderHeight").get<float>();
                if (g.contains("val") && g.at("val").is_array() && g.at("val").size() == 3)
                {
                    dst.valX = g.at("val")[0].get<float>();
                    dst.valY = g.at("val")[1].get<float>();
                    dst.valZ = g.at("val")[2].get<float>();
                }
            }
        }

        propagateLinkGroup(emit); // F4: keep link-group siblings in sync
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/duplicate -------------------------------------
    //
    // Mirrors legacy `EmitterList_DuplicateEmitter` at
    // [src/UI/EmitterList.cpp:4707]. Round-trips the source through
    // the chunk serializer so the duplicate starts with empty
    // m_instances (a direct copy-construct would shallow-copy that
    // std::set and double-free on later deletion). The duplicate
    // becomes a root via `insertEmitterAfter`.
    // -------- emitters/import-from-file (audit G1) ------------------
    //
    // Clone the `selected` source emitters from another `.alo` into the
    // live system as new roots, via the shared data-layer core
    // ParticleSystem::ImportEmittersFrom (the same logic the legacy import
    // dialog uses). Atomic single undo; emits the tree-changed event.
    // Placed after the captureUndo lambda (defined above) with the other
    // emitter-mutation handlers.
    if (kind == "emitters/import-from-file")
    {
        // Hard failures go through sendErr (envelope ok:false) so the bridge
        // promise REJECTS and the dialog's catch surfaces the error + keeps the
        // modal open. (A nested sendOk{ok:false} would resolve as success and
        // close the dialog silently — review finding, the G3 nested-ok trap.)
        if (!m_pParticleSystem || !*m_pParticleSystem)
        {
            sendErr("no particle system bound");
            return res;
        }
        std::string path8 = params.value("path", std::string{});
        if (path8.empty())
        {
            sendErr("missing path");
            return res;
        }
        std::vector<size_t> picks;
        if (params.contains("selected") && params["selected"].is_array())
        {
            for (const auto& v : params["selected"])
            {
                if (!v.is_number()) continue;
                long long n = v.get<long long>();
                if (n >= 0) picks.push_back(static_cast<size_t>(n));
            }
        }
        if (picks.empty())
        {
            sendErr("no emitters selected");
            return res;
        }
        std::wstring path = Utf8ToWide(path8);
        std::string err;
        std::unique_ptr<ParticleSystem> tmp = LoadParticleSystem(path, &err);
        if (!tmp)
        {
            sendErr(err.empty() ? std::string("could not load file") : err);
            return res;
        }
        // Drop out-of-range picks BEFORE capturing undo. An all-out-of-range
        // request imports nothing, so it must not push an undo snapshot or
        // dirty the document (audit-F1 'no dirty on a no-op'). In-range picks
        // keep their order.
        const size_t srcCount = tmp->getEmitters().size();
        std::vector<size_t> validPicks;
        for (size_t p : picks) if (p < srcCount) validPicks.push_back(p);
        if (validPicks.empty())
        {
            sendErr("no valid emitters to import");
            return res;
        }
        // Snapshot BEFORE mutating so the whole import is one undo unit; the
        // load + bound-check above ran first, so a failed/no-op import never
        // leaves a stray undo entry.
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        size_t n = sys->ImportEmittersFrom(
            *tmp, validPicks,
            [sys](const std::string& nm) { return GenerateDuplicateName(sys, nm); });
        sendOk(json{{"ok", true}, {"imported", static_cast<int>(n)}});
        // Only signal a mutation if something actually imported (n could be 0
        // only if every clone threw — a corrupt source that still loaded).
        if (n > 0)
        {
            markDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return res;
    }

    if (kind == "emitters/duplicate")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            sendOk(json{{"ok", false}, {"error", "emitter not found"}});
            return res;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* dup = nullptr;
        MemoryFile* memfile = new MemoryFile;
        try
        {
            ChunkWriter writer(memfile);
            source->copy(writer);

            memfile->seek(0);
            ChunkReader reader(memfile);
            ParticleSystem::Emitter cleanCopy(reader);

            // Auto-suffix the name to avoid collisions; mirrors the
            // legacy convention from EmitterList.cpp:4731.
            cleanCopy.name = GenerateDuplicateName(sys, source->name);

            dup = sys->insertEmitterAfter(source, cleanCopy);
        }
        catch (...)
        {
            memfile->Release();
            sendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
            return res;
        }
        memfile->Release();

        if (dup == nullptr)
        {
            sendOk(json{{"ok", false}, {"error", "insertEmitterAfter returned null"}});
            return res;
        }
        const int newId = static_cast<int>(dup->index);
        sendOk(json{{"ok", true}, {"newId", newId}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/duplicate-many --------------------------------
    //
    // Batch duplicate: clone each selected emitter (the same single-emitter
    // copy emitters/duplicate does, looped). Returns `newIds` — the copies'
    // final indices, aligned to the input `ids` order — so the React side
    // re-selects the new copies. We keep the dup POINTERS and read their
    // ->index AFTER every insert, so the index shifts each insertEmitterAfter
    // causes don't corrupt the result.
    if (kind == "emitters/duplicate-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"ok", false}, {"error", "no particle system bound"}});
            return res;
        }
        ParticleSystem* sys = m_pParticleSystem->get();

        std::vector<ParticleSystem::Emitter*> sources;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e != nullptr) sources.push_back(e);
            }
        }
        if (sources.empty())
        {
            sendOk(json{{"ok", false}, {"error", "no emitters to duplicate"}});
            return res;
        }

        captureUndo();

        std::vector<ParticleSystem::Emitter*> dups;
        dups.reserve(sources.size());
        for (ParticleSystem::Emitter* src : sources)
        {
            ParticleSystem::Emitter* dup = nullptr;
            MemoryFile* memfile = new MemoryFile;
            try
            {
                ChunkWriter writer(memfile);
                src->copy(writer);
                memfile->seek(0);
                ChunkReader reader(memfile);
                ParticleSystem::Emitter cleanCopy(reader);
                cleanCopy.name = GenerateDuplicateName(sys, src->name);
                dup = sys->insertEmitterAfter(src, cleanCopy);
            }
            catch (...)
            {
                memfile->Release();
                sendOk(json{{"ok", false}, {"error", "emitter copy failed"}});
                return res;
            }
            memfile->Release();
            if (dup == nullptr)
            {
                sendOk(json{{"ok", false}, {"error", "insertEmitterAfter returned null"}});
                return res;
            }
            dups.push_back(dup);
        }

        json newIds = json::array();
        for (ParticleSystem::Emitter* dup : dups)
            newIds.push_back(static_cast<int>(dup->index));

        sendOk(json{{"ok", true}, {"newIds", newIds}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/delete ---------------------------------------
    //
    // Mirrors legacy `EmitterList_DeleteEmitter` at
    // [src/UI/EmitterList.cpp:4651]. ParticleSystem::deleteEmitter
    // recursively deletes a subtree. If the deleted id matches the
    // selection scalar, clear it and emit emitters/selected { id: null }.
    if (kind == "emitters/delete")
    {
        int id = params.value("id", -1);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            // No-op delete still returns success — matches the
            // mock's permissive behaviour.
            sendOk(json::object());
            return res;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        const bool wasSelected = (m_selectedEmitterId == id);
        sys->deleteEmitter(target);

        if (wasSelected)
        {
            m_selectedEmitterId = -1;
            if (m_emit)
            {
                json env = {
                    {"type",    "evt"},
                    {"kind",    "emitters/selected"},
                    {"payload", json{{"id", json(nullptr)}}},
                };
                m_emit(env.dump());
            }
        }

        // Demote any singleton groups left over from the
        // recursive subtree deletion (member of a 2-member group
        // deleted → survivor is now alone in the group → demote).
        // captureUndo() above covers both the deletion AND the
        // sweep atomically.
        EnforceSingleMemberLinkGroups();

        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/rename ---------------------------------------
    //
    // Legacy uses an inline tree-view edit (EmitterList_RenameEmitter
    // at line 4814 → TreeView_EditLabel). The new-UI flow uses a modal,
    // dispatched here as a plain setName. Capture-undo guards against
    // mid-edit Ctrl-Z weirdness.
    if (kind == "emitters/rename")
    {
        int id = params.value("id", -1);
        std::string name = params.value("name", std::string{});
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        captureUndo();
        target->name = name;

        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/delete-track-keys + set-track-interpolation --
    //
    // Screen 5 / Screen 6 Batch B-α track mutations. Both handlers
    // resolve the emitter by id, look up the named track on `tracks[]`
    // (the slot pointer aliasing — see the comment block in
    // ParticleSystem.h:148), then mutate the underlying multiset /
    // enum directly. Border keys (first + last in time order on the
    // multiset, which is already ordered by Key::operator<) are
    // silently skipped by delete-track-keys per legacy semantics;
    // they define the track's [0, 100] time range and aren't
    // deletable.
    //
    // Both mutations capture undo, mark the editor dirty, and emit
    // engine/state/changed + emitters/tree/changed so the React
    // panel re-fetches via `emitters/get-tracks`.
    auto trackNameToIndex = [](const std::string& name) -> int {
        if (name == "red")           return ParticleSystem::TRACK_RED_CHANNEL;
        if (name == "green")         return ParticleSystem::TRACK_GREEN_CHANNEL;
        if (name == "blue")          return ParticleSystem::TRACK_BLUE_CHANNEL;
        if (name == "alpha")         return ParticleSystem::TRACK_ALPHA_CHANNEL;
        if (name == "scale")         return ParticleSystem::TRACK_SCALE;
        if (name == "index")         return ParticleSystem::TRACK_INDEX;
        if (name == "rotationSpeed") return ParticleSystem::TRACK_ROTATION_SPEED;
        return -1;
    };

    if (kind == "emitters/delete-track-keys")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        const json& timesJson = params.contains("times") ? params["times"] : json::array();

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            sendErr("unknown track");
            return res;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr || track->keys.empty())
        {
            // Nothing to delete — return success silently to match the
            // mock's no-op semantics. Don't emit; nothing changed.
            sendOk(json::object());
            return res;
        }

        // Border keys = first + last in the multiset (ordered by
        // Key::operator< on `time`). std::multiset::begin / rbegin
        // are the cheapest way to grab them; cache the time values
        // for the skip check below.
        const float firstTime = track->keys.begin()->time;
        const float lastTime  = track->keys.rbegin()->time;

        // Capture undo BEFORE any erase — if every requested time is
        // a border-key no-op we'll discover that in the loop and
        // the capture is a wasted snapshot, but Undo coalescing
        // handles that gracefully and the alternative (capture-late)
        // can't restore the half-mutated multiset if iteration aborts.
        captureUndo();

        int removed = 0;
        for (const auto& t : timesJson)
        {
            if (!t.is_number()) continue;
            float timeVal = t.get<float>();
            // Silent-skip border keys.
            if (timeVal == firstTime || timeVal == lastTime) continue;
            // std::multiset::find takes a `Key` constructed from the
            // time alone; operator< compares only on time so the
            // probe value's `value` field is irrelevant.
            ParticleSystem::Emitter::Track::Key probe(timeVal, 0.0f);
            auto it = track->keys.find(probe);
            if (it != track->keys.end())
            {
                track->keys.erase(it);
                removed++;
            }
        }

        sendOk(json::object());
        if (removed > 0)
        {
            propagateLinkGroup(target); // F4: sync link-group siblings
            // Re-seat live particle track cursors — the erase(s) above
            // invalidated any cursor pointing at a removed key (see the
            // set-track-key handler for the full rationale).
            if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
            markDirty();
            EmitEmittersTreeChanged();
            EmitEngineStateChanged();
        }
        return res;
    }

    if (kind == "emitters/set-track-interpolation")
    {
        int id = params.value("id", -1);
        std::string trackName  = params.value("track",         std::string{});
        std::string interpName = params.value("interpolation", std::string{});

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            sendErr("unknown track");
            return res;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr)
        {
            // No track slot bound — silent no-op (matches the wire
            // contract which never surfaces a refusal envelope).
            sendOk(json::object());
            return res;
        }

        ParticleSystem::Emitter::Track::InterpolationType next;
        if      (interpName == "linear") next = ParticleSystem::Emitter::Track::IT_LINEAR;
        else if (interpName == "smooth") next = ParticleSystem::Emitter::Track::IT_SMOOTH;
        else if (interpName == "step")   next = ParticleSystem::Emitter::Track::IT_STEP;
        else
        {
            sendErr("unknown interpolation");
            return res;
        }

        if (track->interpolation == next)
        {
            // No-op — don't capture undo or fire events.
            sendOk(json::object());
            return res;
        }

        captureUndo();
        track->interpolation = next;

        propagateLinkGroup(target); // F4: sync link-group siblings
        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/set-track-lock ------------------------------
    //
    // Per-channel track lock. The legacy combo at
    // [TrackEditor.cpp:90-110](src/UI/TrackEditor.cpp:90) and the
    // file-load consolidation at [ParticleSystem.cpp:428] use the
    // *pointer identity* of `emit->tracks[i]` as the source of
    // truth for lock state: `tracks[i] == &trackContents[j]` (with
    // `i != j`) means channel `i` is read-only and displays
    // channel `j`'s key data.
    //
    // Locking rules (mirror legacy):
    //   - Only the first four channels (RGBA) participate.
    //   - Channel can only lock to an *earlier* channel (Green→Red,
    //     Blue→Red/Green, Alpha→Red/Green/Blue). Other combinations
    //     silently become "unlock" — UI surface already restricts
    //     options so this is defensive only.
    //   - `lockTo: null` restores `tracks[i] = &trackContents[i]`
    //     (channel owns its keys again; previous trackContents[i]
    //     is preserved because the lock didn't touch it).
    if (kind == "emitters/set-track-lock")
    {
        int id = params.value("id", -1);
        std::string channelName = params.value("channel", std::string{});
        // lockTo is `string | null` per the schema; nlohmann's
        // `value<string>` would throw on null, so check explicitly.
        std::string lockToName;
        bool lockToIsNull = !params.contains("lockTo") || params["lockTo"].is_null();
        if (!lockToIsNull) lockToName = params["lockTo"].get<std::string>();

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        int channelIdx = trackNameToIndex(channelName);
        if (channelIdx < 0)
        {
            sendErr("unknown channel");
            return res;
        }

        // Only RGBA participate. Silently accept and no-op for the
        // other three — keeps the React side simple (it can always
        // dispatch without first checking which channel it's on).
        if (channelIdx >= 4)
        {
            sendOk(json::object());
            return res;
        }

        ParticleSystem::Emitter::Track* desired = nullptr;
        if (lockToIsNull)
        {
            desired = &target->trackContents[channelIdx];
        }
        else
        {
            int targetIdx = trackNameToIndex(lockToName);
            // Reject invalid targets (must be 0..3 AND earlier than us)
            // by falling back to self-pointer = unlock.
            if (targetIdx >= 0 && targetIdx < 4 && targetIdx < channelIdx)
            {
                desired = &target->trackContents[targetIdx];
            }
            else
            {
                desired = &target->trackContents[channelIdx];
            }
        }

        if (target->tracks[channelIdx] == desired)
        {
            // No-op — don't capture undo or fire events.
            sendOk(json::object());
            return res;
        }

        captureUndo();
        target->tracks[channelIdx] = desired;

        propagateLinkGroup(target); // F4: sync link-group siblings
        // Re-seat live particle track cursors for this channel. The lock
        // repointed tracks[channelIdx] at a DIFFERENT KeyMap, so existing
        // cursors (iterators into the old container) would be compared
        // against the new container's end() next frame — undefined
        // behavior. Reloading re-seats them into the now-current container.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(channelIdx);
        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/set-track-key (Screen 6 Batch B-β) ----------
    //
    // Drag-to-move + Spinner edit commit. Erases the key at
    // `oldTime` from the multiset and inserts `(newTime, newValue)`.
    // Border keys (first + last in time order on the multiset) have
    // their `newTime` silently overridden to `oldTime` — only the
    // value moves. This mirrors the React-side drag clamping; the
    // host is the source of truth.
    //
    // `std::multiset<Key>::find` accepts a Key constructed from the
    // time alone — operator< compares only on `time`, so the probe
    // Key's `value` field is irrelevant. Erase + insert is the
    // ordered-key idiom (no in-place mutation of the ordering key).
    if (kind == "emitters/set-track-key")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        float oldTime  = params.value("oldTime",  0.0f);
        float newTime  = params.value("newTime",  0.0f);
        float newValue = params.value("newValue", 0.0f);

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            sendErr("unknown track");
            return res;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr || track->keys.empty())
        {
            // Nothing to move — silent ok.
            sendOk(json::object());
            return res;
        }

        // Identify border keys before any mutation.
        const float firstTime = track->keys.begin()->time;
        const float lastTime  = track->keys.rbegin()->time;
        const bool isBorder = (oldTime == firstTime || oldTime == lastTime);
        if (isBorder)
        {
            // Border keys: time fixed.
            newTime = oldTime;
        }

        ParticleSystem::Emitter::Track::Key probe(oldTime, 0.0f);
        auto it = track->keys.find(probe);
        if (it == track->keys.end())
        {
            // Key not found at oldTime — silent ok (matches the
            // overlay's read-modify-write semantics).
            sendOk(json::object());
            return res;
        }

        // Coalesce rapid same-track edits on the same emitter (a wheel/
        // hold-arrow/scrub Value or Time key spinner, plus a multi-key
        // group shift's N per-key calls) into a single undo step within the
        // window. Per-TRACK keying — legacy's exact choice
        // (track<<16|emitterIdx) and the only stable key for a Time spinner,
        // whose oldTime moves every tick. Mirrors the emitter-property layout
        // (set-properties, this file) with trackIdx in place of the field
        // hash; bit 31 set so the key is never 0 (= structural / never-fold).
        const DWORD coalesceKey =
            0x80000000u | ((static_cast<DWORD>(trackIdx) & 0x7FFFu) << 16)
                        | (static_cast<DWORD>(id) & 0xFFFFu);
        captureUndo(coalesceKey);
        track->keys.erase(it);
        track->keys.insert(ParticleSystem::Emitter::Track::Key(newTime, newValue));

        propagateLinkGroup(target); // F4: sync link-group siblings
        // Re-seat the live per-particle track cursors. EmitterInstance
        // caches multiset iterators (prev/next) into tracks[trackIdx]->keys
        // for every live particle; the erase above invalidated any cursor
        // pointing at the moved key, so the next Engine::Update would
        // dereference a singular iterator (xtree assert). OnParticleSystemChanged
        // with the specific track index reloads those cursors — mirrors the
        // legacy editor's per-edit call at src/main.cpp:2695.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/add-track-key (Screen 6 Batch B-β) ----------
    //
    // Click-to-add (Insert mode) commit. Inserts a new key into the
    // multiset. If a key at the exact `time` already exists, bumps
    // `time` by 0.001 until unique so the multiset doesn't accumulate
    // ambiguously-ordered duplicates (the dedupe is mirrored by the
    // mock at `addTrackKeyInOverlay`). Returns the actual inserted
    // (time, value) so the React side can auto-select the new key.
    if (kind == "emitters/add-track-key")
    {
        int id = params.value("id", -1);
        std::string trackName = params.value("track", std::string{});
        float time  = params.value("time",  0.0f);
        float value = params.value("value", 0.0f);

        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        int trackIdx = trackNameToIndex(trackName);
        if (trackIdx < 0)
        {
            sendErr("unknown track");
            return res;
        }

        ParticleSystem::Emitter::Track* track = target->tracks[trackIdx];
        if (track == nullptr)
        {
            // No track slot bound — silent ok with the request shape.
            sendOk(json{{"time", time}, {"value", value}});
            return res;
        }

        // Dedupe-by-epsilon: bump until the time is unique. Bounded
        // by 1000 iterations as a defensive safety net so a pathological
        // dataset can't lock the dispatch thread.
        ParticleSystem::Emitter::Track::Key probe(time, 0.0f);
        int safety = 1000;
        while (track->keys.find(probe) != track->keys.end() && safety-- > 0)
        {
            time += 0.001f;
            probe = ParticleSystem::Emitter::Track::Key(time, 0.0f);
        }

        captureUndo();
        track->keys.insert(ParticleSystem::Emitter::Track::Key(time, value));

        propagateLinkGroup(target); // F4: sync link-group siblings
        // Re-seat live particle track cursors. insert() doesn't invalidate
        // existing iterators, but a key added BETWEEN a particle's prev/next
        // cursors would be skipped (stale interpolation) until the cursors
        // reload — so re-seat here too, matching the legacy per-edit call.
        if (m_engine != nullptr) m_engine->OnParticleSystemChanged(trackIdx);
        sendOk(json{{"time", time}, {"value", value}});
        markDirty();
        EmitEmittersTreeChanged();
        EmitEngineStateChanged();
        return res;
    }

    // -------- emitters/duplicate-with-index-increment ---------------
    //
    // Legacy `EmitterList_DuplicateEmitter(hWnd, indexDelta)` at
    // [src/UI/EmitterList.cpp:4707]. Duplicate first (same path as
    // above), then shift the TRACK_INDEX track on the duplicate by
    // `delta` via `ShiftIndexTrack` (legacy helper at
    // [src/UI/EmitterList.cpp:2307]). The shift adds `delta` to every
    // keyframe value; if the track is empty, inserts a single key at
    // t=0 with value=delta.
    if (kind == "emitters/duplicate-with-index-increment")
    {
        int id = params.value("id", -1);
        float delta = params.value("delta", 0.0f);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        captureUndo();

        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* dup = nullptr;
        MemoryFile* memfile = new MemoryFile;
        try
        {
            ChunkWriter writer(memfile);
            source->copy(writer);
            memfile->seek(0);
            ChunkReader reader(memfile);
            ParticleSystem::Emitter cleanCopy(reader);
            cleanCopy.name = GenerateDuplicateName(sys, source->name);
            dup = sys->insertEmitterAfter(source, cleanCopy);
        }
        catch (...)
        {
            memfile->Release();
            sendErr("emitter copy failed");
            return res;
        }
        memfile->Release();

        if (dup == nullptr)
        {
            sendErr("insertEmitterAfter returned null");
            return res;
        }

        // Mirror legacy ShiftIndexTrack at [src/UI/EmitterList.cpp:2307].
        // The set's iterators are const, so rebuild via a temporary
        // vector + clear + reinsert.
        if (delta != 0.0f)
        {
            ParticleSystem::Emitter::Track* track =
                dup->tracks[ParticleSystem::TRACK_INDEX];
            if (track->keys.empty())
            {
                track->keys.insert(
                    ParticleSystem::Emitter::Track::Key(0.0f, delta));
            }
            else
            {
                std::vector<ParticleSystem::Emitter::Track::Key> tmp(
                    track->keys.begin(), track->keys.end());
                track->keys.clear();
                for (size_t i = 0; i < tmp.size(); ++i)
                {
                    track->keys.insert(
                        ParticleSystem::Emitter::Track::Key(
                            tmp[i].time, tmp[i].value + delta));
                }
            }
        }

        const int newId = static_cast<int>(dup->index);
        sendOk(json{{"newId", newId}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- engine/action/rescale-emitter -------------------------
    //
    // Per-emitter rescale (vs `engine/action/rescale-system` which
    // walks every emitter). Mirrors the inner loop body of legacy
    // `RescaleEmitter` at src/Rescale.cpp; here we just call
    // DoRescaleEmitter once on the chosen emitter.
    if (kind == "engine/action/rescale-emitter")
    {
        int id = params.value("id", -1);
        float durPct  = params.value("durationScalePercent", 100.0f);
        float sizePct = params.value("sizeScalePercent",     100.0f);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendErr("emitter not found");
            return res;
        }

        captureUndo();
        DoRescaleEmitter(target, durPct / 100.0f, sizePct / 100.0f);

        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- linkGroups/list-exempt-fields -------------------------
    //
    // Read the per-group LinkExemptFlags via
    // `ParticleSystem::getLinkExemptFlags`. Unknown groups return the
    // v1 default exempt set (handled inside getLinkExemptFlags).
    if (kind == "linkGroups/list-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));
        const LinkExemptFlags& flags =
            (*m_pParticleSystem)->getLinkExemptFlags(groupId);
        sendOk(json{{"fields", LinkExemptFlagsToJsonArray(flags)}});
        return res;
    }

    // -------- linkGroups/set-exempt-fields --------------------------
    //
    // Write the per-group exempt set. ParticleSystem normalises an
    // all-default value back out of the map (see
    // [src/ParticleSystem.h:351]); calling with the v1 default fields
    // therefore leaves the on-disk chunk untouched, matching legacy
    // save behaviour.
    if (kind == "linkGroups/set-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));
        const json& fieldsJson =
            params.contains("fields") ? params["fields"] : json::array();
        LinkExemptFlags flags = LinkExemptFlagsFromJsonArray(fieldsJson);

        ParticleSystem* sys = m_pParticleSystem->get();
        const LinkExemptFlags oldFlags = sys->getLinkExemptFlags(groupId);

        captureUndo();
        sys->setLinkExemptFlags(groupId, flags);

        // LNK settings surface — faithful to legacy settings-OK
        // (EmitterList.cpp:2841): when a field transitions exempt→shared and
        // members disagree, resolve it by copying the canonical (members[0],
        // first-in-tree-order) value to every sibling for exactly the
        // newly-shared fields (the diff mask). Only the newly-shared fields
        // are touched, so already-shared params are left as-is. captureUndo()
        // above already snapshotted, so flags + clobbered values fold into
        // ONE undo entry (matches legacy).
        bool resolved = false;
        std::vector<ParticleSystem::Emitter*> members =
            GetLinkGroupMembers(*sys, groupId);
        if (groupId != 0 && members.size() >= 2)
        {
            const LinkExemptFlags diffMask = MakeNewlySharedMask(oldFlags, flags);
            bool anyDisagree = false;
            for (size_t i = 1; i < members.size() && !anyDisagree; ++i)
                if (!DiffNonExemptParams(*members[i], *members[0], diffMask).empty())
                    anyDisagree = true;

            if (anyDisagree)
            {
                for (size_t i = 1; i < members.size(); ++i)
                    members[i]->copySharedParamsFrom(*members[0], diffMask);
                resolved = true;
            }
        }
        //: copySharedParamsFrom reassigns each sibling's non-exempt
        // track multisets, orphaning live particles' cached cursor iterators.
        // Reseat every instance's cursors (the propagateLinkGroup chokepoint
        // pattern). Only fires when we actually copied.
        if (resolved && m_engine != nullptr)
            m_engine->OnParticleSystemChanged(-1);

        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- linkGroups/reset-exempt-fields ------------------------
    //
    // Reset = set to v1 defaults. ParticleSystem normalises that back
    // out of the map, so this effectively erases the per-group entry.
    if (kind == "linkGroups/reset-exempt-fields")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        uint32_t groupId =
            params.value("groupId", static_cast<uint32_t>(0));

        captureUndo();
        (*m_pParticleSystem)->setLinkExemptFlags(groupId, LinkExemptFlags{});

        sendOk(json::object());
        markDirty();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- linkGroups/diff-membership () --------------------
    //
    // Read-only preview of a would-be join's non-exempt field
    // disagreements, so the UI can warn before set-membership silently
    // clobbers them. Mirrors the set-membership branches EXACTLY so the
    // warning lists precisely the fields that handler would overwrite:
    //   - groupId null/0 (leave): nothing is overwritten → no conflicts.
    //   - groupId  >  0 and the group EXISTS (join): canonical =
    //     members[0]; exempt = the group's flags; every target not
    //     already in the group is diffed against the canonical
    //     (matches JoinLinkGroup, which copies each joiner from members[0]
    //     under the group's exempt set).
    //   - groupId  >  0 but the group is empty, or groupId == -1 (new
    //     group): canonical = the first resolved target; exempt = v1
    //     defaults; the remaining targets are diffed against it (matches
    //     set-membership's create paths).
    // No mutation, no undo capture, no events fired.
    if (kind == "linkGroups/diff-membership")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }

        ParticleSystem* sys = m_pParticleSystem->get();

        int groupIdRaw = 0;
        if (params.contains("groupId") && !params["groupId"].is_null())
            groupIdRaw = params["groupId"].get<int>();

        const json& idsJson =
            params.contains("ids") ? params["ids"] : json::array();

        // Resolve (wire-id, emitter) pairs in caller order. The wire id
        // is echoed back in each conflict so the UI can attribute it.
        std::vector<std::pair<int, ParticleSystem::Emitter*>> targets;
        for (const auto& v : idsJson)
        {
            int id = v.get<int>();
            ParticleSystem::Emitter* e = getEmitterById(id);
            if (e != nullptr) targets.emplace_back(id, e);
        }

        // Determine the canonical member, the exempt set, and the joiners
        // to diff — exactly as set-membership would.
        ParticleSystem::Emitter* canonical = nullptr;
        const LinkExemptFlags*   exempt    = nullptr;
        std::vector<std::pair<int, ParticleSystem::Emitter*>> joiners;

        if (groupIdRaw > 0)
        {
            uint32_t target = static_cast<uint32_t>(groupIdRaw);
            std::vector<ParticleSystem::Emitter*> members =
                GetLinkGroupMembers(*sys, target);
            if (!members.empty())
            {
                canonical = members[0];
                exempt    = &sys->getLinkExemptFlags(target);
                for (const auto& t : targets)
                    if (t.second->linkGroup != target)
                        joiners.push_back(t);
            }
            else if (!targets.empty())
            {
                canonical = targets[0].second;
                exempt    = &GetDefaultLinkExemptFlags();
                for (size_t i = 1; i < targets.size(); ++i)
                    joiners.push_back(targets[i]);
            }
        }
        else if (groupIdRaw == -1 && !targets.empty())
        {
            canonical = targets[0].second;
            exempt    = &GetDefaultLinkExemptFlags();
            for (size_t i = 1; i < targets.size(); ++i)
                joiners.push_back(targets[i]);
        }
        // groupIdRaw == 0 (leave): canonical stays null → no conflicts.

        json conflicts = json::array();
        if (canonical != nullptr && exempt != nullptr)
        {
            for (const auto& j : joiners)
            {
                std::vector<std::string> fields =
                    DiffNonExemptParams(*j.second, *canonical, *exempt);
                if (!fields.empty())
                {
                    json entry;
                    entry["id"]     = j.first;
                    entry["fields"] = fields;
                    conflicts.push_back(entry);
                }
            }
        }

        sendOk(json{{"conflicts", conflicts}});
        return res;
    }

    // -------- linkGroups/diff-exempt-change (LNK settings surface) ---
    //
    // Read-only preview for the settings dialog: which existing members the
    // PROPOSED exempt set would overwrite to the canonical (members[0],
    // first-in-tree-order) value when a now-exempt field becomes SHARED.
    // Mirrors the legacy settings-OK disagreement scan (EmitterList.cpp:2841):
    // only fields transitioning exempt(stored)→shared(proposed) count, diffed
    // per non-canonical member. set-exempt-fields resolves it on commit.
    // No mutation, no undo, no events.
    if (kind == "linkGroups/diff-exempt-change")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        ParticleSystem* sys = m_pParticleSystem->get();

        uint32_t groupId = params.value("groupId", static_cast<uint32_t>(0));
        const json& exemptJson =
            params.contains("exempt") ? params["exempt"] : json::array();
        const LinkExemptFlags proposed = LinkExemptFlagsFromJsonArray(exemptJson);

        json conflicts = json::array();
        std::vector<ParticleSystem::Emitter*> members =
            GetLinkGroupMembers(*sys, groupId);
        if (groupId != 0 && members.size() >= 2)
        {
            const LinkExemptFlags  oldFlags = sys->getLinkExemptFlags(groupId);
            const LinkExemptFlags  diffMask = MakeNewlySharedMask(oldFlags, proposed);

            const auto& allEmitters = sys->getEmitters();
            auto wireIdOf = [&](ParticleSystem::Emitter* e) -> int {
                for (size_t i = 0; i < allEmitters.size(); ++i)
                    if (allEmitters[i] == e) return static_cast<int>(i);
                return -1;
            };

            ParticleSystem::Emitter* canonical = members[0];
            for (size_t i = 1; i < members.size(); ++i)
            {
                std::vector<std::string> fields =
                    DiffNonExemptParams(*members[i], *canonical, diffMask);
                if (!fields.empty())
                {
                    json entry;
                    entry["id"]     = wireIdOf(members[i]);
                    entry["fields"] = fields;
                    conflicts.push_back(entry);
                }
            }
        }

        sendOk(json{{"conflicts", conflicts}});
        return res;
    }

    // -------- Screen 4 Batch B2 — add child / move / link-group memb -

    // -------- emitters/add-lifetime-child ---------------------------
    //
    // Wraps `ParticleSystem::addLifetimeEmitter(parent, Emitter())`.
    // The engine refuses (returns NULL) when the parent's lifetime
    // slot is already filled — surface that as `newId: -1`. Otherwise
    // return the new emitter's index in getEmitters().
    if (kind == "emitters/add-lifetime-child")
    {
        int parentId = params.value("parentId", -1);
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addLifetimeEmitter(parent);
        if (child == nullptr)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        sendOk(json{{"newId", static_cast<int>(child->index)}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/add-root --------------------------------------
    //
    // Phase 4.1 Fix dispatch 5 — wraps `ParticleSystem::addRootEmitter()`
    // for the new top-level Emitters → New Emitter → Root menu item.
    // The engine always succeeds (no max-roots cap); the only failure
    // path is a missing particle-system pointer, surfaced as
    // `newId: -1` for parity with the add-child handlers.
    if (kind == "emitters/add-root")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addRootEmitter();
        if (child == nullptr)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        sendOk(json{{"newId", static_cast<int>(child->index)}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/add-death-child -------------------------------
    if (kind == "emitters/add-death-child")
    {
        int parentId = params.value("parentId", -1);
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        captureUndo();
        ParticleSystem::Emitter* child =
            (*m_pParticleSystem)->addDeathEmitter(parent);
        if (child == nullptr)
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        sendOk(json{{"newId", static_cast<int>(child->index)}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/move ------------------------------------------
    //
    // Reorder the emitter among its siblings. Wraps
    // `ParticleSystem::moveEmitter(emitter, direction)`. The engine
    // already enforces "root-only" and "no-op at the edges" — a
    // refused move returns false and is a silent no-op here (the React
    // side disables the menu item at the edges; reaching this path with
    // a refusal is defensive). Always sends `{}` to match the schema.
    if (kind == "emitters/move")
    {
        int id = params.value("id", -1);
        std::string dirStr = params.value("direction", std::string{"up"});
        int dir = (dirStr == "down") ? +1 : -1;
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json::object());
            return res;
        }
        captureUndo();
        const bool moved = (*m_pParticleSystem)->moveEmitter(target, dir);
        sendOk(json::object());
        if (moved)
        {
            markDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return res;
    }

    // -------- emitters/move-many -------------------------------------
    //
    // Batch reorder: move the selected ROOT emitters up/down by one as a UNIT
    // (non-root selections are no-ops — moveEmitter is root-only). Order is
    // preserved: if the edge-most selected root is pinned at the edge, NOTHING
    // moves (the block doesn't deform by compacting trailing members past the
    // non-selected roots). Returns `newIds` (targets' final indices,
    // input-order-aligned) read from the stable pointers afterwards, so the
    // React selection follows the reorder.
    if (kind == "emitters/move-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"newIds", json::array()}});
            return res;
        }
        const std::string dirStr = params.value("direction", std::string{"up"});
        const int dir = (dirStr == "down") ? +1 : -1;

        std::vector<ParticleSystem::Emitter*> targets;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e != nullptr) targets.push_back(e);
            }
        }
        if (targets.empty())
        {
            sendOk(json{{"newIds", json::array()}});
            return res;
        }

        ParticleSystem* sys = m_pParticleSystem->get();
        auto isSel = [&](ParticleSystem::Emitter* e) {
            return std::find(targets.begin(), targets.end(), e) != targets.end();
        };

        // Roots in vector order.
        std::vector<ParticleSystem::Emitter*> roots;
        for (ParticleSystem::Emitter* e : sys->getEmitters())
            if (e != nullptr && e->parent == nullptr) roots.push_back(e);

        // Preserve order: the selection moves as a UNIT, or not at all. If the
        // edge-most root in the move direction is selected, the block is pinned
        // against the edge and NOTHING moves — rather than letting the trailing
        // members compact past the non-selected roots (order matters in a
        // particle system). Otherwise every selected root shifts by one,
        // processed ascending (up) / descending (down) so each one's neighbour
        // is already an unselected root when it swaps.
        std::vector<ParticleSystem::Emitter*> movable;
        const bool edgePinned =
            !roots.empty() && isSel(dir == -1 ? roots.front() : roots.back());
        if (!edgePinned)
        {
            if (dir == -1)
            {
                for (size_t i = 0; i < roots.size(); ++i)
                    if (isSel(roots[i])) movable.push_back(roots[i]);   // ascending
            }
            else
            {
                for (size_t i = roots.size(); i-- > 0; )
                    if (isSel(roots[i])) movable.push_back(roots[i]);   // descending
            }
        }

        if (!movable.empty()) captureUndo();
        bool anyMoved = false;
        for (ParticleSystem::Emitter* e : movable)
            if (sys->moveEmitter(e, dir)) anyMoved = true;

        json newIds = json::array();
        for (ParticleSystem::Emitter* e : targets)
            newIds.push_back(static_cast<int>(e->index));

        sendOk(json{{"newIds", newIds}});
        if (anyMoved)
        {
            markDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return res;
    }

    // -------- emitters/set-visible -----------------------------------
    //
    // (Group A): per-emitter visibility toggle for the EmitterTree
    // panel toolbar's [👁] button. Sets `Emitter::visible` for the
    // target only — children are untouched. `visible` is editor-only
    // state (not persisted to the .alo file), so this handler does NOT
    // markDirty. Still emits tree-changed + state-changed so the engine
    // re-renders and any open inspector reflects the new flag.
    if (kind == "emitters/set-visible")
    {
        int  id      = params.value("id", -1);
        bool visible = params.value("visible", true);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            sendOk(json::object());
            return res;
        }
        target->visible = visible;
        sendOk(json::object());
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/set-all-visible -------------------------------
    //
    // (Group A): bulk Show All / Hide All from the EmitterTree
    // panel toolbar. Walks the entire emitter array (the engine stores
    // all emitters flat with parent pointers — no recursion needed)
    // and sets `visible` uniformly. Same editor-only semantic as
    // set-visible above; no markDirty.
    if (kind == "emitters/set-all-visible")
    {
        bool visible = params.value("visible", true);
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json::object());
            return res;
        }
        const auto& emitters = (*m_pParticleSystem)->getEmitters();
        for (ParticleSystem::Emitter* e : emitters)
        {
            if (e != nullptr) e->visible = visible;
        }
        sendOk(json::object());
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- linkGroups/set-membership ------------------------------
    //
    // Assign each emitter in `ids` to a link group:
    //   - groupId === null OR === 0 → leave the group
    //   - groupId  >  0             → join that existing group
    //   - groupId === -1            → create a new group
    //
    // F4: this drives the LinkGroup.h API (Create/Join/Leave) rather
    // than stamping `e->linkGroup` raw. That stamp set the membership ID
    // (so the bracket gutter drew) but NEVER synchronised the members'
    // non-exempt fields, so the group had no behavioural effect — the
    // root cause of "link groups don't work". Create/Join overwrite each
    // member's non-exempt params from the canonical member (first in
    // tree order for a new group; the group's canonical member for a
    // join), and Leave auto-dissolves a group left with one member.
    // Members already in a different group are detached first so the
    // operation always succeeds (CreateLinkGroup refuses if any member
    // is still grouped). The pre-mutation captureUndo() snapshots the
    // whole system, so one Ctrl+Z restores the prior membership AND the
    // pre-sync field values.
    if (kind == "linkGroups/set-membership")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendErr("particle system not bound");
            return res;
        }
        const json& idsJson =
            params.contains("ids") ? params["ids"] : json::array();
        // `groupId` may be a JSON number or null. Absent/null → 0 (leave).
        int groupIdRaw = 0;
        if (params.contains("groupId") && !params["groupId"].is_null())
        {
            groupIdRaw = params["groupId"].get<int>();
        }

        ParticleSystem* sys = m_pParticleSystem->get();

        // Resolve the target emitters once.
        std::vector<ParticleSystem::Emitter*> targets;
        for (const auto& v : idsJson)
        {
            ParticleSystem::Emitter* e = getEmitterById(v.get<int>());
            if (e != nullptr) targets.push_back(e);
        }

        captureUndo();

        if (groupIdRaw == 0)
        {
            // Leave / unlink.
            for (size_t i = 0; i < targets.size(); ++i)
                LeaveLinkGroup(*sys, targets[i]);
        }
        else if (groupIdRaw > 0)
        {
            // Explicit positive id. If the group already exists, JOIN each
            // target to it (joiners adopt the group's canonical values).
            // If it does NOT exist yet, CREATE it with this caller-chosen
            // id — `JoinLinkGroup` refuses a non-existent group, and the
            // contract (and the legacy stamp) is "assign to that group,
            // creating it if needed". The real dialog only sends positive
            // ids for existing groups, but bridge callers (and tests) rely
            // on create-if-needed.
            uint32_t target = static_cast<uint32_t>(groupIdRaw);
            const bool exists = !GetLinkGroupMembers(*sys, target).empty();
            // Detach any target currently in a *different* group first.
            for (size_t i = 0; i < targets.size(); ++i)
            {
                if (targets[i]->linkGroup != 0 && targets[i]->linkGroup != target)
                    LeaveLinkGroup(*sys, targets[i]);
            }
            if (exists)
            {
                for (size_t i = 0; i < targets.size(); ++i)
                    if (targets[i]->linkGroup != target)
                        JoinLinkGroup(*sys, targets[i], target);
            }
            else if (!targets.empty())
            {
                // New group with an explicit id: first target is canonical;
                // the rest sync their non-exempt params to it (mirrors
                // CreateLinkGroup, which only allocates max+1 ids).
                const LinkExemptFlags& exempt = sys->getLinkExemptFlags(target);
                targets[0]->linkGroup = target;
                for (size_t i = 1; i < targets.size(); ++i)
                {
                    targets[i]->copySharedParamsFrom(*targets[0], exempt);
                    targets[i]->linkGroup = target;
                }
            }
        }
        else // groupIdRaw == -1 : new group
        {
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i]->linkGroup != 0)
                    LeaveLinkGroup(*sys, targets[i]);
            // Minimum group size is 2; a 1-id "new group" is a no-op.
            if (targets.size() >= 2)
                CreateLinkGroup(*sys, targets);
        }

        // Idempotent safety net — the API already preserves the
        // "no singleton groups" invariant, but a defensive sweep keeps
        // any future caller honest.
        EnforceSingleMemberLinkGroups();

        //: Join/Create call `copySharedParamsFrom`, which REPLACES
        // each joining member's non-exempt track multisets with copies from
        // the canonical member. Any live particle of those members holds
        // cached cursor iterators into the OLD containers — now orphaned —
        // and the next Engine::Update would dereference a dangling iterator
        // (the xtree:181 "value-initialized iterator" assert). The legacy
        // key-edit handlers reseat per-track; a membership change can touch
        // EVERY non-exempt track on MULTIPLE members, so reseat all cursors
        // for all instances (-1). Cheap (re-finds cursors) and idempotent.
        if (m_engine != nullptr)
            m_engine->OnParticleSystemChanged(-1);

        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/drop (Screen 4 Batch B3) ----------------------
    //
    // Drag-and-drop reorder + reparent. Tagged-union on params.mode:
    //   - "reorder":  wraps `ParticleSystem::moveEmitterToRootIndex`.
    //                 `rootIndex` is the gap index in the rendered root
    //                 list (gap K = "land before position K"). The
    //                 engine refuses non-root sources, out-of-range
    //                 gaps, and no-op gaps (sourceIdx and sourceIdx+1)
    //                 by returning false; surface that as
    //                 `{ ok: false, error: "reorder refused" }`.
    //   - "reparent": wraps `ParticleSystem::reparentEmitter`. The
    //                 engine itself checks cycle, same-parent, and
    //                 slot-full — refusal returns false; surface as
    //                 `{ ok: false, error: "reparent refused" }`.
    //
    // React side resolves slot before calling (auto-pick: both free →
    // "lifetime"; only one free → that one; both filled → no bridge
    // call). The wire shape never carries "auto".
    if (kind == "emitters/drop")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return res;
        }
        std::string mode = params.value("mode", std::string{});
        int id = params.value("id", -1);
        ParticleSystem::Emitter* source = getEmitterById(id);
        if (source == nullptr)
        {
            sendOk(json{{"ok", false}, {"error", "source emitter not found"}});
            return res;
        }
        // After a successful drop the dragged emitter's positional index has
        // changed; re-select it (by its post-op index) so the highlight FOLLOWS
        // the moved emitter rather than sticking on the slot it left behind
        // (which now holds a different emitter). The web side leans on the
        // emitters/selected event this emits. `source` is the same Emitter
        // object across the move, so its new index is its position in the
        // (now-reordered) flat emitter vector.
        auto reselectMovedEmitter = [&]() {
            const auto& es = (*m_pParticleSystem)->getEmitters();
            for (size_t i = 0; i < es.size(); ++i)
            {
                if (es[i] == source)
                {
                    m_selectedEmitterId = static_cast<int>(i);
                    // emitters/selected event — same narrow payload the
                    // emitters/select handler emits; EmitterTree syncs primary.
                    if (m_emit)
                    {
                        json env = {
                            {"type",    "evt"},
                            {"kind",    "emitters/selected"},
                            {"payload", json{{"id", json(m_selectedEmitterId)}}},
                        };
                        m_emit(env.dump());
                    }
                    break;
                }
            }
        };
        captureUndo();
        if (mode == "reorder")
        {
            int rootIndex = params.value("rootIndex", -1);
            if (rootIndex < 0)
            {
                sendOk(json{{"ok", false}, {"error", "invalid rootIndex"}});
                return res;
            }
            const bool ok = (*m_pParticleSystem)->moveEmitterToRootIndex(
                source, static_cast<size_t>(rootIndex));
            if (!ok)
            {
                sendOk(json{{"ok", false}, {"error", "reorder refused"}});
                return res;
            }
            sendOk(json{{"ok", true}});
            markDirty();
            reselectMovedEmitter();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
            return res;
        }
        if (mode == "reparent")
        {
            int targetId = params.value("targetId", -1);
            std::string slot = params.value("slot", std::string{"lifetime"});
            ParticleSystem::Emitter* target = getEmitterById(targetId);
            if (target == nullptr)
            {
                sendOk(json{{"ok", false}, {"error", "target emitter not found"}});
                return res;
            }
            const bool useDuringLife = (slot != "death");
            const bool ok = (*m_pParticleSystem)->reparentEmitter(
                source, target, useDuringLife);
            if (!ok)
            {
                sendOk(json{{"ok", false}, {"error", "reparent refused"}});
                return res;
            }
            sendOk(json{{"ok", true}});
            markDirty();
            reselectMovedEmitter();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
            return res;
        }
        sendOk(json{{"ok", false}, {"error", "unknown mode"}});
        return res;
    }

    // -------- emitters/reorder-many (multi-select drag-reorder) ------
    //
    // Batch absolute-position root reorder. Moves the selected ROOT
    // emitters (params.ids, positional) to land contiguous at gap
    // params.rootIndex, preserving tree order; non-contiguous selections
    // collapse. Wraps ParticleSystem::reorderManyRootsToIndex, which refuses
    // out-of-range / non-root / empty / own-footprint no-op. Returns the
    // moved roots' final indices as newIds (a contiguous run).
    if (kind == "emitters/reorder-many")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"ok", false}, {"error", "particle system not bound"}});
            return res;
        }
        int rootIndex = params.value("rootIndex", -1);
        if (rootIndex < 0)
        {
            sendOk(json{{"ok", false}, {"error", "invalid rootIndex"}});
            return res;
        }
        std::vector<ParticleSystem::Emitter*> selection;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& j : params["ids"])
            {
                ParticleSystem::Emitter* e =
                    getEmitterById(j.is_number_integer() ? j.get<int>() : -1);
                if (e == nullptr)
                {
                    sendOk(json{{"ok", false}, {"error", "emitter not found"}});
                    return res;
                }
                if (e->parent != nullptr)
                {
                    sendOk(json{{"ok", false}, {"error", "non-root in selection"}});
                    return res;
                }
                selection.push_back(e);
            }
        }
        if (selection.empty())
        {
            sendOk(json{{"ok", false}, {"error", "empty selection"}});
            return res;
        }
        captureUndo();
        std::vector<size_t> outNewIds;
        const bool ok = (*m_pParticleSystem)->reorderManyRootsToIndex(
            selection, static_cast<size_t>(rootIndex), outNewIds);
        if (!ok)
        {
            sendOk(json{{"ok", false}, {"error", "reorder refused"}});
            return res;
        }
        json newIds = json::array();
        for (size_t v : outNewIds) newIds.push_back(static_cast<int>(v));
        sendOk(json{{"ok", true}, {"newIds", newIds}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- emitters/copy / cut / paste (Screen 4 Batch C) --------
    //
    // Process-local clipboard. We reuse the existing import-from-
    // file serialise pattern: per emitter, allocate a MemoryFile, wrap
    // it with a ChunkWriter, call `Emitter::copy(writer)` (which is
    // `write(writer, true)` — preserves identity-less form), then
    // snapshot the bytes into a `std::vector<uint8_t>`. Paste reverses
    // the round-trip via `Emitter(ChunkReader&)`. One buffer per copied
    // subtree so each can be deserialised independently (multi-id paste
    // produces multiple new roots).
    //
    // Cut = copy + delete. Single undo capture at the start, single
    // tree-changed at the end — the user sees one atomic step in undo.
    // Descending-id delete order keeps lower indices valid through the
    // loop (deleteEmitter shifts everything above the deleted index
    // down by one).
    if (kind == "emitters/copy" || kind == "emitters/cut")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json::object());
            return res;
        }
        // Pull the id list from params.ids (an array of numbers).
        std::vector<int> ids;
        if (params.contains("ids") && params["ids"].is_array())
        {
            for (const auto& v : params["ids"])
            {
                if (v.is_number_integer()) ids.push_back(v.get<int>());
            }
        }
        // Clear the clipboard before refilling — every copy/cut
        // replaces the entire contents.
        m_emitterClipboard.clear();
        for (int id : ids)
        {
            ParticleSystem::Emitter* source = getEmitterById(id);
            if (source == nullptr) continue;
            MemoryFile* memfile = new MemoryFile;
            try
            {
                ChunkWriter writer(memfile);
                source->copy(writer);
                std::vector<uint8_t> buf(memfile->size());
                memfile->seek(0);
                if (!buf.empty())
                {
                    memfile->read(buf.data(), static_cast<unsigned long>(buf.size()));
                }
                m_emitterClipboard.push_back(std::move(buf));
            }
            catch (...)
            {
                // Best-effort: skip this id and continue with the rest.
            }
            memfile->Release();
        }
        if (kind == "emitters/copy")
        {
            // Read-only — no undo, no dirty, no tree-changed.
            sendOk(json::object());
            return res;
        }
        // ---- cut: delete the originals atomically ----
        captureUndo();
        // Sort ids descending so the iteration is robust against any
        // mid-loop index reshuffling. We also re-resolve each id via
        // getEmitterById inside the loop because the legacy
        // `deleteEmitter` shifts subsequent slots down, invalidating
        // raw pointers across calls.
        std::sort(ids.begin(), ids.end(), std::greater<int>());
        ParticleSystem* sys = m_pParticleSystem->get();
        bool clearedSelection = false;
        for (int id : ids)
        {
            ParticleSystem::Emitter* target = getEmitterById(id);
            if (target == nullptr) continue;
            if (m_selectedEmitterId == id) clearedSelection = true;
            sys->deleteEmitter(target);
        }
        if (clearedSelection)
        {
            m_selectedEmitterId = -1;
            if (m_emit)
            {
                json env = {
                    {"type",    "evt"},
                    {"kind",    "emitters/selected"},
                    {"payload", json{{"id", json(nullptr)}}},
                };
                m_emit(env.dump());
            }
        }
        sendOk(json::object());
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }
    if (kind == "emitters/paste")
    {
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            sendOk(json{{"newIds", json::array()}});
            return res;
        }
        if (m_emitterClipboard.empty())
        {
            // Nothing to paste — silent no-op, no dirty, no undo.
            sendOk(json{{"newIds", json::array()}});
            return res;
        }
        // Optional `afterId` — the root the paste should land directly
        // after. When omitted or not a current root, paste at the end
        // of the root list.
        int afterId = -1;
        if (params.contains("afterId") && !params["afterId"].is_null())
        {
            afterId = params.value("afterId", -1);
        }
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        json newIds = json::array();
        ParticleSystem::Emitter* prevAnchor = (afterId >= 0)
            ? getEmitterById(afterId)
            : nullptr;
        // Track failure separately so a partial paste still emits one
        // tree-changed and returns the ids that *did* land.
        for (auto& buf : m_emitterClipboard)
        {
            if (buf.empty()) continue;
            MemoryFile* memfile = new MemoryFile;
            ParticleSystem::Emitter* pasted = nullptr;
            try
            {
                memfile->write(buf.data(),
                               static_cast<unsigned long>(buf.size()));
                memfile->seek(0);
                ChunkReader reader(memfile);
                ParticleSystem::Emitter staging(reader);
                staging.name = GenerateDuplicateName(sys, staging.name);
                if (prevAnchor != nullptr)
                {
                    pasted = sys->insertEmitterAfter(prevAnchor, staging);
                }
                else
                {
                    pasted = sys->addRootEmitter(staging);
                }
            }
            catch (...)
            {
                // Skip this entry; continue with the rest.
            }
            memfile->Release();
            if (pasted != nullptr)
            {
                newIds.push_back(static_cast<int>(pasted->index));
                // Chain subsequent pastes after this one so multi-id
                // paste keeps clipboard order.
                prevAnchor = pasted;
            }
        }
        sendOk(json{{"newIds", newIds}});
        if (!newIds.empty())
        {
            markDirty();
            EmitEngineStateChanged();
            EmitEmittersTreeChanged();
        }
        return res;
    }
    // -------- emitters/paste-as-child (legacy Paste As ▸) -----------
    //
    // Deserialise the FIRST clipboard buffer and attach it into the
    // parent's lifetime or death child slot — the splice of the
    // emitters/paste deser (above) and the add-lifetime/death-child
    // attach. `addLifetimeEmitter`/`addDeathEmitter` self-guard (return
    // NULL) when the slot is already filled, so a stale menu can't
    // double-occupy. One emitter per slot: a multi-buffer clipboard
    // pastes only buffer[0] (matches legacy's single-blob clipboard).
    if (kind == "emitters/paste-as-child")
    {
        int parentId = params.value("parentId", -1);
        std::string slot = params.value("slot", std::string());
        ParticleSystem::Emitter* parent = getEmitterById(parentId);
        if (parent == nullptr || m_pParticleSystem == nullptr || !*m_pParticleSystem
            || m_emitterClipboard.empty() || m_emitterClipboard.front().empty())
        {
            sendOk(json{{"newId", -1}});
            return res;
        }
        captureUndo();
        ParticleSystem* sys = m_pParticleSystem->get();
        ParticleSystem::Emitter* child = nullptr;
        MemoryFile* memfile = new MemoryFile;
        try
        {
            auto& buf = m_emitterClipboard.front();
            memfile->write(buf.data(), static_cast<unsigned long>(buf.size()));
            memfile->seek(0);
            ChunkReader reader(memfile);
            ParticleSystem::Emitter staging(reader);
            staging.name = GenerateDuplicateName(sys, staging.name);
            child = (slot == "death")
                ? sys->addDeathEmitter(parent, staging)
                : sys->addLifetimeEmitter(parent, staging);
        }
        catch (...)
        {
            // Deser failed — fall through to the null-child refusal.
        }
        memfile->Release();
        if (child == nullptr)
        {
            // Slot occupied or deser threw. captureUndo already ran —
            // parity with add-lifetime-child, which also captures before
            // this null-check.
            sendOk(json{{"newId", -1}});
            return res;
        }
        sendOk(json{{"newId", static_cast<int>(child->index)}});
        markDirty();
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return res;
    }

    // -------- everything else (emitters/* etc.) ---------------------
    sendErr("not implemented yet (Phase 3+)");
    return res;
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

void BridgeDispatcher::EmitEmittersTreeChanged()
{
    if (!m_emit) return;
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

void BridgeDispatcher::EmitEngineStateChanged()
{
    if (!m_emit || !m_engine) return;
    json spawnerJson = m_spawnerDriver
        ? SpawnerConfigToJson(m_spawnerDriver->GetConfig())
        : m_spawnerConfig;
    const std::wstring& activeModPath = m_modManager ? m_modManager->GetSelectedModPath() : std::wstring();
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
                                         size_t selIdx)
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
// see ROADMAP §1.1 for the enumeration) AND from file/open
// after binding a loaded ParticleSystem (so pre-saved files
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
            // reflects the disabled state.
            if (m_spawnerDriver)
            {
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
    // T9] Gate test-driven freeze. When frozen, the React
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

} // namespace host
