// ModManager — single source of truth for mod discovery and active-mod
// state. Driven by the host bridge (src/host/BridgeDispatcher.cpp);
// extracted in D6 from the inline mod-discovery code that previously
// lived in main.cpp.
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
//   3. `RestoreLastLayerStack()` reads HKCU\Software\AloParticleEditor\
//      LastLayers (with a one-time migration from the legacy LastMod +
//      LastSubmods) and applies the persisted ordered content-layer
//      stack so startup behaviour matches the prior session.
//   4. `SetEngine(Engine*)` after the Engine is built. Required for
//      the shader/texture reload inside SetLayerStack. In --new-ui mode
//      the Engine doesn't exist at ModManager-construction time, so
//      this is a separate step.
//   5. `SetLayerStack(layers)` is the atomic activation chain (an empty
//      stack means Unmodded). `SelectMod(modPath)` is the one-layer
//      quick-switch shorthand. Callers add their own per-mode
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

#include "ModScan.h"   // LayerRef + the Win32 nested-layer scan

class Engine;
class IFileManager;

// A single discovered mod entry + its nested layers (immediate child folders
// that carry a Data\Art tree, e.g. Mod's Mod/GCW/Rev/TR/Core).
struct ModEntry
{
    std::wstring path;        // full path, e.g. D:\...\corruption\Mods\Mod
    std::wstring folderName;  // "Mod"
    std::wstring nickname;    // user-set, may be empty (read from registry)
    bool         isFoC;       // true if under corruption\Mods, false if under GameData\Mods
    bool         rootHasArt;  // the mod root itself carries a Data\Art tree
    std::vector<LayerRef> nested;  // child folders with a Data\Art tree
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

    // Reads HKCU\Software\AloParticleEditor\LastLayers and applies the
    // persisted ordered content-layer stack (one-time migration from the legacy
    // LastMod + LastSubmods when LastLayers is absent). Layers whose folder no
    // longer exists are dropped. Idempotent.
    void RestoreLastLayerStack();

    // Quick-switch shorthand: replace the whole stack with a single layer
    // (empty `modPath` = Unmodded). Delegates to SetLayerStack — see that method for
    // the full side-effect chain and the partial-success (false) semantics.
    bool SelectMod(const std::wstring& modPath);

    // Set the ordered content-layer stack (absolute paths, front = highest
    // precedence; [] = Unmodded). Canonicalised + de-duplicated; drives
    // FileManager::SetLayers, persists LastLayers (+ a best-effort LastMod = primary
    // kept in sync for the one-time legacy-selection migration), swaps the texture palette to the primary
    // layer, clears the thumbnail cache, and reloads engine assets (if bound).
    // Returns false if the engine shader reload failed (state still rolls forward).
    bool SetLayerStack(const std::vector<std::wstring>& absoluteLayers);

    // Read-only accessors.
    const std::vector<ModEntry>& GetMods() const { return m_mods; }
    const std::vector<std::wstring>& GetLayerStack() const { return m_layerStack; }
    // Primary layer = top of the stack (highest precedence), or empty when Unmodded.
    // The single-path "active mod" view for the texture palette, file dialogs, and
    // the snapshot's activeModPath (the quick-switch checkmark).
    std::wstring GetPrimaryLayerPath() const
    { return m_layerStack.empty() ? std::wstring() : m_layerStack.front(); }

private:
    IFileManager*             m_fileManager;
    Engine*                   m_engine = nullptr;
    std::vector<std::wstring> m_gameRoots;
    std::vector<ModEntry>     m_mods;
    std::vector<std::wstring> m_layerStack;   // absolute slash-free, [0]=highest
};

// Registry helpers for mod nicknames, exposed for direct use by the host
// bridge without going through ModManager — they're trivially
// side-effect-free and don't carry mutable state.
std::wstring ReadModNickname(const std::wstring& modPath);
void         WriteModNickname(const std::wstring& modPath,
                              const std::wstring& nickname);

#endif // MOD_MANAGER_H
