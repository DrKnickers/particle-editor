// ModManager — single source of truth for mod discovery and active-mod
// state. Driven by the host bridge (src/host/BridgeDispatcher.cpp);
// extracted from the inline mod-discovery code that previously
// lived in main.cpp.
//
// Why a separate class:
//   - The host doesn't construct the legacy APPLICATION_INFO struct, so
//     it wasn't a viable carrier for mod state.
//   - The host needs to discover mods, restore the last-active mod
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
//      LastLayers and applies the persisted ordered content-layer
//      stack so startup behaviour matches the prior session.
//   4. `SetEngine(Engine*)` after the Engine is built. Required for
//      the shader/texture reload inside SetLayerStack. The Engine doesn't
//      exist at ModManager-construction time, so this is a separate step.
//   5. `SetLayerStack(layers)` is the atomic activation chain (an empty
//      stack means Unmodded). `SelectMod(modPath)` is the one-layer
//      quick-switch shorthand. After the activation chain the host fires
//      `engine/state/changed` to refresh the React UI.
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
// that carry a Data\Art tree -- a submod-based mod ships several, plus a
// shared core folder; see ModLayers.h).
struct ModEntry
{
    std::wstring path;        // full path, e.g. D:\...\corruption\Mods\MyMod
    std::wstring folderName;  // "MyMod"
    std::wstring nickname;    // user-set, may be empty (read from registry)
    bool         isFoC;       // true if under corruption\Mods, false if under GameData\Mods
    bool         rootHasArt;  // the mod root itself carries a Data\Art tree
    std::vector<LayerRef> nested;  // child folders with a Data\Art tree
};

class ModManager
{
public:
    // ephemeral (true in --drive mode): suppress ALL registry writes
    // (LastLayers/LastMod) so a --drive run never rewrites the
    // daily driver's persisted mod stack — notably the startup write-back via
    // RestoreLastLayerStack -> SetLayerStack. HostWindow and BridgeDispatcher
    // carry related automation/persistence state under their own ownership.
    ModManager(IFileManager* fileManager,
               const std::vector<std::wstring>& gameRoots,
               bool ephemeral = false);

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
    // persisted ordered content-layer stack; absent or unreadable means
    // Unmodded. Temporarily unavailable paths remain configured but inactive
    // so a removable drive being offline cannot erase the saved order.
    // Idempotent.
    void RestoreLastLayerStack();

    // Quick-switch shorthand: replace the whole stack with a single layer
    // (empty `modPath` = Unmodded). Delegates to SetLayerStack — see that method for
    // the full side-effect chain and the partial-success (false) semantics.
    bool SelectMod(const std::wstring& modPath);

    // Set the ordered configured content-layer stack (absolute paths, front =
    // highest precedence; [] = Unmodded). Canonicalised + de-duplicated without
    // dropping temporarily unavailable paths. FileManager activates only paths
    // that currently exist; LastLayers retains the full configured value.
    // LastMod remains a best-effort write-only record of the first active layer.
    // Also swaps the texture palette, clears the thumbnail cache, and reloads
    // engine assets (if bound).
    // Returns false if the engine shader reload failed OR the stack could not be
    // persisted (state still rolls forward in memory either way).
    // allowPersist=false suppresses the registry write on top of the ephemeral
    // gate (the dispatcher passes the test-host settings gate through here so a
    // --test-host run never rewrites the daily driver's LastLayers/LastMod), and
    // RestoreLastLayerStack passes it because a restore is not a user edit.
    // outError, when supplied, receives a human-readable reason on failure so
    // the caller can say WHICH failure occurred — the UI's fallback text blames
    // the shader reload, which is the wrong diagnosis for a registry problem.
    bool SetLayerStack(const std::vector<std::wstring>& absoluteLayers, bool allowPersist = true,
                       std::string* outError = nullptr);

    // Read-only accessors.
    const std::vector<ModEntry>& GetMods() const { return m_mods; }
    // Full configured order, including paths unavailable in this session.
    const std::vector<std::wstring>& GetLayerStack() const { return m_layerStack; }
    // Resolved game install root(s) (argv-or-registry, primary first) — the same
    // roots mods are discovered under. The --record path resolves a ${GAME} token
    // against GameRoots().front() so a clip's mod path survives a reinstall.
    const std::vector<std::wstring>& GameRoots() const { return m_gameRoots; }
    // Primary layer = first currently available configured path (highest active
    // precedence), or empty when no configured path is available. The single-path
    // "active mod" view for the texture palette, file dialogs, and the snapshot's
    // activeModPath (the quick-switch checkmark).
    std::wstring GetPrimaryLayerPath() const { return m_primaryLayerPath; }

private:
    bool                      m_ephemeral = false;  // --drive: suppress registry writes
    IFileManager*             m_fileManager;
    Engine*                   m_engine = nullptr;
    std::vector<std::wstring> m_gameRoots;
    std::vector<ModEntry>     m_mods;
    std::vector<std::wstring> m_layerStack;       // configured, absolute slash-free
    std::wstring              m_primaryLayerPath; // first configured path that exists
};

// Registry helper for mod nicknames, exposed for direct use by the host
// bridge without going through ModManager — it's trivially side-effect-free
// and doesn't carry mutable state.
std::wstring ReadModNickname(const std::wstring& modPath);

#endif // MOD_MANAGER_H
