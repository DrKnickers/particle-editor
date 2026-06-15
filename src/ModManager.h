// ModManager — single source of truth for mod discovery and active-mod
// state. Shared between the legacy Win32 menu (src/main.cpp) and the
// new-UI bridge (src/host/BridgeDispatcher.cpp); extracted in D6
// from the inline mod-discovery code that previously lived in main.cpp.
//
// Why a separate class:
//   - The new-UI host doesn't construct APPLICATION_INFO (legacy WinMain
//     is unreachable when `--new-ui` is passed), so the legacy struct
//     wasn't a viable carrier for mod state in --new-ui mode.
//   - Both UI modes need to discover mods, restore the last-active mod
//     from the registry, and apply a mod selection's full side-effect
//     chain (FileManager swap + registry write + texture palette swap
//     + thumbnail cache clear + engine shader/texture reload).
//
// Lifecycle:
//   1. Construct with `IFileManager*` and the gameRoots vector that
//      created the FileManager.
//   2. `DiscoverMods()` scans the gameRoots' Mods\ subdirectories.
//      Idempotent — safe to call again on a Refresh.
//   3. `RestoreLastSelectedMod()` reads HKCU\Software\AloParticleEditor\
//      LastMod; if the path still exists on disk it becomes active
//      (calls FileManager::SetModPath + TexturePalette::SetActiveMod
//      so startup behaviour matches the legacy boot sequence).
//   4. `SetEngine(Engine*)` after the Engine is built. Required for
//      the shader/texture reload inside SelectMod. In --new-ui mode
//      the Engine doesn't exist at ModManager-construction time, so
//      this is a separate step.
//   5. `SelectMod(modPath)` is the atomic activation chain. Empty
//      path means Unmodded. Callers add their own per-mode
//      finalisation: legacy rebuilds the HMENU + invalidates the
//      render window; --new-ui fires `engine/state/changed`.
//
// Lifetime ordering: ModManager outlives FileManager only if the
// caller arranges it (FileManager pointer is borrowed, not owned).
// Engine pointer set via SetEngine is also borrowed.

#ifndef MOD_MANAGER_H
#define MOD_MANAGER_H

#include <string>
#include <vector>

class Engine;
class IFileManager;

// A single discovered mod entry. Mirrors the legacy struct that lived
// at src/main.cpp:421-427 before D6 extracted it here.
struct ModEntry
{
    std::wstring path;        // full path, e.g. D:\...\corruption\Mods\Mod
    std::wstring folderName;  // "Mod"
    std::wstring nickname;    // user-set, may be empty (read from registry)
    bool         isFoC;       // true if under corruption\Mods, false if under GameData\Mods
};

class ModManager
{
public:
    ModManager(IFileManager* fileManager,
               const std::vector<std::wstring>& gameRoots);

    // Late-bound engine pointer. Required before the first SelectMod
    // that needs to take visible effect (shader + texture reload).
    // Null is tolerated: SelectMod will still update FileManager +
    // registry + palette state, just won't trigger the engine refresh.
    void SetEngine(Engine* engine);

    // Scans the gameRoots' Mods\ subdirectories. Populates the internal
    // mods vector, sorted FoC-first then alphabetically by folder name.
    // Idempotent — safe to call on Refresh.
    void DiscoverMods();

    // Reads HKCU\Software\AloParticleEditor\LastMod. If the stored path
    // exists on disk, activates it (FileManager::SetModPath +
    // TexturePalette::SetActiveMod). If the path is missing or stale,
    // leaves state in the Unmodded position. Idempotent.
    void RestoreLastSelectedMod();

    // Atomic mod activation. The side-effect chain in order:
    //   1. m_selectedModPath = modPath
    //   2. fileManager->SetModPath(modPath)
    //   3. WriteLastMod(modPath) (registry persist)
    //   4. TexturePalette::Store::Instance().SetActiveMod(modPath)
    //   5. TexturePalette::ClearThumbnailCache()
    //   6. TexturePalette::RefreshPopup() (no-op if no popup exists)
    //   7. engine->ReloadShaders() + ReloadTextures() (if engine is bound)
    //
    // Returns true on full success. Returns false if shader reload
    // failed; state is still updated (matches legacy behaviour —
    // partial-success rolls forward, legacy reports the shader
    // failure on its status bar).
    //
    // Empty `modPath` means Unmodded. The registry entry is still
    // written (as an empty string) so the next launch restores
    // Unmodded explicitly.
    bool SelectMod(const std::wstring& modPath);

    // Submods — subfolders of the active mod that carry their own Data\Art
    // (e.g. Mod's Mod/GCW/Rev/TR). The shared Core core is one of them now
    // too — a normal selectable layer, no longer auto-loaded. Several can be stacked in
    // explicit precedence order (front wins) — the user mirrors their game launch stack.
    //
    // DiscoverSubmods() scans the active mod root for immediate subdirectories with a
    // Data\Art tree (Core INCLUDED), sorted alphabetically. Call after
    // SelectMod / RestoreLastSelectedMod. Pure discovery; doesn't change the selection.
    void DiscoverSubmods();

    // Activate the ordered submod stack (folder names, highest precedence first;
    // empty = none). Names not in the discovered set are dropped; the discovered
    // on-disk casing is adopted. Side-effect chain mirrors SelectMod's tail:
    // FileManager::SetSubmods (rebuild content roots) -> WriteLastSubmods (persist)
    // -> thumbnail-cache clear -> engine reload. Returns false if the engine shader
    // reload failed (state still rolls forward). No-op when no mod is active.
    bool SelectSubmods(const std::vector<std::wstring>& names);

    // Read-only accessors. Pointers/references are stable as long as
    // no concurrent mutation is in flight (this class is not
    // thread-safe; all calls must come from the UI thread).
    const std::vector<ModEntry>& GetMods() const { return m_mods; }
    const std::wstring& GetSelectedModPath() const { return m_selectedModPath; }
    const std::vector<std::wstring>& GetSubmods() const { return m_submods; }
    const std::vector<std::wstring>& GetSelectedSubmods() const { return m_selectedSubmods; }

private:
    IFileManager*             m_fileManager;
    Engine*                   m_engine = nullptr;
    std::vector<std::wstring> m_gameRoots;
    std::vector<ModEntry>     m_mods;
    std::wstring              m_selectedModPath;
    std::vector<std::wstring> m_submods;          // available under the active mod
    std::vector<std::wstring> m_selectedSubmods;  // ordered stack, highest precedence first
};

// Registry helpers, exposed for the legacy nickname dialog. Both
// modes can call these without going through ModManager — they're
// trivially side-effect-free and don't carry mutable state.
std::wstring ReadModNickname(const std::wstring& modPath);
void         WriteModNickname(const std::wstring& modPath,
                              const std::wstring& nickname);

#endif // MOD_MANAGER_H
