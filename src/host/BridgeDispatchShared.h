#pragma once
//
// Declarations for the file-scope helpers that DispatchInternal's ladder
// blocks call, shared between BridgeDispatcher.cpp and the per-domain
// dispatch TUs (BridgeDispatch_*.cpp). Bridge-side counterpart of the
// engine_internal.h rule in tasks/2026-07-06-heavyweight-refactor-plan.md:
// every helper keeps exactly ONE definition (in BridgeDispatcher.cpp) —
// never a per-TU static copy. Helpers used only by BridgeDispatcher.cpp's
// own dispatch plumbing (BuildDispatchExceptionEnvelope, JsonStringField,
// EndDispatchSpan) stay static there and are deliberately absent here.

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
void PersistReferenceObjectName(const std::wstring& name);
void PersistReferenceObjectVisible(bool visible);
void PersistReferenceObjectLock(bool locked);
void PersistReferenceObjectTransform(const D3DXVECTOR3& pos, const D3DXVECTOR3& rot);
void PersistGrid(bool visible, float spacing);
void PersistSnap(bool enabled);

// Spawner config plumbing.
SpawnerConfig JsonToSpawnerConfig(const nlohmann::json& j);
nlohmann::json SpawnerConfigToJson(const SpawnerConfig& cfg);
nlohmann::json DefaultSpawnerConfigJson();

// Link-group DTO plumbing.
nlohmann::json LinkExemptFlagsToJsonArray(const LinkExemptFlags& flags);
LinkExemptFlags LinkExemptFlagsFromJsonArray(const nlohmann::json& arr);
LinkExemptFlags MakeNewlySharedMask(const LinkExemptFlags& oldFlags,
                                    const LinkExemptFlags& proposed);

// Tree / state snapshot builders.
nlohmann::json BuildEmitterTreeNode(const ParticleSystem* sys, std::size_t idx);
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
