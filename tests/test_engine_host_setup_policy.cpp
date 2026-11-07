// Exercises setup decisions without Windows, game content or disk writes.
#include "host/EngineHostSetupPolicy.h"
#include <cstdio>
#include <functional>

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

void CheckRefused(const std::function<void()>& action, const char* message)
{
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        return;
    }
    Check(false, message);
}

nlohmann::json MakePackage()
{
    const std::string hash(kSha256HexChars, 'a');
    nlohmann::json    files = nlohmann::json::object();
    for (const std::string& name :
         {"d3d9.dll", "alamo_mapped.txt", "scene/PREVIEW_SOURCE.TED", "layer/PREVIEWTEST.XML", "README.txt"})
        files[name] = hash;
    files["signatures/" + hash + ".txt"] = hash;
    return {{"format", kPackageFormat},
            {"host_version", "test-host"},
            {"editors", {{{"sha256", hash}, {"label", "64-bit FoC editor"}}}},
            {"files", files}};
}

void TestPackages()
{
    const nlohmann::json manifest = MakePackage();
    const SetupPackage   package  = ParseSetupPackage(manifest.dump());
    Check(package.hostVersion == "test-host", "valid package version survives parsing");
    Check(FindSetupEditorLabel(package, std::string(kSha256HexChars, 'A')) == "64-bit FoC editor",
          "hash lookup ignores case");
    try
    {
        FindSetupEditorLabel(package, std::string(kSha256HexChars, 'b'));
        Check(false, "unsupported editor refuses");
    }
    catch (const std::exception& error)
    {
        const std::string refusal = error.what();
        Check(refusal.find("mod tools page") != std::string::npos, "refusal says where to get a supported build");
        Check(refusal.find("64-bit FoC editor") == std::string::npos, "refusal hides internal package labels");
    }
    CheckRefused([]() -> void { ParseSetupPackage("{"); }, "malformed JSON refuses");
    CheckRefused([]() -> void { ParseSetupPackage("[]"); }, "non-object manifest refuses");
    for (const nlohmann::json& patch :
         {nlohmann::json{{"host_version", ""}}, nlohmann::json{{"editors", nlohmann::json::object()}},
          nlohmann::json{{"editors", nlohmann::json::array()}}, nlohmann::json{{"files", nlohmann::json::array()}}})
    {
        nlohmann::json invalid = manifest;
        invalid.update(patch);
        CheckRefused([&]() -> void { ParseSetupPackage(invalid.dump()); }, "invalid manifest field shape refuses");
    }
    nlohmann::json invalidLabel         = manifest;
    invalidLabel["editors"][0]["label"] = "";
    CheckRefused([&]() -> void { ParseSetupPackage(invalidLabel.dump()); }, "empty editor label refuses");
    nlohmann::json duplicateEditor = manifest;
    duplicateEditor["editors"].push_back(duplicateEditor["editors"][0]);
    CheckRefused([&]() -> void { ParseSetupPackage(duplicateEditor.dump()); }, "duplicate editor hash refuses");
    for (const char* field : {"format", "host_version", "editors", "files"})
    {
        nlohmann::json missing = manifest;
        missing.erase(field);
        CheckRefused([&]() -> void { ParseSetupPackage(missing.dump()); }, "each required manifest field refuses");
    }
    for (auto file = manifest["files"].begin(); file != manifest["files"].end(); ++file)
    {
        nlohmann::json missing = manifest;
        missing["files"].erase(file.key());
        CheckRefused([&]() -> void { ParseSetupPackage(missing.dump()); }, "each required package file refuses");
    }
    for (const nlohmann::json& format :
         {nlohmann::json(2), nlohmann::json("1"), nlohmann::json(1.0), nlohmann::json(1.5)})
    {
        nlohmann::json invalid = manifest;
        invalid["format"]      = format;
        CheckRefused([&]() -> void { ParseSetupPackage(invalid.dump()); }, "unsupported package format refuses");
    }
    for (const std::string& hash : {std::string("abc"), std::string(kSha256HexChars, 'g')})
    {
        nlohmann::json invalid          = manifest;
        invalid["editors"][0]["sha256"] = hash;
        CheckRefused([&]() -> void { ParseSetupPackage(invalid.dump()); }, "bad editor hash refuses");
        invalid                      = manifest;
        invalid["files"]["d3d9.dll"] = hash;
        CheckRefused([&]() -> void { ParseSetupPackage(invalid.dump()); }, "bad file hash refuses");
    }
    for (const char* relative :
         {"../outside", "/absolute", "C:/absolute", "a\\b", "a/../b", "a//b", "a/", "a:stream", "a./b", "a\nb"})
    {
        nlohmann::json invalid     = manifest;
        invalid["files"][relative] = std::string(kSha256HexChars, 'a');
        CheckRefused([&]() -> void { ParseSetupPackage(invalid.dump()); }, "unsafe package path refuses");
    }
    for (const char* relative : {"CON", "nul.txt", "folder/COM1.txt", "a/LPT9"})
        Check(!IsSafeSetupRelativePath(relative), "Windows device path refuses");
    nlohmann::json aliased       = manifest;
    aliased["files"]["D3D9.DLL"] = std::string(kSha256HexChars, 'a');
    CheckRefused([&]() -> void { ParseSetupPackage(aliased.dump()); }, "case-aliased package files refuse");
    nlohmann::json uppercase          = manifest;
    uppercase["editors"][0]["sha256"] = std::string(kSha256HexChars, 'A');
    Check(ParseSetupPackage(uppercase.dump()).editors.size() == 1, "uppercase editor hash is accepted");
    Check(!IsSafeSetupRelativePath(std::string("a\0b", 3)), "NUL path refuses");
    Check(IsSafeSetupRelativePath("Data/XML/GAMEOBJECTFILES.XML"), "normal relative path is safe");
    Check(IsSafeSetupRelativePath("folder/.hidden"), "ordinary hidden file is safe");
}

