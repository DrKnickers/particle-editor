// ModManager — implementation. Extracted from src/main.cpp in D6.
//
// Header comments document the why; this file documents the how.
// Internal helpers (ScanModsDir, ReadLastMod, WriteLastMod) are file-
// scope statics — they were `static` in main.cpp and stay private here.
// ReadModNickname / WriteModNickname are exposed in the header because
// the legacy nickname dialog calls them directly from a WM_COMMAND
// handler, and there's no benefit in routing that through a ModManager
// method.

#include "ModManager.h"

#include "engine.h"
#include "managers.h"
#include "UI/TexturePalette.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <vector>

#pragma comment(lib, "shlwapi.lib")  // PathIsDirectory

using std::wstring;
using std::vector;

// ---------------------------------------------------------------------------
// Registry helpers (file-scope private, plus the two exposed nickname ones).
// All four were `static` in src/main.cpp:3136-3203 before this extraction.
// ---------------------------------------------------------------------------

wstring ReadModNickname(const wstring& modPath)
{
    wstring nickname;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor\\ModNicknames", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        TCHAR  buf[256] = {0};
        DWORD  type;
        DWORD  size = sizeof(buf);
        if (RegQueryValueEx(hKey, modPath.c_str(), NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
        {
            nickname = buf;
        }
        RegCloseKey(hKey);
    }
    return nickname;
}

void WriteModNickname(const wstring& modPath, const wstring& nickname)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor\\ModNicknames", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        if (nickname.empty())
        {
            RegDeleteValue(hKey, modPath.c_str());
        }
        else
        {
            RegSetValueEx(hKey, modPath.c_str(), 0, REG_SZ,
                          (const BYTE*)nickname.c_str(),
                          (DWORD)((nickname.size() + 1) * sizeof(TCHAR)));
        }
        RegCloseKey(hKey);
    }
}

static wstring ReadLastMod()
{
    wstring path;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        TCHAR  buf[MAX_PATH] = {0};
        DWORD  type;
        DWORD  size = sizeof(buf);
        if (RegQueryValueEx(hKey, L"LastMod", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
        {
            path = buf;
        }
        RegCloseKey(hKey);
    }
    return path;
}

static void WriteLastMod(const wstring& modPath)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, L"LastMod", 0, REG_SZ,
                      (const BYTE*)modPath.c_str(),
                      (DWORD)((modPath.size() + 1) * sizeof(TCHAR)));
        RegCloseKey(hKey);
    }
}

// Last selected submod stack: an ORDERED list of folder NAMES (not paths;
// resolved under whatever mod is active at restore time, applied in order, each
// only if still present). Stored as REG_MULTI_SZ.
static vector<wstring> ReadLastSubmods()
{
    vector<wstring> out;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buf[1024] = {0};   // ample for a handful of short folder names
        DWORD   type = 0;
        DWORD   size = sizeof(buf);
        if (RegQueryValueEx(hKey, L"LastSubmods", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ)
        {
            for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
                out.push_back(p);
        }
        RegCloseKey(hKey);
    }
    return out;
}

