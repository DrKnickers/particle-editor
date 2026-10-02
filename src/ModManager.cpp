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
#include "GameRoots.h"
#include "host/SettingsRegistry.h"

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
    if (HKEY hKey = host::OpenSettingsKeyForRead(host::kRegModNicknamesPath))
    {
        // ReadRegSz terminates the value itself: a REG_SZ is NOT required to
        // be NUL-terminated.
        nickname = host::ReadRegSz(hKey, modPath.c_str());
        RegCloseKey(hKey);
    }
    return nickname;
}

static void WriteLastMod(const wstring& modPath)
{
    // Unmodded is recorded as an empty string, not a deleted value, so this
    // writes directly instead of through WriteRegSz (which deletes on empty).
    if (HKEY hKey = host::OpenSettingsKeyForWrite())
    {
        RegSetValueExW(hKey, L"LastMod", 0, REG_SZ,
                       (const BYTE*)modPath.c_str(),
                       (DWORD)((modPath.size() + 1) * sizeof(wchar_t)));
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
    if (HKEY hKey = host::OpenSettingsKeyForRead())
    {
        const std::vector<wchar_t> buf = host::ReadRegMultiSz(hKey, L"LastLayers");
        if (!buf.empty())
            out = modlayers::ParseMultiSz(buf.data(), buf.size());
        RegCloseKey(hKey);
    }
    return out;
}

// Returns false when the stack could NOT be persisted (key open or value write
// failed), so a locked or policy-blocked registry doesn't look like a
// successful save and the bridge doesn't answer {ok:true} (2026-07 audit).
// LastLayers is authoritative, so its failure is the caller's business.
static bool WriteLastLayers(const vector<wstring>& layers)
{
    return host::WriteRegMultiSz(L"LastLayers", modlayers::SerializeMultiSz(layers));
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
                       bool suppressPersistence)
    : m_suppressPersistence(suppressPersistence),
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
        const wstring trimmed = gameroots::TrimTrailingSlashes(root);
        const gameroots::RootFlavor flavor = gameroots::ClassifyGameRoot(trimmed);
        if (flavor == gameroots::RootFlavor::Other) continue;
        const bool isFoC = (flavor == gameroots::RootFlavor::FoC);

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
    // LastLayers is the only persisted stack. The one-time migration from the
    // legacy LastMod/LastSubmods pair was removed before the v0.3.0 release: no
    // released build ever wrote those values without also writing LastLayers, so
    // the only profiles it could act on were mid-development ones. Absent or
    // unreadable LastLayers now simply restores Unmodded.
    std::vector<wstring> stack = ReadLastLayers();

    // A RESTORE is not a user edit, so it must never write back. Preserve the
    // full configured value in memory as well: a path on an offline removable
    // drive is inactive for this session, not deleted from the user's order.
    // SetLayerStack tolerates m_engine == null during startup.
    SetLayerStack(stack, /*allowPersist=*/false);
    printf("[Mods] Restored %zu configured layer(s)\n", m_layerStack.size()); fflush(stdout);
}

bool ModManager::SetLayerStack(const vector<wstring>& absoluteLayers, bool allowPersist,
                               std::string* outError)
{
    // Every call site, including capture/quick-switch paths outside the bridge,
    // must reject before changing configured roots while D3D work is blocked.
    // ReloadShaders cannot legally validate the candidate stack in this state,
    // and deferred texture work would otherwise produce a mixed old-shader /
    // new-root session.
    if (m_engine != NULL && m_engine->DeviceCallsBlocked())
    {
        if (outError != nullptr)
            *outError =
                "the rendering device is unavailable; the load order was not changed";
        return false;
    }

    // Preserve the configured value even when a layer is temporarily
    // unavailable. Runtime activation is separate: primary is the first path
    // that exists now, and FileManager::SetLayers applies its own existence
    // filter before building content roots.
    const modlayers::ResolvedLayerStack resolved =
        modlayers::ResolveLayerStack(absoluteLayers, [](const wstring& path) {
            return PathIsDirectory(path.c_str()) != FALSE;
        });
    m_layerStack = resolved.configured;
    m_primaryLayerPath = resolved.primary;

    const wstring primary = GetPrimaryLayerPath();

    // 1. FileManager content roots (unavailable paths are filtered there;
    //    slashes are re-added by BuildContentRoots).
    if (m_fileManager) m_fileManager->SetLayers(m_layerStack);

    // 2. Registry persistence is DEFERRED to after the engine reload below, so a
    //    failed shader reload never records a stack the next launch cannot render
    //    (release-audit #5). The in-memory stack + content roots are applied now.

    // 3. Texture palette follows the primary layer. (The current path busts
    //    its own base64 thumbnail cache via the bridge palette refresh —
    //    BridgeDispatcher ClearBridgeThumbCache; the legacy GDI popup
    //    cache-clear / refresh was removed with the old UI.)
    TexturePalette::Store::Instance().SetActiveMod(primary);

    printf("[Mods] Layer stack: %zu configured layer(s), primary=%S\n",
           m_layerStack.size(), primary.empty() ? L"(unmodded)" : primary.c_str());
    fflush(stdout);

    // 4. Engine hot-swap (if bound).
    bool ok = true;
    if (m_engine != NULL)
    {
        if (!m_engine->ReloadShaders()) ok = false;
        m_engine->ReloadTextures();
    }

    // 5. Persist the stack to the registry ONLY if the reload succeeded
    //    (release-audit #5): LastLayers is authoritative; LastMod = primary is a
    //    write-only best-effort record (nothing in the editor reads it anymore).
    //    On a failed reload we leave the registry untouched so the next launch boots
    //    the last-known-good stack, not one whose shaders failed to load.
    //    (A headless run — m_suppressPersistence — never rewrites the daily
    //    driver's mod stack.)
    if (allowPersist && !m_suppressPersistence && modlayers::ShouldPersistLayers(ok))
    {
        if (!WriteLastLayers(m_layerStack))
        {
            // Reported, not swallowed: the in-memory switch succeeded but the
            // choice will not survive a restart, and only the user can act on
            // that. LastMod stays best-effort — nothing reads it any more.
            ok = false;
            if (outError != nullptr)
                *outError = "the load order was applied but could not be saved "
                            "to the registry, so it will not survive a restart";
            printf("[Mods] WARNING: LastLayers persist FAILED\n"); fflush(stdout);
        }
        WriteLastMod(primary);
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
