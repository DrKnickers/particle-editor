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