void TestRefreshedFiles()
{
    nlohmann::json    manifest = MakePackage();
    const std::string editorHash(kSha256HexChars, 'b');
    manifest["editors"].push_back({{"sha256", editorHash}, {"label", "Second editor"}});
    manifest["files"]["signatures/" + editorHash + ".txt"] = editorHash;
    const SetupPackage                    package          = ParseSetupPackage(manifest.dump());
    const std::vector<RefreshedSetupFile> expected = {{"Data/XML/PREVIEWTEST.XML", kPackageLayerXml},
                                                      {"scene-source/PREVIEW_A.TED", kPackageSceneMap},
                                                      {"scene-source/PREVIEW_B.TED", kPackageSceneMap},
                                                      {"d3d9.dll", kPackageHostDll},
                                                      {"alamo_mapped.txt", kPackageMappedBuilds},
                                                      {"alamo_signatures.txt", "signatures/" + editorHash + ".txt"},
                                                      {"alamo_preview.ini", ""}};
    const std::vector<RefreshedSetupFile> files    = ListRefreshedSetupFiles(package, editorHash);
    Check(files.size() == expected.size(), "refresh has exactly the package overrides and generated ini");
    for (size_t index = 0; index < (std::min)(files.size(), expected.size()); ++index)
    {
        Check(files[index].destination == expected[index].destination, "refresh destination order matches setup");
        Check(files[index].packageSource == expected[index].packageSource,
              "refresh selects the matching package source");
    }
    const std::string                     firstEditorHash(kSha256HexChars, 'a');
    const std::vector<RefreshedSetupFile> firstFiles = ListRefreshedSetupFiles(package, firstEditorHash);
    for (const RefreshedSetupFile& file : firstFiles)
        if (file.destination == "alamo_signatures.txt")
            Check(file.packageSource == "signatures/" + firstEditorHash + ".txt",
                  "signature follows the copy's editor hash");

    std::map<std::string, std::string> expectedHashes;
    for (const RefreshedSetupFile& file : files) expectedHashes[file.destination] = editorHash;
    std::map<std::string, std::string> installedHashes = expectedHashes;
    Check(IsSetupCopyCurrent(expectedHashes, installedHashes), "matching installed hashes are current");
    installedHashes["d3d9.dll"] = firstEditorHash;
    Check(!IsSetupCopyCurrent(expectedHashes, installedHashes), "one differing hash needs refresh");
    installedHashes = expectedHashes;
    installedHashes.erase("scene-source/PREVIEW_A.TED");
    Check(!IsSetupCopyCurrent(expectedHashes, installedHashes), "one missing installed file needs refresh");
}

