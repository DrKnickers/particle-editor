// Checks launch prerequisites without filesystem or process access. ConnectHost reuses
// an existing pipe; EnsureStarted reuses the saved process while it is alive.
#pragma once

#include <string>

namespace host::engine_host
{
struct IniValues
{
    std::wstring hidden;
    std::wstring effectPath;
    std::wstring mapA;
    std::wstring mapB;
    std::wstring sceneDir;
};

constexpr const char* kPathNotSetMessage = "Set the map editor executable in Preferences > Engine preview.";

inline std::string FindMissingLaunchPrerequisites(bool hasPath, const IniValues& ini)
{
    if (!hasPath) return kPathNotSetMessage;
    std::string missing;
    if (ini.hidden != L"1") missing += " [Window] Hidden=1;";
    if (ini.effectPath.empty()) missing += " [Preview] EffectPath;";
    if (ini.mapA.empty()) missing += " [Preview] MapA;";
    if (ini.mapB.empty()) missing += " [Preview] MapB;";
    if (ini.sceneDir.empty()) missing += " [Preview] SceneDir;";
    if (!missing.empty()) return "alamo_preview.ini next to the map editor is missing:" + missing;
    return "";
}
} // namespace host::engine_host
