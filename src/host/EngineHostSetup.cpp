// Builds, refreshes and removes the journaled private render host. Game files are read-only inputs;
// setup-owned overrides are fresh files, never hard links into the game installation.
#include "EngineHostSetup.h"
#include "EngineHostSetupPolicy.h"
#include "HostMessages.h"
#include "StringConv.h"
#include "common/files.h"
#include "common/sha256.h"
#include "gamedata/GameRoots.h"
#include "gamedata/managers.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace host
{
namespace
{
namespace fs = std::filesystem;
using namespace engine_host;
constexpr ULONGLONG   kSetupProgressIntervalMs = 100;
constexpr DWORD       kProcessPathChars        = 32768;
constexpr const char* kIncompleteSetupMessage  = "This setup didn't finish. Remove setup and set up again.";

fs::path StagedRefreshPath(const fs::path& destination)
{
    return fs::u8path(BuildStagedRefreshPath(destination.u8string()));
}

struct CloseSetupHandle
{
    void operator()(void* handle) const
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using SetupHandle      = std::unique_ptr<void, CloseSetupHandle>;
using ProgressReporter = std::function<void(const std::string&, size_t, size_t)>;

void ThrowIfSetupCancelled(const std::atomic<bool>& cancelRequested)
{
    if (cancelRequested.load()) throw std::runtime_error("Setup was cancelled.");
}

void RequireWindows(bool succeeded, const std::string& action)
{
    if (!succeeded) throw std::runtime_error(action + " (Windows error " + std::to_string(GetLastError()) + ").");
}

std::wstring ReadCurrentUserSid()
{
    HANDLE rawToken = nullptr;
    RequireWindows(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) != FALSE,
                   "Cannot open the current user token");
    const SetupHandle token(rawToken);
    DWORD             requiredBytes = 0;
    const BOOL        sized         = GetTokenInformation(token.get(), TokenUser, nullptr, 0, &requiredBytes);
    if (sized || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        throw std::runtime_error("Cannot determine the current user token size.");
    std::vector<BYTE> tokenBytes(requiredBytes);
    RequireWindows(GetTokenInformation(token.get(), TokenUser, tokenBytes.data(), requiredBytes, &requiredBytes) !=
                       FALSE,
                   "Cannot read the current user token");
    const TOKEN_USER* user   = reinterpret_cast<const TOKEN_USER*>(tokenBytes.data());
    wchar_t*          rawSid = nullptr;
    RequireWindows(ConvertSidToStringSidW(user->User.Sid, &rawSid) != FALSE, "Cannot convert the current user SID");
    std::unique_ptr<void, decltype(&LocalFree)> sidMemory(rawSid, LocalFree);
    const std::wstring                          sid(rawSid);
    RequireWindows(LocalFree(sidMemory.get()) == nullptr, "Cannot free the current user SID string");
    sidMemory.release();
    return sid;
}

void RefuseForeignOwnedFolder(const fs::path& folder, const std::wstring& ownerSid)
{
    PSID                 owner         = nullptr;
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    const DWORD read = GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                             nullptr, nullptr, nullptr, &rawDescriptor);
    if (read != ERROR_SUCCESS) throw std::runtime_error("Cannot read the setup target owner.");
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor(rawDescriptor, LocalFree);
    wchar_t*                                          rawOwnerSid = nullptr;
    RequireWindows(ConvertSidToStringSidW(owner, &rawOwnerSid) != FALSE, "Cannot convert the setup target owner");
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> ownerText(rawOwnerSid, LocalFree);
    if (ownerSid != ownerText.get())
        throw std::runtime_error(
            "The engine preview setup folder belongs to another user. Delete it and set up again.");
}

void CreatePrivateSetupTarget(const fs::path& target)
{
    const std::wstring sid = ReadCurrentUserSid();
    // A protected, inheritable DACL keeps ProgramData copies private regardless of parent permissions.
    const std::wstring   dacl          = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    RequireWindows(ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &rawDescriptor,
                                                                        nullptr) != FALSE,
                   "Cannot build private setup permissions");
    std::unique_ptr<void, decltype(&LocalFree)> descriptor(rawDescriptor, LocalFree);
    SECURITY_ATTRIBUTES                         security = {sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    if (!CreateDirectoryW(target.c_str(), &security))
    {
        RequireWindows(GetLastError() == ERROR_ALREADY_EXISTS, "Cannot create setup target");
        // Any user can pre-create folders under ProgramData, and an owner can always rewrite the DACL,
        // so an existing folder is reused only when this user owns it.
        RefuseForeignOwnedFolder(target, sid);
        // An empty folder left by an earlier attempt must get the same explicit protection.
        BOOL daclPresent   = FALSE;
        BOOL daclDefaulted = FALSE;
        PACL privateDacl   = nullptr;
        RequireWindows(GetSecurityDescriptorDacl(descriptor.get(), &daclPresent, &privateDacl, &daclDefaulted) != FALSE,
                       "Cannot read private setup permissions");
        const DWORD applied = SetNamedSecurityInfoW(const_cast<wchar_t*>(target.c_str()), SE_FILE_OBJECT,
                                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                    nullptr, nullptr, privateDacl, nullptr);
        if (applied != ERROR_SUCCESS) throw std::runtime_error("Cannot protect setup target permissions.");
    }
    RequireWindows(LocalFree(descriptor.get()) == nullptr, "Cannot free private setup permissions");
    descriptor.release();
}

void LockSetupJournal(HANDLE journal)
{
    // Read sharing alone permits another remover; the byte-range lock excludes other journal readers and refreshes.
    OVERLAPPED range = {};
    RequireWindows(LockFileEx(journal, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                              &range) != FALSE,
                   "Cannot exclusively lock the setup journal");
}

void RefuseReparseAncestors(fs::path path)
{
    while (!path.empty())
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Setup cannot use a junction or symbolic link: " + path.u8string());
        const fs::path parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}

std::string ReadSetupFile(const fs::path& path)
{
    RefuseReparseAncestors(path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path.u8string() + ".");
    const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (stream.bad()) throw std::runtime_error("Cannot finish reading " + path.u8string() + ".");
    return bytes;
}

std::string HashSetupFile(const fs::path& path)
{
    const std::string bytes = ReadSetupFile(path);
    const std::string hash  = Sha256Hex(bytes.data(), bytes.size());
    if (hash.empty()) throw std::runtime_error("Cannot calculate SHA-256 for " + path.u8string() + ".");
    return hash;
}

void WriteSetupBytes(HANDLE file, const std::string& bytes)
{
    constexpr size_t kWriteChunkBytes = 1024 * 1024;
    size_t           done             = 0;
    while (done < bytes.size())
    {
        const DWORD count   = static_cast<DWORD>((std::min)(kWriteChunkBytes, bytes.size() - done));
        DWORD       written = 0;
        RequireWindows(WriteFile(file, bytes.data() + done, count, &written, nullptr) && written == count,
                       "Cannot write setup file");
        done += written;
    }
    RequireWindows(FlushFileBuffers(file) != FALSE, "Cannot flush setup file");
}

DWORD SetupVolumeSerial(const fs::path& path)
{
    wchar_t volume[kProcessPathChars] = {};
    RequireWindows(GetVolumePathNameW(path.c_str(), volume, kProcessPathChars) != FALSE, "Cannot locate volume");
    DWORD serial = 0;
    RequireWindows(GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) != FALSE,
                   "Cannot read volume serial number");
    return serial;
}

void ValidateSetupTarget(const fs::path& target, const EngineHostSetup::Job& job)
{
    RefuseReparseAncestors(target);
    wchar_t    windows[MAX_PATH + 1] = {};
    const UINT length                = GetWindowsDirectoryW(windows, MAX_PATH + 1);
    if (length == 0 || length > MAX_PATH) throw std::runtime_error("Cannot resolve the Windows directory.");
    std::vector<std::string> roots;
    for (const std::wstring& root : job.basepaths) roots.push_back(WideToUtf8(root));
    std::vector<std::string> gitAncestors;
    for (fs::path ancestor = target; !ancestor.empty();)
    {
        if (fs::exists(ancestor / L".git")) gitAncestors.push_back(ancestor.u8string());
        const fs::path parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    const std::string refusal =
        FindSetupTargetRefusal(target.u8string(), roots, fs::path(job.editorExePath).parent_path().u8string(),
                               WideToUtf8(windows), gitAncestors);
    if (!refusal.empty()) throw std::runtime_error(refusal);
}

// Deepest paths first, so per-process scene folders are empty by the time they are reached.
void RemoveHostRuntimeOutputs(const fs::path& target)
{
    std::vector<fs::path> outputs;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(target))
    {
        if (entry.is_symlink()) continue;
        if (IsHostRuntimeOutput(entry.path().lexically_relative(target).generic_u8string()))
            outputs.push_back(entry.path());
    }
    std::sort(outputs.begin(), outputs.end(),
              [](const fs::path& left, const fs::path& right) -> bool { return left.native() > right.native(); });
    for (const fs::path& output : outputs)
        if (!fs::is_directory(output) || fs::is_empty(output)) fs::remove(output);
}

// The journal remains on partial removal so retry can delete the remaining entries.
void RemoveJournaledSetup(const fs::path& target, const ProgressReporter& progressed)
{
    RefuseReparseAncestors(target);
    if (!fs::exists(target)) return;
    if (EngineHostSetup::IsRunning(target.wstring()))
        throw std::runtime_error("Stop the live engine view first; a process is running from the managed folder.");
    const fs::path journal = target / kSetupJournalName;
    if (!fs::exists(journal))
    {
        if (!fs::is_empty(target))
            throw std::runtime_error("Cannot remove an unjournaled, nonempty engine host folder.");
        fs::remove(target);
        return;
    }
    // Denying write sharing refuses removal while another editor instance is still building this copy.
    SetupHandle journalLock(CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    RequireWindows(journalLock.get() != INVALID_HANDLE_VALUE, "Cannot lock the setup journal for removal");
    const std::string contents = ReadSetupFile(journal);
    LockSetupJournal(journalLock.get());
    std::vector<fs::path> entries;
    size_t                start = 0;
    while (start < contents.size())
    {
        const size_t end = contents.find('\n', start);
        // A crash during append cannot have created the unflushed final entry.
        if (end == std::string::npos) break;
        const std::string relative = contents.substr(start, end - start);
        start                      = end + 1;
        if (relative + '\n' == kSetupCompletionRecord) continue;
        if (!IsSafeSetupRelativePath(relative) || FoldSetupText(relative) == kSetupJournalName)
            throw std::runtime_error("The setup journal contains an unsafe path.");
        const fs::path path = target / fs::u8path(relative);
        RefuseReparseAncestors(path);
        const fs::path staged = StagedRefreshPath(path);
        RefuseReparseAncestors(staged);
        entries.push_back(path);
        entries.push_back(staged);
    }
    RemoveHostRuntimeOutputs(target);
    size_t done = 0;
    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
    {
        // Runtime files are not ours to delete, even inside a journaled directory.
        const bool keepsUnjournaledFiles = fs::is_directory(*entry) && !fs::is_empty(*entry);
        if (!keepsUnjournaledFiles) fs::remove(*entry);
        progressed("Removing private copy", ++done, entries.size());
    }
    journalLock.reset();
    fs::remove(journal);
    if (fs::is_empty(target)) fs::remove(target);
}

class SetupWriter
{
public:
    explicit SetupWriter(const fs::path& target) : m_target(target)
    {
        fs::create_directories(target.parent_path());
        const bool createdTarget = !fs::exists(target);
        CreatePrivateSetupTarget(target);
        m_journal.reset(CreateFileW((target / kSetupJournalName).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (m_journal.get() == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (createdTarget && fs::is_empty(target)) fs::remove(target);
            throw std::runtime_error("Cannot create setup journal (Windows error " + std::to_string(error) + ").");
        }
    }

    void Record(const std::string& relative)
    {
        if (!IsSafeSetupRelativePath(relative)) throw std::runtime_error("Unsafe setup output path.");
        WriteSetupBytes(m_journal.get(), relative + '\n');
    }

    void RecordCompletion() { WriteSetupBytes(m_journal.get(), kSetupCompletionRecord); }

    void EnsureDirectory(const fs::path& relative)
    {
        if (relative.empty() || fs::exists(m_target / relative)) return;
        EnsureDirectory(relative.parent_path());
        Record(relative.generic_u8string());
        RequireWindows(CreateDirectoryW((m_target / relative).c_str(), nullptr) != FALSE,
                       "Cannot create setup directory");
    }

    void WriteFresh(const std::string& relative, const std::string& bytes)
    {
        const fs::path path = fs::u8path(relative);
        EnsureDirectory(path.parent_path());
        Record(relative);
        const SetupHandle file(CreateFileW((m_target / path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, nullptr));
        RequireWindows(file.get() != INVALID_HANDLE_VALUE, "Cannot create " + relative);
        WriteSetupBytes(file.get(), bytes);
    }

    void Copy(const fs::path& source, const fs::path& relative, bool link)
    {
        RefuseReparseAncestors(source);
        EnsureDirectory(relative.parent_path());
        Record(relative.generic_u8string());
        const fs::path destination = m_target / relative;
        const BOOL     copied      = link ? CreateHardLinkW(destination.c_str(), source.c_str(), nullptr)
                                          : CopyFileW(source.c_str(), destination.c_str(), TRUE);
        RequireWindows(copied != FALSE, "Cannot " + std::string(link ? "link " : "copy ") + source.u8string());
    }

private:
    fs::path    m_target;
    SetupHandle m_journal;
};

fs::path FindCorruptionData(const std::vector<std::wstring>& roots)
{
    for (const std::wstring& root : roots)
        if (gameroots::ClassifyGameRoot(root) == gameroots::RootFlavor::FoC) return fs::path(root) / L"Data";
    throw std::runtime_error(
        "Engine preview setup requires a Forces of Corruption root (corruption) in the game paths.");
}

SetupPackage VerifySetupPackage(const fs::path&          manifest,
                                const ProgressReporter&  progressed,
                                const std::atomic<bool>& cancelRequested)
{
    if (!manifest.is_absolute() || manifest.filename() != kPackageManifestName)
        throw std::runtime_error("Invalid bundled host manifest path.");
    if (!fs::is_regular_file(manifest))
        throw std::runtime_error("The bundled engine-host folder is missing or damaged. "
                                 "Reinstall the Particle Editor from its release zip.");
    const SetupPackage package = ParseSetupPackage(ReadSetupFile(manifest));
    size_t             done    = 0;
    progressed("Verifying host package", 0, package.files.size());
    for (const std::pair<const std::string, std::string>& file : package.files)
    {
        ThrowIfSetupCancelled(cancelRequested);
        if (HashSetupFile(manifest.parent_path() / fs::u8path(file.first)) != file.second)
            throw std::runtime_error("The host package file failed SHA-256 verification: " + file.first);
        progressed("Verifying host package", ++done, package.files.size());
    }
    return package;
}

SetupPackage VerifySetupInputsAndSetEditorHash(const EngineHostSetup::Job& job,
                                               std::string&                editorHash,
                                               const ProgressReporter&     progressed,
                                               const std::atomic<bool>&    cancelRequested)
{
    const SetupPackage package = VerifySetupPackage(fs::path(job.packageManifestPath), progressed, cancelRequested);
    ThrowIfSetupCancelled(cancelRequested);
    const fs::path editor(job.editorExePath);
    if (!editor.is_absolute() || FoldSetupText(editor.extension().u8string()) != ".exe")
        throw std::runtime_error("Choose an absolute map editor executable path.");
    editorHash = HashSetupFile(editor);
    FindSetupEditorLabel(package, editorHash); // Throws when the editor build is unsupported.
    return package;
}

std::string BuildSetupIniBytes(const fs::path& target)
{
    // GetPrivateProfileStringW reads Unicode paths only from a UTF-16 BOM ini.
    const std::wstring ini = L"\ufeff" + Utf8ToWide(BuildSetupIni(target.u8string()));
    return std::string(reinterpret_cast<const char*>(ini.data()), ini.size() * sizeof(wchar_t));
}

std::string ReadRefreshedSetupBytes(const RefreshedSetupFile& file,
                                    const SetupPackage&       package,
                                    const fs::path&           packageFolder,
                                    const fs::path&           target)
{
    if (file.packageSource.empty()) return BuildSetupIniBytes(target);
    const std::string bytes = ReadSetupFile(packageFolder / fs::u8path(file.packageSource));
    // Verify the bytes used for overrides again, so a modified package cannot be installed after verification.
    if (Sha256Hex(bytes.data(), bytes.size()) != package.files.at(FoldSetupText(file.packageSource)))
        throw std::runtime_error("The host package changed during setup: " + file.packageSource);
    return bytes;
}

bool IsInstalledSetupCurrent(const fs::path& target, const SetupPackage& package, const std::string& editorHash)
{
    std::map<std::string, std::string> expectedHashes;
    std::map<std::string, std::string> installedHashes;
    for (const RefreshedSetupFile& file : ListRefreshedSetupFiles(package, editorHash))
    {
        const fs::path installed = target / fs::u8path(file.destination);
        if (file.packageSource.empty())
        {
            if (!fs::is_regular_file(installed) || ReadSetupFile(installed) != BuildSetupIniBytes(target)) return false;
            continue;
        }
        expectedHashes[file.destination] = package.files.at(FoldSetupText(file.packageSource));
        if (fs::is_regular_file(installed)) installedHashes[file.destination] = HashSetupFile(installed);
    }
    return IsSetupCopyCurrent(expectedHashes, installedHashes);
}

void StageRefreshedSetupFile(const RefreshedSetupFile& file,
                             const SetupPackage&       package,
                             const fs::path&           packageFolder,
                             const fs::path&           target,
                             std::vector<fs::path>&    stagedPaths)
{
    const fs::path destination = target / fs::u8path(file.destination);
    RefuseReparseAncestors(destination);
    if (!fs::is_directory(destination.parent_path())) throw std::runtime_error(kIncompleteSetupMessage);
    const std::string bytes  = ReadRefreshedSetupBytes(file, package, packageFolder, target);
    const fs::path    staged = StagedRefreshPath(destination);
    // A refresh killed after staging leaves this unjournaled file behind; it is ours by its suffix.
    fs::remove(staged);
    {
        const SetupHandle output(
            CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        RequireWindows(output.get() != INVALID_HANDLE_VALUE, "Cannot stage " + file.destination);
        stagedPaths.push_back(staged);
        WriteSetupBytes(output.get(), bytes);
    }
    const bool matches = file.packageSource.empty()
                             ? ReadSetupFile(staged) == bytes
                             : HashSetupFile(staged) == package.files.at(FoldSetupText(file.packageSource));
    if (!matches) throw std::runtime_error("The staged engine host file failed verification: " + file.destination);
}

void RefreshSetupCopy(const EngineHostSetup::Job& job, const fs::path& target, const ProgressReporter& progressed)
{
    RefuseReparseAncestors(target);
    if (EngineHostSetup::IsRunning(target.wstring()))
        throw std::runtime_error("Stop the live engine view first; a process is running from the managed folder.");
    const fs::path journal = target / kSetupJournalName;
    const fs::path editor  = target / kManagedEditorExeName;
    if (!fs::is_regular_file(journal) || !fs::is_regular_file(editor))
        throw std::runtime_error(kIncompleteSetupMessage);
    RefuseReparseAncestors(journal);
    // Denying write sharing keeps setup writers and journal deletion out for the entire refresh.
    const SetupHandle journalLock(CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, nullptr));
    RequireWindows(journalLock.get() != INVALID_HANDLE_VALUE, "Cannot lock the setup journal for refresh");
    if (!IsSetupJournalComplete(ReadSetupFile(journal))) throw std::runtime_error(kIncompleteSetupMessage);
    LockSetupJournal(journalLock.get());
    // Refresh is not cancellable (it takes about a second); the package check still needs a flag.
    const std::atomic<bool> cancelRequested{false};
    const SetupPackage package    = VerifySetupPackage(fs::path(job.packageManifestPath), progressed, cancelRequested);
    const std::string  editorHash = HashSetupFile(editor);
    const std::vector<RefreshedSetupFile> files = ListRefreshedSetupFiles(package, editorHash);
    std::vector<fs::path>                 stagedPaths;
    try
    {
        progressed("Refreshing engine host", 0, files.size());
        for (const RefreshedSetupFile& file : files)
            StageRefreshedSetupFile(file, package, fs::path(job.packageManifestPath).parent_path(), target,
                                    stagedPaths);
        size_t done = 0;
        for (const RefreshedSetupFile& file : files)
        {
            const fs::path destination = target / fs::u8path(file.destination);
            const fs::path staged      = StagedRefreshPath(destination);
            RequireWindows(MoveFileExW(staged.c_str(), destination.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
                           "Cannot refresh " + file.destination);
            progressed("Refreshing engine host", ++done, files.size());
        }
    }
    catch (const std::exception& failure)
    {
        std::string error = failure.what();
        for (const fs::path& staged : stagedPaths)
        {
            std::error_code cleanupError;
            fs::remove(staged, cleanupError);
            if (cleanupError)
                error += " Cannot remove staged file " + staged.u8string() + ": " + cleanupError.message();
        }
        throw std::runtime_error(error);
    }
}

void CopySetupGameData(SetupWriter&             writer,
                       const fs::path&          gameData,
                       const fs::path&          target,
                       const ProgressReporter&  progressed,
                       const std::atomic<bool>& cancelRequested)
{
    RefuseReparseAncestors(gameData);
    const bool link = ShouldLinkGameFile(SetupVolumeSerial(target), SetupVolumeSerial(gameData));
    progressed("Scanning FoC data", 0, 0);
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(gameData))
    {
        ThrowIfSetupCancelled(cancelRequested);
        RefuseReparseAncestors(entry.path());
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    size_t done = 0;
    for (const fs::path& source : files)
    {
        ThrowIfSetupCancelled(cancelRequested);
        const fs::path relative = source.lexically_relative(gameData);
        if (!IsSetupOwnedGameFile(relative.generic_u8string())) writer.Copy(source, fs::path(L"Data") / relative, link);
        progressed(link ? "Linking game data" : "Copying game data", ++done, files.size());
    }
}

void BuildSetupCopy(const EngineHostSetup::Job& job,
                    const fs::path&             target,
                    const ProgressReporter&     progressed,
                    const std::atomic<bool>&    cancelRequested)
{
    ThrowIfSetupCancelled(cancelRequested);
    ValidateSetupTarget(target, job);
    const fs::path     gameData = FindCorruptionData(job.basepaths);
    std::string        editorHash;
    const SetupPackage package = VerifySetupInputsAndSetEditorHash(job, editorHash, progressed, cancelRequested);
    ThrowIfSetupCancelled(cancelRequested);
    if (fs::exists(target) && !fs::is_empty(target))
        throw std::runtime_error("The managed engine host folder is not empty. Remove its previous setup first.");
    if (EngineHostSetup::IsRunning(target.wstring()))
        throw std::runtime_error("Stop the live engine view first; a process is running from the managed folder.");
    // Each worker opens independent MEG handles, including the active layer precedence.
    FileManager files(job.basepaths);
    files.SetLayers(job.contentRoots);
    const std::vector<unsigned char> xml         = ReadAndRelease(files.getFile("DATA\\XML\\GAMEOBJECTFILES.XML"));
    const std::string                gameObjects = InsertPreviewGameObjectFile(std::string(xml.begin(), xml.end()));
    bool                             ownsJournal = false;
    try
    {
        ThrowIfSetupCancelled(cancelRequested);
        SetupWriter writer(target);
        ownsJournal = true;
        writer.Copy(fs::path(job.editorExePath), kManagedEditorExeName, false);
        if (HashSetupFile(target / kManagedEditorExeName) != editorHash)
            throw std::runtime_error("The map editor changed during setup.");
        for (const fs::directory_entry& entry : fs::directory_iterator(fs::path(job.editorExePath).parent_path()))
        {
            ThrowIfSetupCancelled(cancelRequested);
            if (entry.is_regular_file() && ShouldCopyEditorEntry(entry.path().filename().u8string()))
                writer.Copy(entry.path(), entry.path().filename(), false);
        }
        CopySetupGameData(writer, gameData, target, progressed, cancelRequested);
        ThrowIfSetupCancelled(cancelRequested);
        writer.WriteFresh("Data/XML/GAMEOBJECTFILES.XML", gameObjects);
        const fs::path packageFolder = fs::path(job.packageManifestPath).parent_path();
        writer.EnsureDirectory(L"Data/Art/Models");
        // WriteEnginePreviewCopy needs a document. Live start writes this reserved path before opening the map.
        writer.Record("Data/Art/Models/P_PREVIEW_TEST.ALO");
        writer.EnsureDirectory(L"scene");
        for (const RefreshedSetupFile& file : ListRefreshedSetupFiles(package, editorHash))
        {
            ThrowIfSetupCancelled(cancelRequested);
            writer.WriteFresh(file.destination, ReadRefreshedSetupBytes(file, package, packageFolder, target));
        }
        ThrowIfSetupCancelled(cancelRequested);
        writer.RecordCompletion();
        progressed("Setup completed", 1, 1);
    }
    catch (const std::exception& failure)
    {
        // A failed journal creation may belong to a concurrent setup; never roll back its entries.
        if (!ownsJournal) throw;
        const std::string error = failure.what();
        try
        {
            RemoveJournaledSetup(target, progressed);
        }
        catch (const std::exception& rollback)
        {
            throw std::runtime_error(error + " Rollback: " + rollback.what());
        }
        throw std::runtime_error(error);
    }
}
} // namespace

std::wstring EngineHostSetup::ManagedPath()
{
    wchar_t       localAppData[MAX_PATH] = {};
    const HRESULT resolved = SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData);
    if (FAILED(resolved)) throw std::runtime_error("Cannot resolve Local AppData for engine preview setup.");
    const std::wstring localPath = (fs::path(localAppData) / kManagedHostRelativePath).wstring();
    if (IsPrintableAsciiSetupPath(localPath)) return localPath;
    wchar_t*      rawProgramData = nullptr;
    const HRESULT sharedResolved = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &rawProgramData);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> programData(rawProgramData, CoTaskMemFree);
    if (FAILED(sharedResolved)) throw std::runtime_error("Cannot resolve ProgramData for engine preview setup.");
    const std::wstring sharedPath =
        (fs::path(programData.get()) / kManagedHostRelativePath / ReadCurrentUserSid()).wstring();
    const std::wstring chosen = ChooseManagedHostPath(localPath, sharedPath);
    if (chosen.empty())
        throw std::runtime_error("The engine preview needs a folder whose path uses only plain ASCII characters, "
                                 "and none was found.");
    return chosen;
}

bool EngineHostSetup::IsSetupComplete(const std::wstring& target)
{
    const fs::path journal = fs::path(target) / kSetupJournalName;
    return fs::is_regular_file(journal) && IsSetupJournalComplete(ReadSetupFile(journal));
}

EngineHostSetup::RefreshStatus EngineHostSetup::CheckRefreshStatus(const std::wstring& target,
                                                                   const std::wstring& packageManifestPath)
{
    const fs::path managed(target);
    if (!fs::is_regular_file(managed / kSetupJournalName)) return {};
    const fs::path manifest(packageManifestPath);
    if (!fs::is_regular_file(manifest))
        throw std::runtime_error("The bundled engine-host folder is missing or damaged. "
                                 "Re-extract the Particle Editor release zip.");
    const SetupPackage package = ParseSetupPackage(ReadSetupFile(manifest));
    RefreshStatus      status;
    status.bundledHostVersion = package.hostVersion;
    // A missing or unsupported managed editor counts as out of date, so Preferences offers a refresh
    // whose refusal names the problem.
    status.isHostOutOfDate = true;
    const fs::path editor  = managed / kManagedEditorExeName;
    if (!fs::is_regular_file(editor)) return status;
    const std::string editorHash = HashSetupFile(editor);
    if (package.editors.count(editorHash) == 0) return status;
    status.isHostOutOfDate = !IsInstalledSetupCurrent(managed, package, editorHash);
    return status;
}

bool EngineHostSetup::IsRunning(const std::wstring& target)
{
    const SetupHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    RequireWindows(snapshot.get() != INVALID_HANDLE_VALUE, "Cannot enumerate running processes");
    PROCESSENTRY32W entry  = {};
    entry.dwSize           = sizeof(entry);
    BOOL  found            = Process32FirstW(snapshot.get(), &entry);
    DWORD enumerationError = found ? ERROR_SUCCESS : GetLastError();
    while (found)
    {
        // Protected and exiting processes cannot be inspected; neither can be running from the
        // managed folder, which only this user's editor launches.
        const SetupHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        wchar_t           image[kProcessPathChars] = {};
        DWORD             chars                    = kProcessPathChars;
        const bool        hasImage = process && QueryFullProcessImageNameW(process.get(), 0, image, &chars) != FALSE;
        if (hasImage && engine_host::IsSetupPathWithin(WideToUtf8(image), WideToUtf8(target))) return true;
        found            = Process32NextW(snapshot.get(), &entry);
        enumerationError = found ? ERROR_SUCCESS : GetLastError();
    }
    if (enumerationError != ERROR_NO_MORE_FILES)
        throw std::runtime_error("Cannot finish enumerating running processes (Windows error " +
                                 std::to_string(enumerationError) + ").");
    return false;
}

EngineHostSetup::EngineHostSetup(HWND hwnd) : m_hwnd(hwnd) {}
bool EngineHostSetup::IsBusy() const
{
    return m_busy;
}

std::optional<EngineHostSetup::Mode> EngineHostSetup::ActiveMode() const
{
    if (!IsBusy()) return std::nullopt;
    return m_mode;
}

bool EngineHostSetup::Start(Job job, std::string& error)
{
    if (m_busy.exchange(true))
    {
        error = "Engine host setup is already running.";
        return false;
    }
    Finish();
    m_mode            = job.mode;
    m_cancelRequested = false;
    try
    {
        m_worker = std::thread([this, job = std::move(job)]() -> void { Run(job); });
    }
    catch (const std::exception& failure)
    {
        m_busy = false;
        error  = std::string("Cannot start the engine host setup worker: ") + failure.what();
        return false;
    }
    return true;
}

void EngineHostSetup::Publish(Event event)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.push_back(std::move(event));
    }
    if (!PostMessageW(m_hwnd, WM_APP_ENGINE_HOST_SETUP_READY, 0, 0))
        OutputDebugStringA("[engine-host-setup] result notification failed\n");
}

void EngineHostSetup::ReportProgress(const std::string& stage, size_t done, size_t total)
{
    const ULONGLONG now = GetTickCount64();
    if (stage == m_lastStage && done != total && now - m_lastProgressMs < kSetupProgressIntervalMs) return;
    m_lastStage      = stage;
    m_lastProgressMs = now;
    Publish({false, {{"stage", stage}, {"done", done}, {"total", total}}});
}

void EngineHostSetup::Run(const Job& job)
{
    std::string action = "setup";
    if (job.mode == Mode::Refresh) action = "refresh";
    if (job.mode == Mode::Remove) action = "remove";
    nlohmann::json result = {{"ok", false}, {"action", action}};
    try
    {
        const fs::path         target(ManagedPath());
        const ProgressReporter progressed = [this](const std::string& stage, size_t done, size_t total) -> void
        {
            ReportProgress(stage, done, total);
        };
        if (job.mode == Mode::Remove)
            RemoveJournaledSetup(target, progressed);
        else if (job.mode == Mode::Refresh)
            RefreshSetupCopy(job, target, progressed);
        else
            BuildSetupCopy(job, target, progressed, m_cancelRequested);
        result["ok"] = true;
        if (job.mode == Mode::Setup) result["exePath"] = (target / kManagedEditorExeName).u8string();
    }
    catch (const std::exception& error)
    {
        result["error"] = error.what();
    }
    Publish({true, std::move(result)});
}

std::vector<EngineHostSetup::Event> EngineHostSetup::TakeEvents()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Event>          events;
    events.swap(m_events);
    for (const Event& event : events)
        if (event.finished) m_busy = false;
    return events;
}
} // namespace host
