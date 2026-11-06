// Covers each required launch prerequisite without Win32 or an ini file.
#include "host/EngineHostLaunchPolicy.h"
#include <cstdio>

using namespace host::engine_host;
namespace
{
int  g_failed = 0;
void Check(bool passed, const char* message)
{
    if (passed) return;
    ++g_failed;
    std::printf("FAIL: %s\n", message);
}
} // namespace
int main()
{
    const IniValues ready{L"1", L"effect", L"map-a", L"map-b", L"scene"};
    Check(FindMissingLaunchPrerequisites(false, ready) == kPathNotSetMessage, "missing executable is named");
    Check(FindMissingLaunchPrerequisites(true, ready).empty(), "complete configuration launches");
    const std::string missing = FindMissingLaunchPrerequisites(true, {});
    for (const char* field : {"Hidden=1", "EffectPath", "MapA", "MapB", "SceneDir"})
        Check(missing.find(field) != std::string::npos, "every missing ini value is named");
    for (std::wstring IniValues::* field :
         {&IniValues::hidden, &IniValues::effectPath, &IniValues::mapA, &IniValues::mapB, &IniValues::sceneDir})
    {
        IniValues incomplete = ready;
        incomplete.*field    = L"";
        Check(!FindMissingLaunchPrerequisites(true, incomplete).empty(), "each required value refuses on its own");
    }
    IniValues visible = ready;
    visible.hidden    = L"0";
    Check(!FindMissingLaunchPrerequisites(true, visible).empty(), "visible editor is refused");
    return g_failed == 0 ? 0 : 1;
}
