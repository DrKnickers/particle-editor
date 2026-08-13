#pragma once
//
// Declarations for the file-scope helpers that DispatchInternal's ladder
// blocks call, shared between BridgeDispatcher.cpp and the per-domain
// dispatch TUs (BridgeDispatch_*.cpp). Bridge-side counterpart of the
// engine_internal.h rule in tasks/2026-07-06-heavyweight-refactor-plan.md:
// every helper keeps exactly ONE production definition. Most live in
// BridgeDispatcher.cpp; cohesive extracted helpers may live in a dedicated
// production TU such as RecentFiles.cpp. Never add a per-TU static copy.
// Helpers used only by BridgeDispatcher.cpp's own dispatch plumbing
// (BuildDispatchExceptionEnvelope, JsonStringField, EndDispatchSpan) stay
// static there and are deliberately absent here.

#include <cstddef>
#include <string>
#include <vector>

#include <windows.h>          // COLORREF (self-contained; don't rely on include order)

#include "third_party/nlohmann/json.hpp"

#include "../engine.h"        // Engine::Camera/Light/LightType, ReferenceObjectStatus, SkydomeSlotStatus, D3DX types
#include "../LinkGroup.h"     // LinkExemptFlags
#include "../SpawnerDriver.h" // SpawnerConfig (returned by value)

class ParticleSystem;

// Emitter duplicate-name helper, implemented in src/main.cpp.
extern std::string GenerateDuplicateName(const ParticleSystem* system,
                                         const std::string& sourceName);

// CSS color string -> COLORREF (global scope — defined above namespace host
// in BridgeDispatcher.cpp).
bool ParseCssColorToColorRef(const std::string& in, COLORREF& out);

namespace host {

// HKCU key under which ALL editor state persists (recent files, view state,
// spawner config, lighting). Mirrors legacy main.cpp's registry layout.
constexpr const wchar_t* kRegistryKeyPath = L"Software\\AloParticleEditor";

// One-value registry writers for the settings key above. Every Persist* used
// to hand-open/write/close the key with the identical RegCreateKeyExW
// boilerplate (14 copies across three files); these own the open flags in one
// place. Failure is deliberately silent, matching the old blocks: a failed
// settings write must never interrupt an edit. Empty string => delete the
// value (mirror legacy: clearing a slot deletes it).
inline HKEY OpenSettingsKeyForWrite()
{
    HKEY hKey = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    return hKey;   // nullptr on failure; the writers below no-op
}
inline void WriteRegDword(const wchar_t* name, DWORD v)
{
    if (HKEY k = OpenSettingsKeyForWrite())
    {
        RegSetValueExW(k, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(k);
    }
}
inline void WriteRegSz(const wchar_t* name, const std::wstring& s)
{
    if (HKEY k = OpenSettingsKeyForWrite())
    {
        if (s.empty())
            RegDeleteValueW(k, name);
        else
            RegSetValueExW(k, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(s.c_str()),
                           static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
inline void WriteRegBinary(const wchar_t* name, const void* data, DWORD bytes)
{
    if (HKEY k = OpenSettingsKeyForWrite())
    {
        RegSetValueExW(k, name, 0, REG_BINARY,
                       static_cast<const BYTE*>(data), bytes);
        RegCloseKey(k);
    }
}

// Raw response-envelope builders (async Dispatch path).
std::string BuildOkResponse(const std::string& id, const nlohmann::json& data);
std::string BuildErrResponse(const std::string& id, const std::string& error);

// JSON <-> native converters.
nlohmann::json Vec3ToJson(const D3DXVECTOR3& v);
nlohmann::json Vec4ToJson(const D3DXVECTOR4& v);
nlohmann::json CameraToJson(const Engine::Camera& c);
nlohmann::json LightToJson(const Engine::Light& l);
D3DXVECTOR3 JsonToVec3(const nlohmann::json& j);
D3DXVECTOR4 JsonToVec4(const nlohmann::json& j);
Engine::LightType ParseLightWhich(const std::string& s);
const char* RefStatusToString(ReferenceObjectStatus s);
const char* SkyStatusToString(SkydomeSlotStatus s);

// Registry-backed persistence (HKCU\Software\AloParticleEditor).
std::vector<std::wstring> ReadRecentFiles();
std::vector<std::wstring> WriteRecentFile(const std::wstring& path);
void PersistSkydomeIndex(int value);
void PersistSkydomeCustomPath(int slot, const std::wstring& path);
void PersistSkydomeEnvironment(int context, const std::wstring& primaryName,
                               const std::wstring& secondaryName);
void PersistBackgroundColor(COLORREF color);
void PersistShowGround(bool enabled);
void PersistReferenceObjectName(const std::wstring& name);
void PersistReferenceObjectVisible(bool visible);
void PersistReferenceObjectLock(bool locked);
void PersistReferenceObjectTransform(const D3DXVECTOR3& pos, const D3DXVECTOR3& rot);
void PersistGrid(bool visible, float spacing);
void PersistSnap(bool enabled);

// Link-group DTO plumbing.
nlohmann::json LinkExemptFlagsToJsonArray(const LinkExemptFlags& flags);
LinkExemptFlags LinkExemptFlagsFromJsonArray(const nlohmann::json& arr);
LinkExemptFlags MakeNewlySharedMask(const LinkExemptFlags& oldFlags,
                                    const LinkExemptFlags& proposed);

// Tree / state snapshot builders.
// `depth` is the recursion backstop (2026-07 audit) and is not part of
// the wire shape -- callers always start a root at 0. ValidateEmitterGraph caps
// chain depth at load/import, but it is never called from the bridge mutation
// path, so the serializer refuses to recurse past the same cap on its own.
nlohmann::json BuildEmitterTreeNode(const ParticleSystem* sys, std::size_t idx,
                                    std::size_t depth = 0);
nlohmann::json BuildEngineStateSnapshot(Engine* engine,
                                        const std::wstring& currentFilePath,
                                        bool dirty,
                                        const nlohmann::json& spawnerConfig,
                                        int selectedEmitterId,
                                        const std::wstring& activeModPath,
                                        bool leaveParticles,
                                        bool canUndo,
                                        bool canRedo);

} // namespace host
