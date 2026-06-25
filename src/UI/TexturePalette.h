#ifndef TEXTUREPALETTE_H
#define TEXTUREPALETTE_H

// Frequently-used textures palette.
//
// PaletteStore is a process-global singleton holding the per-mod palette
// state (pinned + recent texture filenames, last-used Color/Bump filter).
// Persisted to %APPDATA%\AloParticleEditor\texture-palettes.ini, with one
// [mod=<crc32-of-path>] section per mod. The palette UI lives in
// React (WebView2).
//
// This header declares the data layer (PaletteStore) plus the bridge
// thumbnail helpers (GetThumbnail / GetTexturePreview) the React palette
// consumes. The legacy native popup was removed with the old architecture.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// Forward declarations to avoid pulling D3D + manager headers into
// every translation unit that touches the palette.
class IFileManager;
struct IDirect3DDevice9;

namespace TexturePalette {

// Which texture slot an entry has been used for. Bit-flags so a single
// entry can be flagged for both (e.g., a master texture used as the
// color slot on one emitter and the bump slot on another).
enum SlotMask : uint8_t
{
    SLOT_NONE  = 0,
    SLOT_COLOR = 1 << 0,
    SLOT_BUMP  = 1 << 1,
};

// One palette entry. Filenames are basenames as stored in
// ParticleSystem::Emitter::colorTexture / normalTexture (8-bit ANSI).
// We hold them as wstring internally so INI round-trip via
// WritePrivateProfileStringW is lossless for non-ASCII names; the
// touch/commit boundary converts to/from UTF-8 ANSI.
struct Entry
{
    std::wstring filename;
    bool         isPinned;        // true = pins row, false = recents row
    uint8_t      slotMask;        // bit-flags from SlotMask
    uint64_t     lastUsedNs;      // QueryPerformanceCounter snapshot; recents LRU sort key
};

// Capacity caps. Hard-coded for v1; raising them is a one-line change.
// MUST match THUMBS_PER_ROW * SECTION_ROWS in TexturePalette.cpp so
// every stored entry has a visible cell to occupy.
static const size_t MAX_PINS    = 12;
static const size_t MAX_RECENTS = 12;

class Store
{
public:
    static Store& Instance();

    // Mod lifecycle. Loads/saves the matching INI section. Switching
    // mods flushes the previous mod's state to disk before reading
    // the new one.
    void              SetActiveMod(const std::wstring& modPath);
    void              ClearActiveMod();   // wipes the current mod's INI section (Reset View Settings)
    const std::wstring& ActiveMod() const { return m_activeMod; }
    bool              HasActiveMod() const { return !m_activeMod.empty(); }

    // Filter (per-mod). Defaults to SLOT_COLOR.
    SlotMask          ActiveFilter() const;
    void              SetActiveFilter(SlotMask filter);

    // Mutations. Each writes to disk before returning — INI is small
    // and writes are infrequent enough that debouncing isn't worth it.
    // TouchRecent: if `filename` already exists in this mod's entries,
    // bumps its lastUsedNs and OR's `usedAs` into its slotMask. Otherwise
    // adds a new recent. If recents are full (MAX_RECENTS), the oldest
    // recent is evicted.
    void              TouchRecent(const std::wstring& filename, SlotMask usedAs);
    // TogglePin: flips pinned state. Returns false if pinning would
    // exceed MAX_PINS (caller should show "pins full" status feedback).
    bool              TogglePin(const std::wstring& filename);

    // Read access for the popup's WM_PAINT. Returns entries matching
    // the given filter, in display order:
    //   - Pins: insertion order, oldest first.
    //   - Recents: most-recently-used first.
    std::vector<Entry> Pins   (SlotMask filter) const;
    std::vector<Entry> Recents(SlotMask filter) const;

private:
    Store();
    ~Store();
    Store(const Store&)            = delete;
    Store& operator=(const Store&) = delete;

    struct ModPalette
    {
        std::vector<Entry> entries;   // pins first (insertion order), then recents (insertion order)
        SlotMask           filter;
    };

    // Loads a mod section from INI on demand; cached in m_byMod.
    ModPalette&       LoadOrInit(const std::wstring& modPath);
    void              FlushMod  (const std::wstring& modPath, const ModPalette& mp) const;

    // INI path under %APPDATA%\AloParticleEditor\.
    std::wstring      IniPath() const;
    // Section name for a mod path: "mod=<8-hex-crc32>".
    std::wstring      SectionFor(const std::wstring& modPath) const;

    std::wstring                                       m_activeMod;
    std::unordered_map<std::wstring, ModPalette>       m_byMod;        // keyed by lowercased mod path
    mutable bool                                       m_iniPathChecked;
    mutable std::wstring                               m_iniPathCache;
};

// ============================================================================
// New-UI bridge thumbnails
//
// The palette popover lives in React (WebView2), so thumbnails must
// cross the bridge as image data rather than being blitted to a Win32
// window. GetThumbnail decodes a texture (by basename) using the same D3DX
// technique as the legacy popup's DecodeThumbnail, GDI+ PNG-encodes it, and
// returns a `data:image/png;base64,...` URI on success. It also
// reports WHY there's no image — Missing (FileManager couldn't find the file)
// vs Broken (file present but empty / won't decode) — mirroring the legacy
// popup's grey-X vs magenta-X placeholders, so React can tell them apart.
// Results are cached by filename; ClearBridgeThumbCache() drops the cache on
// mod switch so same-named textures from different mods don't leak across.
enum class ThumbStatus { Ok, Missing, Broken };
struct ThumbnailResult {
    std::string dataUri;   // non-empty only when status == Ok
    ThumbStatus status;
};
ThumbnailResult GetThumbnail(const std::wstring& filename,
                             IFileManager* fileManager,
                             IDirect3DDevice9* device);
void            ClearBridgeThumbCache();

// ============================================================================
// Atlas frame picker preview
//
// GetTexturePreview decodes a texture at (close to) its native resolution
// rather than the fixed square thumbnail size, so the atlas frame picker can
// render the full sheet and overlay a frame grid. Unlike GetThumbnail it does
// NOT square the image — it preserves aspect ratio and only downscales when a
// dimension exceeds `maxBound`. The reported srcW/srcH are the texture's true
// source dimensions (pre-clamp), which the picker needs to lay out the grid.
//   status: "ok"      — dataUri holds a data:image/png;base64,... preview
//           "missing" — file couldn't be resolved (typo'd/absent path, no device)
//           "broken"  — file present but unusable (empty, won't decode, cube/volume)
struct PreviewResult {
    std::string status;   // "ok" | "missing" | "broken"
    std::string dataUri;  // valid only when status == "ok"
    int srcW = 0;
    int srcH = 0;
};
PreviewResult GetTexturePreview(const std::wstring& filename,
                                IFileManager* fileManager,
                                IDirect3DDevice9* device,
                                int maxBound = 1024,
                                bool flattenAlpha = true);

} // namespace TexturePalette

#endif
