// Engine-preview wire and camera policy. No windows, connections or document writes;
// the worker and bridge supply pipe names and resolved filesystem paths.
#ifndef HOST_ENGINE_PREVIEW_PIPE_H
#define HOST_ENGINE_PREVIEW_PIPE_H

#include "host/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace host::engine_preview
{
constexpr size_t kMaxLineBytes          = 65536;
constexpr size_t kMaxImageBytes         = 32 * 1024 * 1024;
constexpr double kMaxTimeSeconds        = 600.0;
constexpr double kStillBudgetSeconds    = 180.0;
constexpr double kCommandBudgetSeconds  = 15.0;
constexpr double kTransportGraceSeconds = 5.0;
constexpr double kMaxPitchDegrees       = 80.0;
constexpr double kMaxDistanceUnits      = 60000.0;
constexpr double kMaxTargetUnits        = 1000000.0;
constexpr double kRadiansToDegrees      = 180.0 / 3.14159265358979323846;
// A fixed engine random state makes repeated stills of the same effect identical.
constexpr unsigned kPreviewRandomState = 1;
// The host halves each edge: a 4K editor view becomes about 1.5K wide, which is still sharp in the panel
// and cuts the PNG the worker reads and the data URI the panel decodes to a quarter.
constexpr int kPreviewCaptureScale = 2;
// Awaiting live calibration against an asymmetric effect in the engine host.
constexpr double        kYawZeroDegrees          = 0.0;
constexpr double        kYawSign                 = 1.0;
constexpr std::int64_t  kMaxRequestId            = 9007199254740992LL;
constexpr unsigned      kDecimalRadix            = 10;
constexpr unsigned char kMaxAsciiCode            = 127;
constexpr unsigned char kFirstPrintableAsciiCode = 32;
constexpr size_t        kDrivePrefixChars        = 2;
constexpr size_t        kDriveRootChars          = kDrivePrefixChars + 1;
constexpr size_t        kUncPrefixChars          = 2;
constexpr const char*   kReservedFileNames[] = {"con",  "prn",  "aux",  "nul",  "conin$", "conout$", "com1", "com2",
                                                "com3", "com4", "com5", "com6", "com7",   "com8",    "com9", "lpt1",
                                                "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",   "lpt7",    "lpt8", "lpt9"};

struct Failure : std::runtime_error
{
    std::string code;
    Failure(const std::string& errorCode, const std::string& message) : std::runtime_error(message), code(errorCode) {}
};

inline std::string ErrorMessage(const std::string& code, const std::string& detail)
{
    std::string message;
    if (code == "bad-args")
        message = "The engine host rejected the preview settings.";
    else if (code == "unsupported")
        message = "This engine host cannot run the requested preview. Check its preview setup.";
    else if (code == "internal")
        message = "The engine host could not finish the preview.";
    else if (code == "timeout")
        message = "The engine preview timed out. Check that the host is responsive and retry.";
    else if (code == "not-found")
        message = "The preview effect or map was not found. Check the host's preview scene.";
    else if (code == "needs-reopen")
        message = "The engine host needs to reopen the preview map.";
    else if (code == "cancelled")
        message = "Engine preview cancelled.";
    else if (code == "absent")
        message = "Start the engine preview host, then retry the status check.";
    else if (code == "disconnected")
        message = "The engine preview host disconnected. Retry the status check.";
    else
        message = "The engine preview returned an invalid reply.";
    return detail.empty() ? message : message + " " + detail;
}

inline std::uint32_t ParsePipePid(const std::wstring& name)
{
    const std::wstring prefix     = L"alamo-preview-";
    const std::wstring fullPrefix = L"\\\\.\\pipe\\";
    const std::wstring leaf       = name.rfind(fullPrefix, 0) == 0 ? name.substr(fullPrefix.size()) : name;
    if (leaf.rfind(prefix, 0) != 0 || leaf.size() == prefix.size()) return 0;
    std::uint64_t pid = 0;
    for (size_t index = prefix.size(); index < leaf.size(); ++index)
    {
        const wchar_t digit = leaf[index];
        if (digit < L'0' || digit > L'9') return 0;
        pid = pid * kDecimalRadix + (digit - L'0');
        if (pid > (std::numeric_limits<std::uint32_t>::max)()) return 0;
    }
    if (leaf.substr(prefix.size()) != std::to_wstring(pid)) return 0;
    return static_cast<std::uint32_t>(pid);
}

inline std::wstring SelectPipe(const std::vector<std::wstring>& names, std::uint32_t requestedPid = 0)
{
    std::vector<std::wstring> matches;
    for (const std::wstring& name : names)
    {
        const std::uint32_t pid = ParsePipePid(name);
        if (pid != 0 && (requestedPid == 0 || requestedPid == pid))
            matches.push_back(L"\\\\.\\pipe\\alamo-preview-" + std::to_wstring(pid));
    }
    if (matches.empty()) throw Failure("absent", ErrorMessage("absent", ""));
    if (matches.size() != 1)
        throw Failure("multiple-hosts", "Several engine preview hosts are running. Specify a pid in the status check.");
    return matches.front();
}

inline std::string FrameRequest(std::int64_t& nextId, const std::string& command, const nlohmann::json& args)
{
    if (!args.is_object() || command.empty() || nextId < 1 || nextId > kMaxRequestId)
        throw Failure("bad-args", "Invalid engine preview request.");
    const std::string line = nlohmann::json{{"id", nextId}, {"cmd", command}, {"args", args}}.dump();
    if (line.size() >= kMaxLineBytes) throw Failure("bad-args", "Engine preview request is too large.");
    ++nextId;
    return line + '\n';
}

inline nlohmann::json ParseReply(const std::string& line, std::int64_t expectedId)
{
    const nlohmann::json reply       = nlohmann::json::parse(line, nullptr, false);
    const bool           hasEnvelope = reply.is_object() && reply.contains("id") && reply.contains("ok");
    if (line.size() > kMaxLineBytes || !hasEnvelope || !reply["id"].is_number_integer() || reply["id"] != expectedId ||
        !reply["ok"].is_boolean())
        throw Failure("protocol", "Invalid engine preview reply envelope or request id.");
    if (reply["ok"].get<bool>())
    {
        if (!reply.contains("result") || !reply["result"].is_object())
            throw Failure("protocol", "Engine preview result must be an object.");
        return reply["result"];
    }
    if (!reply.contains("error") || !reply["error"].is_object())
        throw Failure("protocol", "Engine preview error must be an object.");
    const nlohmann::json& error = reply["error"];
    if (!error.contains("code") || !error["code"].is_string() || !error.contains("message") ||
        !error["message"].is_string())
        throw Failure("protocol", "Engine preview error requires a code and message.");
    const std::string code = error["code"].get<std::string>();
    throw Failure(code, ErrorMessage(code, error["message"].get<std::string>()));
}

inline std::string FoldPath(std::string path)
{
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(
        path.begin(), path.end(), path.begin(), [](unsigned char letter) -> char
        { return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : static_cast<char>(letter); });
    while (!path.empty() && path.back() == '\\') path.pop_back();
    return path;
}

inline bool IsAbsolutePath(const std::string& path)
{
    const bool drive =
        path.size() >= kDriveRootChars && path[1] == ':' && path[2] == '\\' && path[0] >= 'a' && path[0] <= 'z';
    const bool share = path.rfind("\\\\", 0) == 0 && path.size() > kDriveRootChars &&
                       path[kDriveRootChars - 1] != '?' && path[kDriveRootChars - 1] != '.';
    if (drive) return true;
    if (!share) return false;
    const size_t serverEnd = path.find('\\', kUncPrefixChars);
    if (serverEnd == std::string::npos || serverEnd == kUncPrefixChars) return false;
    const size_t shareEnd = path.find('\\', serverEnd + 1);
    return shareEnd != std::string::npos && shareEnd > serverEnd + 1;
}

inline bool IsWithin(const std::string& path, const std::string& root)
{
    return path == root ||
           (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '\\');
}

inline std::string CheckPreviewPath(const std::string& path,
                                    const std::string& internalName,
                                    const std::string& installDirectory,
                                    const std::string& windowsDirectory)
{
    for (unsigned char letter : path)
        if (letter < kFirstPrintableAsciiCode || letter > kMaxAsciiCode)
            return "The preview copy path must contain printable ASCII characters only.";
    if (path.empty() || path.back() == '\\' || path.back() == '/') return "The preview copy path must name a file.";
    if (internalName.empty() || internalName.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return "The host's internal name must be a plain filename stem.";
    const std::string folded = FoldPath(path);
    if (!IsAbsolutePath(folded)) return "The preview copy path must be absolute.";
    // Reject ambiguous Win32 components before resolving parents at the write site.
    size_t begin = 0;
    while (begin < folded.size())
    {
        const size_t      end       = folded.find('\\', begin);
        const std::string component = folded.substr(begin, end == std::string::npos ? end : end - begin);
        if (!component.empty() && (component.back() == '.' || component.back() == ' '))
            return "The preview copy path contains an ambiguous component.";
        const bool driveComponent = begin == 0 && component.size() == kDrivePrefixChars && component.back() == ':';
        if (!driveComponent && component.find_first_of(":*?\"<>|") != std::string::npos)
            return "The preview copy path contains an invalid component.";
        const std::string baseName = component.substr(0, component.find('.'));
        for (const char* reserved : kReservedFileNames)
            if (baseName == reserved) return "The preview copy path contains a reserved Windows name.";
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    const size_t      slash              = folded.find_last_of('\\');
    const std::string leaf               = folded.substr(slash + 1);
    constexpr size_t  kAloExtensionChars = 4;
    if (leaf.size() <= kAloExtensionChars || leaf.substr(leaf.size() - kAloExtensionChars) != ".alo" ||
        leaf.find_first_of(":*?\"<>|") != std::string::npos)
        return "The preview copy must be a plain .alo file.";
    if (leaf.substr(0, leaf.size() - kAloExtensionChars) != FoldPath(internalName))
        return "The preview filename must match the host's internal name.";
    const std::string install = FoldPath(installDirectory);
    const std::string windows = FoldPath(windowsDirectory);
    if (install.empty() || windows.empty()) return "Cannot resolve protected preview directories.";
    if (IsWithin(folded, install) || IsWithin(folded, windows))
        return "The preview copy must be outside the editor install and Windows directories.";
    return "";
}

struct Orbit
{
    double                pitch;
    double                yaw;
    double                distance;
    std::array<double, 3> target;
};

inline Orbit OrbitFromLookAt(const std::array<double, 3>& position, const std::array<double, 3>& target)
{
    for (size_t axis = 0; axis < target.size(); ++axis)
        if (!std::isfinite(position[axis]) || !std::isfinite(target[axis]) || std::abs(target[axis]) > kMaxTargetUnits)
            throw Failure("bad-args", "The editor camera has an invalid position or target.");
    const double x          = position[0] - target[0];
    const double y          = position[1] - target[1];
    const double z          = position[2] - target[2];
    const double horizontal = std::hypot(x, y);
    const double distance   = std::hypot(horizontal, z);
    const double pitch      = std::atan2(z, horizontal) * kRadiansToDegrees;
    if (distance <= 0 || distance > kMaxDistanceUnits || pitch < 0 || pitch > kMaxPitchDegrees)
        throw Failure("bad-args", "The editor camera is outside the engine's pitch or distance limits.");
    const double yaw = kYawZeroDegrees + kYawSign * std::atan2(x, -y) * kRadiansToDegrees;
    return Orbit{pitch, yaw, distance, target};
}
} // namespace host::engine_preview
#endif
