#pragma once

#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <filesystem>
#include "ParticleReplayController.h"
#include "host/third_party/nlohmann/json.hpp"

class Engine;
class ParticleSystem;
class UndoStack;

namespace host {

class AlphaCompositor;
class LayoutBroker;
class BridgeDispatcher;

struct ReplayRunDeps
{
    std::unique_ptr<Engine>& engine;
    std::unique_ptr<ParticleSystem>& particleSystem;
    UndoStack& undoStack;
    LayoutBroker& layout;
    std::unique_ptr<AlphaCompositor>& alphaCompositor;
    bool& uiReady;
    bool& sceneRectSeen;
    bool& webDead;
    std::function<ParticleReplayController::Hooks(const std::wstring&)> buildHooks;
    std::function<std::string(const std::string&)> dispatchSync;
    std::function<void()> settleResize;
    std::function<void(const std::string&)> log;
    BridgeDispatcher& dispatcher;
};

struct ReplayRunContext
{
    ReplayRunDeps& deps;
    std::function<bool(int, const char*, const std::string&)> fail;
    std::function<bool()> failed;
    nlohmann::json& preparationLog;
    PreviewScenario inForce;
    nlohmann::json profile, geometry;
    bool geometryLocked = false;
    std::chrono::steady_clock::time_point start;
    // Publish geometry when it locks, even if a later preparation hook fails.
    std::function<void(const nlohmann::json&)> recordGeometry;
};

bool PumpMessages(ReplayRunContext&);
nlohmann::json CaptureGeometry(ReplayRunContext&);
bool CheckGeometry(ReplayRunContext&, const char* hook);
nlohmann::json FixedProfile(Engine&);
bool CheckSettings(ReplayRunContext&);
bool PrepareScene(ReplayRunContext&, const ParticleReplayController::Hooks&);
bool WaitReady(ReplayRunContext&);
bool LoadDocument(ReplayRunContext&, const std::wstring& aloPath);
std::string Sha256(const std::wstring& path);
bool AtomicWrite(const std::filesystem::path&, const std::string& bytes);

} // namespace host