void TestSetupCompletion()
{
    const std::string journal = "Data\nData/config.meg\nalamo_preview.ini\n";
    Check(IsSetupJournalComplete(journal + kSetupCompletionRecord), "final flushed completion record completes setup");
    Check(!IsSetupJournalComplete(journal), "journaled paths alone do not complete setup");
    Check(!IsSetupJournalComplete(""), "empty journal is incomplete");
    std::string truncated = journal + kSetupCompletionRecord;
    truncated.pop_back();
    Check(!IsSetupJournalComplete(truncated), "truncated completion record is incomplete");
    Check(!IsSetupJournalComplete(journal + kSetupCompletionRecord + "other\n"), "completion must be the final record");
    Check(!IsSetupJournalComplete("filename" + std::string(kSetupCompletionRecord)), "completion is a whole record");
}

void TestStagedRefreshPaths()
{
    Check(BuildStagedRefreshPath("C:/Managed/Data/XML/PREVIEWTEST.XML") ==
              "C:/Managed/Data/XML/PREVIEWTEST.XML.particle-editor-refresh",
          "refresh staging is an exact sibling of a nested journaled path");
    Check(BuildStagedRefreshPath("C:/Managed/alamo_preview.ini") ==
              "C:/Managed/alamo_preview.ini.particle-editor-refresh",
          "refresh staging preserves the original file extension");
    Check(BuildStagedRefreshPath("C:/Managed/d3d9.dll") != "C:/Managed/other.dll.particle-editor-refresh",
          "staged cleanup does not select unrelated siblings");
}

void TestPaths()
{
    const std::vector<std::string> games        = {"C:/games/corruption", "D:/games/GameData"};
    const std::vector<std::string> repositories = {"C:/work"};
    const auto                     refusal      = [&](const std::string& target) -> std::string
    {
        return FindSetupTargetRefusal(target, games, "C:/editor", "C:/Windows", repositories);
    };
    Check(refusal("C:/Users/test/EngineHost").empty(), "private folder allowed");
    for (const char* target :
         {"C:/games/corruption", "c:\\GAMES\\corruption\\Data", "D:/games/GameData/host", "C:/editor/host",
          "C:/Windows/host", "C:/work/nested/host", "C:/safe/../Windows/host", ""})
        Check(!refusal(target).empty(), "protected target refuses");
    Check(refusal("C:/editor-other/host").empty(), "prefix sibling allowed");
    Check(IsSetupPathWithin("C:/host/a.exe", "c:\\HOST\\"), "descendant comparison ignores case and slashes");
    Check(!IsSetupPathWithin("C:/hostile/a.exe", "C:/host"), "component boundary protects siblings");
    for (const char* name : {"foo.dll", "FOO.DLL", "_foo.txt", "_FOO.TXT"})
        Check(ShouldCopyEditorEntry(name), "runtime sibling copied");
    for (const char* name : {"d3d9.dll", "D3D9.DLL", "d3d9.dll.bak", "alamo_test.dll", "ALAMO_test.txt",
                             ".alamo-preview-install.json", "foo.txt", "foo.exe", "sub/foo.dll", ""})
        Check(!ShouldCopyEditorEntry(name), "host or non-runtime sibling excluded");
    for (const char* path : {"XML/GAMEOBJECTFILES.XML", "xml/previewtest.xml", "Art/Models/P_PREVIEW_TEST.ALO"})
        Check(IsSetupOwnedGameFile(path), "setup override excluded from game links and copies");
    Check(!IsSetupOwnedGameFile("Art/Models/other.alo"), "ordinary game asset copied");
    Check(ShouldLinkGameFile(1, 1), "same volume links");
    Check(!ShouldLinkGameFile(1, 2), "different volume copies");
}

