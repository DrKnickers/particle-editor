#pragma once

#include "ReplayRunSupport.h"
#include <vector>

namespace host
{

class ObservationRunner
{
public:
    struct Deps
    {
        std::function<bool()> spawnerIdle;
        // Flush the live log before returning its path for the bundle copy.
        std::function<std::wstring()>              hostLogPath;
        std::function<std::vector<std::wstring>()> gameRoots;
        std::function<std::vector<std::wstring>()> selectedModPaths;
    };
    ObservationRunner(ReplayRunDeps, Deps, const std::wstring& specPath, const std::wstring& outputDirectory);
    ~ObservationRunner();
    int  Run();
    bool BeforeRender();
    bool AfterRender(bool succeeded);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace host
