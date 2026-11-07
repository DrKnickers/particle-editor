// Owns setup validation and generated text; performs no filesystem or process I/O.
#pragma once

#include "host/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace host::engine_host
{
constexpr const wchar_t* kManagedHostRelativePath = L"AloParticleEditor\\EngineHost";
constexpr const char*    kSetupJournalName        = ".particle-editor-setup.journal";
// A non-path record cannot be confused with a file that removal owns.
constexpr const char* kSetupCompletionRecord = ":setup-complete\n";
constexpr const char* kRefreshTempSuffix     = ".particle-editor-refresh";
constexpr size_t      kSha256HexChars        = 64;
constexpr int         kPackageFormat         = 1;
// The release zip ships the host package in this folder beside ParticleEditor.exe.
constexpr const wchar_t* kBundledPackageFolder = L"engine-host";
constexpr const wchar_t* kPackageManifestName  = L"package.json";
// The Alamo Preview host package layout and the managed copy's fixed names.
constexpr const char*    kPackageHostDll         = "d3d9.dll";
constexpr const char*    kPackageMappedBuilds    = "alamo_mapped.txt";
constexpr const char*    kPackageSceneMap        = "scene/PREVIEW_SOURCE.TED";
constexpr const char*    kPackageLayerXml        = "layer/PREVIEWTEST.XML";
constexpr const char*    kPackageReadme          = "README.txt";
constexpr const char*    kPackageSignatureFolder = "signatures/";
constexpr const wchar_t* kManagedEditorExeName   = L"EAW Terrain Editor.exe";

inline bool IsPrintableAsciiSetupPath(const std::wstring& path)
{
    constexpr wchar_t kFirstPrintableCharacter = L' ';
    constexpr wchar_t kLastPrintableCharacter  = L'~';
    return !path.empty() &&
           std::all_of(path.begin(), path.end(), [=](wchar_t character) -> bool
                       { return character >= kFirstPrintableCharacter && character <= kLastPrintableCharacter; });
}

inline std::wstring ChooseManagedHostPath(const std::wstring& localAppDataPath, const std::wstring& programDataPath)
{
    // The engine opens these paths through narrow APIs, even though setup writes a Unicode ini.
    if (IsPrintableAsciiSetupPath(localAppDataPath)) return localAppDataPath;
    if (IsPrintableAsciiSetupPath(programDataPath)) return programDataPath;
    return L"";
}

struct SetupPackage
{
    std::string                        hostVersion;
    std::map<std::string, std::string> editors;
    std::map<std::string, std::string> files;
};

inline bool IsSetupJournalComplete(const std::string& contents)
{
    const std::string completion = kSetupCompletionRecord;
    if (contents.size() < completion.size()) return false;
    const size_t start = contents.size() - completion.size();
    return (start == 0 || contents[start - 1] == '\n') && contents.compare(start, completion.size(), completion) == 0;
}

inline std::string BuildStagedRefreshPath(const std::string& destination)
{
    return destination + kRefreshTempSuffix;
}

inline std::string FoldSetupText(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
    return text;
}

inline bool IsSha256(const std::string& hash)
{
    return hash.size() == kSha256HexChars && hash.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

inline bool IsSafeSetupRelativePath(const std::string& path)
{
    if (path.empty() || path.front() == '/') return false;
    if (path.find_first_of("\\:*?\"<>|") != std::string::npos) return false;
    for (unsigned char ch : path)
        if (std::iscntrl(ch)) return false;
    size_t start = 0;
    while (start < path.size())
    {
        const size_t      end  = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (part.back() == '.' || part.back() == ' ') return false;
        const std::string stem          = FoldSetupText(part.substr(0, part.find('.')));
        const bool        isNamedDevice = stem == "con" || stem == "prn" || stem == "aux" || stem == "nul";
        if (isNamedDevice) return false;
        const std::string prefix = stem.empty() ? "" : stem.substr(0, stem.size() - 1);
        const bool isPortDevice  = (prefix == "com" || prefix == "lpt") && stem.back() >= '1' && stem.back() <= '9';
        if (isPortDevice) return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

inline SetupPackage ParseSetupPackage(const std::string& text)
{
    const nlohmann::json manifest = nlohmann::json::parse(text);
    if (!manifest.is_object()) throw std::runtime_error("The host package manifest must be an object.");
    if (!manifest.at("format").is_number_integer() || manifest.at("format") != kPackageFormat)
        throw std::runtime_error("The host package must use format " + std::to_string(kPackageFormat) + ".");
    SetupPackage package;
    package.hostVersion = manifest.at("host_version").get<std::string>();
    if (package.hostVersion.empty() || !manifest.at("editors").is_array())
        throw std::runtime_error("The host package needs a version and supported editors.");
    for (const nlohmann::json& editor : manifest.at("editors"))
    {
        const std::string hash  = editor.at("sha256").get<std::string>();
        const std::string label = editor.at("label").get<std::string>();
        if (!IsSha256(hash) || label.empty()) throw std::runtime_error("Invalid supported editor hash or label.");
        if (!package.editors.emplace(FoldSetupText(hash), label).second)
            throw std::runtime_error("Duplicate supported editor hash.");
    }
    if (package.editors.empty() || !manifest.at("files").is_object())
        throw std::runtime_error("The host package needs supported editors and files.");
    for (auto entry = manifest.at("files").begin(); entry != manifest.at("files").end(); ++entry)
    {
        const std::string hash = entry.value().get<std::string>();
        if (!IsSafeSetupRelativePath(entry.key()) || !IsSha256(hash))
            throw std::runtime_error("Invalid package file path or SHA-256: " + entry.key());
        // Windows file names are case-insensitive; refuse aliases before writing any output.
        if (!package.files.emplace(FoldSetupText(entry.key()), FoldSetupText(hash)).second)
            throw std::runtime_error("Duplicate package file path: " + entry.key());
    }
    std::vector<std::string> required = {kPackageHostDll, kPackageMappedBuilds, kPackageSceneMap, kPackageLayerXml,
                                         kPackageReadme};
    for (const std::pair<const std::string, std::string>& editor : package.editors)
        required.push_back(kPackageSignatureFolder + editor.first + ".txt");
    for (const std::string& path : required)
        if (package.files.count(FoldSetupText(path)) == 0)
            throw std::runtime_error("The host package is missing " + path + ".");
    return package;
}

inline std::string FindSetupEditorLabel(const SetupPackage& package, const std::string& hash)
{
    const auto editor = package.editors.find(FoldSetupText(hash));
    if (editor != package.editors.end()) return editor->second;
    // Package labels are internal build IDs, so the refusal names where to get a supported build instead.
    throw std::runtime_error("This EAW Terrain Editor build isn't supported. Download the 64-bit EAW Terrain "
                             "Editor from Petroglyph's mod tools page and choose that copy; the engine preview "
                             "guide links to it.");
}

struct RefreshedSetupFile
{
    std::string destination;
    // An empty package source selects the generated UTF-16 ini.
    std::string packageSource;
};

inline std::vector<RefreshedSetupFile> ListRefreshedSetupFiles(const SetupPackage& package,
                                                               const std::string&  editorHash)
{
    // Throws for an unsupported editor, so no signature file name is guessed.
    FindSetupEditorLabel(package, editorHash);
    return {{"Data/XML/PREVIEWTEST.XML", kPackageLayerXml},
            {"scene-source/PREVIEW_A.TED", kPackageSceneMap},
            {"scene-source/PREVIEW_B.TED", kPackageSceneMap},
            {"d3d9.dll", kPackageHostDll},
            {"alamo_mapped.txt", kPackageMappedBuilds},
            {"alamo_signatures.txt", kPackageSignatureFolder + FoldSetupText(editorHash) + ".txt"},
            {"alamo_preview.ini", ""}};
}

inline bool IsSetupCopyCurrent(const std::map<std::string, std::string>& expectedHashes,
                               const std::map<std::string, std::string>& installedHashes)
{
    for (const std::pair<const std::string, std::string>& expected : expectedHashes)
    {
        const auto installed = installedHashes.find(expected.first);
        if (installed == installedHashes.end() || installed->second != expected.second) return false;
    }
    return true;
}

inline std::string NormalizeSetupPath(std::string path)
{
    path = FoldSetupText(path);
    std::replace(path.begin(), path.end(), '\\', '/');
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start < path.size())
    {
        const size_t      end  = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part == ".." && !parts.empty())
            parts.pop_back();
        else if (!part.empty() && part != ".")
            parts.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string normalized;
    for (const std::string& part : parts) normalized += part + '/';
    return normalized;
}

inline bool IsSetupPathWithin(const std::string& path, const std::string& root)
{
    const std::string normalizedRoot = NormalizeSetupPath(root);
    if (normalizedRoot.empty()) return false;
    return NormalizeSetupPath(path).compare(0, normalizedRoot.size(), normalizedRoot) == 0;
}

inline std::string FindSetupTargetRefusal(const std::string&              target,
                                          const std::vector<std::string>& gameRoots,
                                          const std::string&              editorFolder,
                                          const std::string&              windowsFolder,
                                          const std::vector<std::string>& gitAncestors)
{
    if (target.empty()) return "The setup target is empty.";
    for (const std::string& root : gameRoots)
        if (IsSetupPathWithin(target, root)) return "The setup target must be outside the game roots.";
    if (IsSetupPathWithin(target, editorFolder)) return "The setup target must be outside the editor folder.";
    if (IsSetupPathWithin(target, windowsFolder)) return "The setup target must be outside the Windows directory.";
    for (const std::string& root : gitAncestors)
        if (IsSetupPathWithin(target, root)) return "The setup target must be outside folders holding .git.";
    return "";
}

inline bool ShouldCopyEditorEntry(const std::string& name)
{
    const std::string folded = FoldSetupText(name);
    if (folded.find_first_of("/\\") != std::string::npos) return false;
    const bool isHostFile = folded == "d3d9.dll" || folded == "d3d9.dll.bak" || folded.rfind("alamo_", 0) == 0 ||
                            folded == ".alamo-preview-install.json";
    if (isHostFile) return false;
    const size_t dot = folded.find_last_of('.');
    if (dot == std::string::npos) return false;
    return folded.substr(dot) == ".dll" || (folded.front() == '_' && folded.substr(dot) == ".txt");
}

inline std::string InsertPreviewGameObjectFile(const std::string& xml)
{
    const std::string folded  = FoldSetupText(xml);
    const size_t      closing = folded.rfind("</game_object_files>");
    if (closing == std::string::npos) throw std::runtime_error("GAMEOBJECTFILES.XML is missing </Game_Object_Files>.");
    // Ignore whitespace within File elements so existing registrations remain single entries.
    std::string compact;
    for (unsigned char ch : folded)
        if (!std::isspace(ch)) compact += static_cast<char>(ch);
    if (compact.find("<file>previewtest.xml</file>") != std::string::npos) return xml;
    std::string result = xml;
    result.insert(closing, "\t<File>PreviewTest.xml</File>\r\n");
    return result;
}

inline std::string BuildSetupIni(const std::string& target)
{
    if (std::any_of(target.begin(), target.end(), [](unsigned char ch) -> bool { return std::iscntrl(ch) != 0; }))
        throw std::runtime_error("The setup target cannot contain line breaks or NUL.");
    return "[Preview]\r\nEnabled=1\r\nEffectPath=" + target +
           "\\Data\\Art\\Models\\P_PREVIEW_TEST.ALO\r\nMapA=" + target +
           "\\scene-source\\PREVIEW_A.TED\r\nMapB=" + target + "\\scene-source\\PREVIEW_B.TED\r\nSceneDir=" + target +
           "\\scene\r\n[Window]\r\nHidden=1\r\n";
}

inline bool ShouldLinkGameFile(std::uint32_t targetVolumeSerial, std::uint32_t gameVolumeSerial)
{
    return targetVolumeSerial == gameVolumeSerial;
}

// Names the files and per-process scene folders the host writes inside the managed copy at
// runtime (its log, content-addressed ground textures, and scene maps), which removal deletes too.
inline bool IsHostRuntimeOutput(const std::string& relative)
{
    std::string path = NormalizeSetupPath(relative);
    if (!path.empty()) path.pop_back();
    const auto hasPrefix = [&path](const std::string& prefix) -> bool
    {
        return path.rfind(prefix, 0) == 0;
    };
    const auto hasSuffix = [&path](const std::string& suffix) -> bool
    {
        return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (path == "alamo_preview.log" || path == "data/art/textures/w_prvcld00.dds") return true;
    const bool isGroundTexture = hasPrefix("data/art/textures/w_prvgnd_") && hasSuffix(".dds");
    if (isGroundTexture) return true;
    if (!hasPrefix("scene/")) return false;
    const std::string inScene   = path.substr(std::string("scene/").size());
    const size_t      slash     = inScene.find('/');
    const std::string pidFolder = inScene.substr(0, slash);
    const bool isPidFolder      = !pidFolder.empty() && pidFolder.find_first_not_of("0123456789") == std::string::npos;
    if (!isPidFolder) return false;
    if (slash == std::string::npos) return true;
    const std::string mapName = inScene.substr(slash + 1);
    return mapName.rfind("prvgnd_", 0) == 0 && hasSuffix(".ted") && mapName.find('/') == std::string::npos;
}

inline bool IsSetupOwnedGameFile(const std::string& relative)
{
    const std::string path = NormalizeSetupPath(relative);
    return path == "xml/gameobjectfiles.xml/" || path == "xml/previewtest.xml/" ||
           path == "art/models/p_preview_test.alo/";
}
} // namespace host::engine_host
