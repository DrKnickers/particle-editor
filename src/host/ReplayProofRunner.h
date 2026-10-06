#pragma once

#include <memory>
#include <string>
#include <functional>
#include "ReplayRunSupport.h"

namespace host {

class ReplayProofRunner
{
public:
    using Deps = ReplayRunDeps;

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
