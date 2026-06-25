// ModManager — implementation. Extracted from src/main.cpp.
//
// Header comments document the why; this file documents the how.
// Internal helpers (ScanModsDir, ReadLastMod, WriteLastMod) are file-
// scope statics — they were `static` in main.cpp and stay private here.
// ReadModNickname is exposed in the header because the host bridge reads
// mod nicknames directly, and there's no benefit in routing that through
// a ModManager method.

#include "ModManager.h"
#include "ModScan.h"   // ScanModNestedLayers / ModRootHasArt (transitively ModLayers.h)

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
// Registry helpers (file-scope private, plus the one exposed nickname read).
// All were `static` in the legacy main.cpp before this extraction.
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

// Persisted ordered layer stack: absolute slash-free paths, front = highest.
// Stored as REG_MULTI_SZ. Read with a size-query + dynamic buffer — a multi-path
// stack easily exceeds any fixed buffer (the old fixed wchar[1024] would silently
// truncate to empty -> Unmodded).
static vector<wstring> ReadLastLayers()
{
    vector<wstring> out;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type = 0, bytes = 0;
        if (RegQueryValueEx(hKey, L"LastLayers", NULL, &type, NULL, &bytes) == ERROR_SUCCESS
            && type == REG_MULTI_SZ && bytes >= sizeof(wchar_t))
        {
            std::vector<wchar_t> buf(bytes / sizeof(wchar_t));
            if (RegQueryValueEx(hKey, L"LastLayers", NULL, &type, (LPBYTE)buf.data(), &bytes) == ERROR_SUCCESS)
                out = modlayers::ParseMultiSz(buf.data(), buf.size());
        }
        RegCloseKey(hKey);
    }
    return out;
}

