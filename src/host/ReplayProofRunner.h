#pragma once

#include <memory>
#include <string>
#include <functional>
#include "ParticleReplayController.h"

class Engine;
class ParticleSystem;
class UndoStack;

namespace host {

class AlphaCompositor;
class LayoutBroker;
class BridgeDispatcher;

class ReplayProofRunner
{
public:
    struct Deps
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

    ReplayProofRunner(Deps, const std::wstring& scenarioPath,
                      const std::wstring& outputDirectory);
    ~ReplayProofRunner();
    int Run();
    bool BeforeRender();
    bool AfterRender(bool succeeded);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace host