void TestGeneratedText()
{
    const std::string original = "<Game_Object_Files>\r\n</gAmE_oBjEcT_fIlEs>";
    const std::string inserted = InsertPreviewGameObjectFile(original);
    Check(inserted == "<Game_Object_Files>\r\n\t<File>PreviewTest.xml</File>\r\n</gAmE_oBjEcT_fIlEs>",
          "insertion preserves original bytes");
    Check(InsertPreviewGameObjectFile(inserted) == inserted, "insertion is idempotent");
    const std::string registered = "<Game_Object_Files><FILE> previewTEST.xml </FILE></Game_Object_Files>";
    Check(InsertPreviewGameObjectFile(registered) == registered, "existing mixed-case registration preserved");
    CheckRefused([]() -> void { InsertPreviewGameObjectFile("<Game_Object_Files>"); }, "missing closing tag refuses");
    const std::string ini = BuildSetupIni("C:\\host");
    for (const char* value :
         {"[Preview]\r\nEnabled=1", "EffectPath=C:\\host\\Data\\Art\\Models\\P_PREVIEW_TEST.ALO",
          "MapA=C:\\host\\scene-source\\PREVIEW_A.TED", "MapB=C:\\host\\scene-source\\PREVIEW_B.TED",
          "SceneDir=C:\\host\\scene", "[Window]\r\nHidden=1"})
        Check(ini.find(value) != std::string::npos, "generated ini contains launch prerequisites");
    CheckRefused([]() -> void { BuildSetupIni("C:\\host\r\nInjected=1"); }, "ini path injection refuses");
}
} // namespace

void TestHostRuntimeOutputs()
{
    Check(IsHostRuntimeOutput("alamo_preview.log"), "host log is a runtime output");
    Check(IsHostRuntimeOutput("Data/Art/Textures/W_PRVGND_ba5b3ab22fc_B.dds"), "ground texture is a runtime output");
    Check(IsHostRuntimeOutput("Data/Art/Textures/W_PRVCLD00.dds"), "cloud texture is a runtime output");
    Check(IsHostRuntimeOutput("scene/71968"), "pid scene folder is a runtime output");
    Check(IsHostRuntimeOutput("scene/71968/PRVGND_START_A.TED"), "scene map is a runtime output");
    Check(!IsHostRuntimeOutput("scene"), "scene folder itself is journaled, not runtime");
    Check(!IsHostRuntimeOutput("scene/notapid/PRVGND_A.TED"), "non-pid scene folder is not runtime");
    Check(!IsHostRuntimeOutput("Data/Art/Textures/W_TEMPGRND00.DDS"), "game texture is not runtime");
    Check(!IsHostRuntimeOutput("Data/config.meg"), "game data is not runtime");
    Check(!IsHostRuntimeOutput("scene-source/PREVIEW_A.TED"), "source maps are journaled, not runtime");
}

int main()
{
    TestPackages();
    TestRefreshedFiles();
    TestSetupCompletion();
    TestStagedRefreshPaths();
    TestPaths();
    TestGeneratedText();
    TestHostRuntimeOutputs();
    if (g_failed == 0) std::puts("engine host setup policy: ALL PASS");
    return g_failed == 0 ? 0 : 1;
}