static void WriteLastLayers(const vector<wstring>& layers)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        const std::wstring blob = modlayers::SerializeMultiSz(layers);
        RegSetValueEx(hKey, L"LastLayers", 0, REG_MULTI_SZ,
                      (const BYTE*)blob.data(), (DWORD)(blob.size() * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// Migration-only reader of the legacy LastSubmods (ordered folder NAMES).
// Dynamic buffer + ParseMultiSz (never the old fixed wchar[1024]).
static vector<wstring> ReadLastSubmodsLegacy()
{
    vector<wstring> out;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type = 0, bytes = 0;
        if (RegQueryValueEx(hKey, L"LastSubmods", NULL, &type, NULL, &bytes) == ERROR_SUCCESS
            && type == REG_MULTI_SZ && bytes >= sizeof(wchar_t))
        {
            std::vector<wchar_t> buf(bytes / sizeof(wchar_t));
            if (RegQueryValueEx(hKey, L"LastSubmods", NULL, &type, (LPBYTE)buf.data(), &bytes) == ERROR_SUCCESS)
                out = modlayers::ParseMultiSz(buf.data(), buf.size());
        }
        RegCloseKey(hKey);
    }
    return out;
}

// One-time migration marker. Core used to be auto-loaded; it is now a normal
// selectable/orderable submod layer. On the FIRST restore after this change we append
// Core to a legacy NON-EMPTY selection so an existing Mod/IR/TR stack keeps it. The
// flag then stops us re-adding it after the user deliberately removes it (e.g. for Rev).
static bool ReadCoreMigrated()
{
    HKEY hKey; DWORD val = 0, size = sizeof(val), type = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueEx(hKey, L"CoreLayerMigrated", NULL, &type, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return val != 0;
}

static void WriteCoreMigrated()
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        RegSetValueEx(hKey, L"CoreLayerMigrated", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------
// Discovery (file-scope helper + ModManager::DiscoverMods).
// ---------------------------------------------------------------------------

// Scan a single Mods\ directory for subfolders and append entries.
// Verbatim port from the legacy main.cpp implementation, with the local `out`
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
                       const vector<wstring>& gameRoots,
                       bool ephemeral)
    : m_ephemeral(ephemeral),
      m_fileManager(fileManager),
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
    // folder name. Matches the legacy ordering.
    std::sort(m_mods.begin(), m_mods.end(), [](const ModEntry& a, const ModEntry& b) {
        if (a.isFoC != b.isFoC) return a.isFoC && !b.isFoC;
        return _wcsicmp(a.folderName.c_str(), b.folderName.c_str()) < 0;
    });

    // Per mod, discover its nested layers + whether its own root has Data\Art.
    for (ModEntry& e : m_mods)
    {
        e.rootHasArt = ModRootHasArt(e.path);
        ScanModNestedLayers(e.path, e.nested);
    }

    printf("[Mods] DiscoverMods: scanned %zu game roots, found %zu mods\n",
           m_gameRoots.size(), m_mods.size()); fflush(stdout);
}

void ModManager::RestoreLastLayerStack()
{
    std::vector<wstring> stack = ReadLastLayers();

    // One-time migration: no LastLayers yet but a legacy LastMod/LastSubmods exists.
    if (stack.empty())
    {
        const wstring lastMod = ReadLastMod();
        if (!lastMod.empty())
        {
            const std::vector<wstring> lastSubmods = ReadLastSubmodsLegacy();
            stack = modlayers::MigrateLegacySelection(lastMod, lastSubmods, ReadCoreMigrated());
            if (!m_ephemeral)
                WriteCoreMigrated();   // the Core decision is now baked into LastLayers
        }
    }

    // Drop layers whose folder no longer exists (the existing ghost-drop behaviour);
    // SetLayerStack persists the validated stack + applies it (no engine reload yet —
    // the engine isn't bound at restore; SetLayerStack tolerates m_engine == null).
    std::vector<wstring> present;
    for (const wstring& p : stack)
        if (PathIsDirectory(modlayers::CanonicalizeLayerPath(p).c_str()))
            present.push_back(p);

    SetLayerStack(present);
    printf("[Mods] Restored %zu layer(s)\n", present.size()); fflush(stdout);
}

bool ModManager::SetLayerStack(const vector<wstring>& absoluteLayers)
{
    // Canonicalise, drop non-existent folders, and dedup (case-insensitive),
    // preserving order. The existence filter keeps m_layerStack / GetLayerStack()
    // / the persisted LastLayers free of ghost paths — matching what
    // FileManager::SetLayers already does for content roots and what
    // RestoreLastLayerStack's own pre-filter did (now redundant but harmless).
    // A migration-appended candidate (e.g. MigrateLegacySelection's
    // mod\Core) is dropped here too when that folder is absent.
    m_layerStack.clear();
    for (const wstring& raw : absoluteLayers)
    {
        const wstring c = modlayers::CanonicalizeLayerPath(raw);
        if (c.empty()) continue;
        if (!PathIsDirectory(c.c_str())) continue;
        bool dup = false;
        for (const wstring& s : m_layerStack)
            if (modlayers::LayerPathsEqual(s, c)) { dup = true; break; }
        if (!dup) m_layerStack.push_back(c);
    }

    const wstring primary = GetPrimaryLayerPath();

    // 1. FileManager content roots (slash re-added by BuildContentRoots).
    if (m_fileManager) m_fileManager->SetLayers(m_layerStack);

    // 2. Persist: LastLayers is authoritative; LastMod = primary is kept
    //    in sync so the one-time legacy-selection migration (ReadLastMod feeds
    //    MigrateLegacySelection) has a sane value if LastLayers is ever cleared.
    if (!m_ephemeral)   // --drive: never rewrite the daily driver's mod stack
    {
        WriteLastLayers(m_layerStack);
        WriteLastMod(primary);
    }

    // 3. Texture palette follows the primary layer. (busts its own
    //    base64 thumbnail cache via the bridge palette refresh —
    //    BridgeDispatcher ClearBridgeThumbCache; the legacy GDI popup
    //    cache-clear / refresh was removed with arch-A —.)
    TexturePalette::Store::Instance().SetActiveMod(primary);

    printf("[Mods] Layer stack: %zu layer(s), primary=%S\n",
           m_layerStack.size(), primary.empty() ? L"(unmodded)" : primary.c_str());
    fflush(stdout);

    // 4. Engine hot-swap (if bound).
    bool ok = true;
    if (m_engine != NULL)
    {
        if (!m_engine->ReloadShaders()) ok = false;
        m_engine->ReloadTextures();
    }
    return ok;
}

bool ModManager::SelectMod(const wstring& modPath)
{
    // Quick-switch = replace the whole stack with this single layer
    // (empty = Unmodded). Used by the host's mod quick-switch.
    return SetLayerStack(modPath.empty() ? std::vector<wstring>{}
                                         : std::vector<wstring>{ modPath });
}