static void WriteLastSubmods(const vector<wstring>& names)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        // Build a double-null-terminated REG_MULTI_SZ block. An empty list is a
        // single terminating null.
        wstring blob;
        for (const wstring& n : names) { blob += n; blob.push_back(L'\0'); }
        blob.push_back(L'\0');
        RegSetValueEx(hKey, L"LastSubmods", 0, REG_MULTI_SZ,
                      (const BYTE*)blob.data(),
                      (DWORD)(blob.size() * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------
// Discovery (file-scope helper + ModManager::DiscoverMods).
// ---------------------------------------------------------------------------

// Scan a single Mods\ directory for subfolders and append entries.
// Verbatim port from src/main.cpp:6872-6900 with the local `out`
// reference replaced by the caller-supplied vector.
static void ScanModsDir(const wstring& modsRoot, bool isFoC, vector<ModEntry>& out)
{
    wstring search = modsRoot;
    if (!search.empty() && search.back() != L'\\') search += L'\\';
    search += L"*";

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;

        ModEntry e;
        e.folderName = fd.cFileName;
        e.path       = modsRoot;
        if (!e.path.empty() && e.path.back() != L'\\') e.path += L'\\';
        e.path      += e.folderName;
        e.isFoC      = isFoC;
        e.nickname   = ReadModNickname(e.path);
        out.push_back(e);
    }
    while (FindNextFile(hFind, &fd));

    FindClose(hFind);
}

// Scan a mod root for submod folders: immediate subdirectories that carry
// their own Data\Art tree, EXCLUDING the shared "Core" core (always loaded).
// Returns folder names (not paths), sorted case-insensitively.
static void ScanSubmods(const wstring& modRoot, vector<wstring>& out)
{
    out.clear();
    if (modRoot.empty()) return;

    wstring base = modRoot;
    if (base.back() != L'\\' && base.back() != L'/') base += L'\\';

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile((base + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (_wcsicmp(fd.cFileName, L"Core") == 0) continue;   // shared core, always loaded

        // Only count folders that actually carry content (a Data\Art tree).
        const wstring art = base + fd.cFileName + L"\\Data\\Art";
        const DWORD attr = GetFileAttributesW(art.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;

        out.push_back(fd.cFileName);
    }
    while (FindNextFile(hFind, &fd));
    FindClose(hFind);

    std::sort(out.begin(), out.end(), [](const wstring& a, const wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
}

// ---------------------------------------------------------------------------
// ModManager.
// ---------------------------------------------------------------------------

ModManager::ModManager(IFileManager* fileManager,
                       const vector<wstring>& gameRoots)
    : m_fileManager(fileManager),
      m_gameRoots(gameRoots)
{}

void ModManager::SetEngine(Engine* engine)
{
    m_engine = engine;
}

void ModManager::DiscoverMods()
{
    m_mods.clear();
    for (const wstring& root : m_gameRoots)
    {
        // Strip trailing slashes; the leaf basename is the engine-flavor
        // discriminator (corruption/ → FoC, GameData/ → Base Game).
        wstring trimmed = root;
        while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();

        size_t sep  = trimmed.find_last_of(L"\\/");
        wstring leaf = (sep == wstring::npos) ? trimmed : trimmed.substr(sep + 1);

        bool isFoC;
        if (_wcsicmp(leaf.c_str(), L"corruption") == 0) isFoC = true;
        else if (_wcsicmp(leaf.c_str(), L"GameData") == 0) isFoC = false;
        else continue;

        wstring modsDir = trimmed + L"\\Mods";
        if (PathIsDirectory(modsDir.c_str()))
        {
            ScanModsDir(modsDir, isFoC, m_mods);
        }
    }

    // Sort: FoC mods first, then base game; within each, alphabetical by
    // folder name. Matches legacy ordering at src/main.cpp:6930-6933.
    std::sort(m_mods.begin(), m_mods.end(), [](const ModEntry& a, const ModEntry& b) {
        if (a.isFoC != b.isFoC) return a.isFoC && !b.isFoC;
        return _wcsicmp(a.folderName.c_str(), b.folderName.c_str()) < 0;
    });

    printf("[Mods] DiscoverMods: scanned %zu game roots, found %zu mods\n",
           m_gameRoots.size(), m_mods.size()); fflush(stdout);
}

void ModManager::RestoreLastSelectedMod()
{
    wstring savedMod = ReadLastMod();
    if (!savedMod.empty() && PathIsDirectory(savedMod.c_str()))
    {
        m_selectedModPath = savedMod;
        if (m_fileManager) m_fileManager->SetModPath(savedMod);
        printf("[Mods] Restored from registry: %S\n", savedMod.c_str()); fflush(stdout);
    }
    else
    {
        m_selectedModPath.clear();
        if (!savedMod.empty())
        {
            printf("[Mods] Saved mod path no longer exists, falling back to unmodded: %S\n", savedMod.c_str()); fflush(stdout);
        }
    }

    // — palette must follow whatever mod we settled on so its
    // INI state matches the active mod's textures from frame one.
    // Safe to call before the palette popup exists (--new-ui has no
    // popup; SetActiveMod is just a data-side state mutation).
    TexturePalette::Store::Instance().SetActiveMod(m_selectedModPath);

    // Submods live under the restored mod; discover them, then re-apply a
    // persisted submod selection if it still exists (SetModPath cleared it).
    // Lightweight (no engine reload — the engine isn't bound yet at restore):
    // just set the FileManager content root so first-frame lookups see it.
    DiscoverSubmods();
    m_selectedSubmods.clear();
    const vector<wstring> savedSubmods = ReadLastSubmods();
    for (const wstring& saved : savedSubmods)
    {
        // Match case-insensitively but adopt the DISCOVERED on-disk casing (the
        // menu compares strictly, so a case-only rename must not hide the check);
        // preserve the saved precedence order; drop names no longer present + dups.
        auto it = std::find_if(m_submods.begin(), m_submods.end(),
            [&](const wstring& s){ return _wcsicmp(s.c_str(), saved.c_str()) == 0; });
        if (it != m_submods.end() &&
            std::find(m_selectedSubmods.begin(), m_selectedSubmods.end(), *it) == m_selectedSubmods.end())
        {
            m_selectedSubmods.push_back(*it);
        }
    }
    if (!m_selectedSubmods.empty())
    {
        if (m_fileManager) m_fileManager->SetSubmods(m_selectedSubmods);
        printf("[Mods] Restored %zu submod(s)\n", m_selectedSubmods.size()); fflush(stdout);
    }
}

bool ModManager::SelectMod(const wstring& modPath)
{
    // 1. Internal state.
    m_selectedModPath = modPath;

    // 2. FileManager priority basepath. Empty path = Unmodded (clears).
    if (m_fileManager) m_fileManager->SetModPath(modPath);

    // 3. Registry persist so the next launch picks up where we left off.
    WriteLastMod(modPath);

    // 4–6. Texture palette + thumbnail cache. SetActiveMod flushes
    // dirty state from the previous mod and lazy-loads the new mod's
    // INI section. ClearThumbnailCache drops bitmaps that are keyed
    // by filename — a same-named texture in a different mod would
    // otherwise show the old mod's thumbnail. RefreshPopup is a
    // no-op when the legacy popup doesn't exist (--new-ui mode).
    TexturePalette::Store::Instance().SetActiveMod(modPath);
    TexturePalette::ClearThumbnailCache();
    TexturePalette::RefreshPopup();

    printf("[Mods] Selected: %S\n", modPath.empty() ? L"(unmodded)" : modPath.c_str()); fflush(stdout);

    // 7. Engine shader + texture hot-swap so the new mod takes effect
    // without restart. Shader reload may fail on a malformed mod
    // shader; we keep the previous shader set and return false so the
    // caller can surface the failure (legacy on status bar, new-UI on
    // engine/state/changed with a separate channel).
    // A new mod has its own submods; re-discover and reset the stack
    // (SetModPath already cleared the FileManager's submods). Persist the empty
    // stack so a relaunch on this mod starts with no submods.
    DiscoverSubmods();
    m_selectedSubmods.clear();
    WriteLastSubmods({});

    bool ok = true;
    if (m_engine != NULL)
    {
        if (!m_engine->ReloadShaders())
        {
            ok = false;
        }
        m_engine->ReloadTextures();
    }
    return ok;
}

// Populate m_submods from the active mod root. Pure discovery — doesn't
// touch the selection (SelectMod / RestoreLastSelectedMod own that).
void ModManager::DiscoverSubmods()
{
    ScanSubmods(m_selectedModPath, m_submods);
    printf("[Mods] DiscoverSubmods: %zu under %S\n", m_submods.size(),
           m_selectedModPath.empty() ? L"(none)" : m_selectedModPath.c_str()); fflush(stdout);
}

// Activate the ordered submod stack (empty = none): validate against the
// discovered set (drop unknowns + dups, adopt on-disk casing, keep order), rebuild
// content roots, persist, drop cached thumbnails, reload engine assets. Mirrors
// SelectMod's tail.
bool ModManager::SelectSubmods(const vector<wstring>& names)
{
    // Submods are only meaningful under an active mod. When Unmodded, ignore the
    // request and keep the stack cleared, so native matches the "submods belong to
    // the active mod" invariant the mock + UI assume (the FileManager stack is
    // already empty here -- SetModPath cleared it). Without this, a stray
    // select-submods while Unmodded would leave GetSelectedSubmods() non-empty and
    // the next mods/list would report submods that aren't really loadable.
    if (m_selectedModPath.empty())
    {
        m_selectedSubmods.clear();
        return true;
    }
    // Keep only discovered submods, in the requested precedence order, adopting the
    // on-disk casing and dropping unknowns + duplicates.
    m_selectedSubmods.clear();
    for (const wstring& n : names)
    {
        auto it = std::find_if(m_submods.begin(), m_submods.end(),
            [&](const wstring& s){ return _wcsicmp(s.c_str(), n.c_str()) == 0; });
        if (it != m_submods.end() &&
            std::find(m_selectedSubmods.begin(), m_selectedSubmods.end(), *it) == m_selectedSubmods.end())
        {
            m_selectedSubmods.push_back(*it);
        }
    }
    if (m_fileManager) m_fileManager->SetSubmods(m_selectedSubmods);
    WriteLastSubmods(m_selectedSubmods);

    // Same-named textures can differ between submods; drop cached thumbnails so
    // a switch doesn't render the previous stack's art.
    TexturePalette::ClearThumbnailCache();
    TexturePalette::RefreshPopup();

    printf("[Mods] Selected %zu submod(s)\n", m_selectedSubmods.size()); fflush(stdout);

    bool ok = true;
    if (m_engine != NULL)
    {
        if (!m_engine->ReloadShaders())
        {
            ok = false;
        }
        m_engine->ReloadTextures();
    }
    return ok;
}
