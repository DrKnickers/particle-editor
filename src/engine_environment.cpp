// engine_environment.cpp — the ground + skydome environment cluster of the Engine class,
// moved verbatim out of engine.cpp (Phase B translation-unit split —
// tasks/2026-07-06-heavyweight-refactor-plan.md). SAME class, same header
// (engine.h); this is a file split, not a class split. Cluster-local
// file-scope statics moved with their consumers; helpers shared across
// TUs are declared in engine_internal.h with one definition.

#include "engine.h"
#include "engine_internal.h"
#include "exceptions.h"
#include "utils.h"
#include "resource.h"
#include "ResourceLimits.h"   // kMaxTextureAssetBytes (asset-read caps)
#include "AssetPathSafety.h"   // IsLocalCustomAssetPath (an-audit-finding remote-path refusal)
#include "AloModel.h"          // AloShaderParam members (ApplyAloMaterialParams)

using namespace std;

// slot 0 is Off (no resource); slots 1-8 map to bundled skydome textures.
// RCDATA entries for IDR_SKYDOME_* are added in Task 5; until then,
// FindResource for slots 1-8 returns NULL and ReloadSkydomeTexture returns false.
static const int kSkydomeBundledResources[Engine::kSkydomeBundledCount] = {
    0,                       // 0: Off
    IDR_SKYDOME_SPACE,       // 1
    IDR_SKYDOME_ATMOSPHERE,  // 2
    IDR_SKYDOME_SUNSET,      // 3
    IDR_SKYDOME_DAWN,        // 4
    IDR_SKYDOME_NIGHT,       // 5
    IDR_SKYDOME_OVERCAST,    // 6
    IDR_SKYDOME_STUDIO,      // 7
    IDR_SKYDOME_INDOOR,      // 8
};

// Parallel table of in-archive paths for slots 1-8. Routed
// through FileManager so the mod-overlay → loose-file → MEG-archive chain
// resolves them automatically (same path emitter textures take). Slot 0 has
// no game asset (Solid colour). When FileManager can't resolve a path (no
// base game installed, the active mod is missing the file), ReloadSkydomeTexture
// falls back to the procedural RCDATA at kSkydomeBundledResources[slot] so
// the slot still renders something useful.
static const char* const kSkydomeBundledGamePaths[Engine::kSkydomeBundledCount] = {
    NULL,                                              // 0: Solid colour
    "DATA\\ART\\TEXTURES\\W_SKYSTORM01.DDS",           // 1: Storm
    "DATA\\ART\\TEXTURES\\W_SKY_MURK_CLOUDS.DDS",      // 2: Murky Clouds
    "DATA\\ART\\TEXTURES\\W_SKY_SMOG_CLOUDS.DDS",      // 3: Smog Clouds
    "DATA\\ART\\TEXTURES\\W_SKYBLUE_HORIZON.DDS",      // 4: Blue Horizon
    "DATA\\ART\\TEXTURES\\W_SKYBLUE01.DDS",            // 5: Blue Sky
    "DATA\\ART\\TEXTURES\\W_SKYORANGE_HORIZON.DDS",    // 6: Orange Horizon
    "DATA\\ART\\TEXTURES\\W_SKYORANGE00.DDS",          // 7: Orange Sky
    "DATA\\ART\\TEXTURES\\W_SKYSTORM_VOLCANIC00.DDS",  // 8: Volcanic Storm
};

// Public getters so main.cpp can build thumbnails without duplicating the tables.
const int* Engine::GetSkydomeBundledResources()
{
    return kSkydomeBundledResources;
}

const char* const* Engine::GetSkydomeBundledGamePaths()
{
    return kSkydomeBundledGamePaths;
}

// Helper: try to load a texture from a file resolved via FileManager. Returns
// the texture (caller owns one ref) on success, NULL on miss. Used by both
// the curated slot path and the custom slot path.
static IDirect3DTexture9* LoadTextureViaFileManager(IDirect3DDevice9* pDevice,
                                                      IFileManager& fileManager,
                                                      const std::string& path)
{
    IFile* file = fileManager.getFile(path);
    if (file == NULL) return NULL;
    // ReadAndRelease handles the exact-byte read
    // (pre-fix this ignored the read's return value) and Releases the
    // file reference. On empty or short read it throws ReadException,
    // which we map to NULL for caller compatibility.
    std::vector<unsigned char> bytes;
    try
    {
        bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes);
    }
    catch (ReadException&)
    {
        return NULL;
    }
    IDirect3DTexture9* pTex = NULL;
    HRESULT hr = D3DXCreateTextureFromFileInMemory(pDevice, bytes.data(), (unsigned long)bytes.size(), &pTex);
    return SUCCEEDED(hr) ? pTex : NULL;
}

// Render-golden captures need one source whose pixels cannot vary with the
// installed game or active mod. This intentionally loads bundled slot 1
// straight from the executable's RCDATA, bypassing FileManager.
static bool LoadEmbeddedSkydomeSlotOne(IDirect3DDevice9* pDevice,
                                       IDirect3DTexture9** ppTexture)
{
    HMODULE hMod = GetModuleHandle(NULL);
    HRSRC hRes = FindResource(
        hMod,
        MAKEINTRESOURCE(kSkydomeBundledResources[1]),
        RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hMod, hRes);
    DWORD dwSize = SizeofResource(hMod, hRes);
    void* pData = hData ? LockResource(hData) : NULL;
    if (!pData || !dwSize) return false;
    const HRESULT hr = D3DXCreateTextureFromFileInMemory(
        pDevice, pData, dwSize, ppTexture);
    return SUCCEEDED(hr) && *ppTexture != NULL;
}


// Ground-texture bundled-resource lookup table. 0 = "no bundled
// resource". Kept in this .cpp (rather than the header) so the .rc IDs
// don't need to be visible to every includer of engine.h.
//
// Index 0 (dirt) is the editor's OWN default, bundled as RCDATA and
// always loadable. Grass/Sand/Snow (1-3) are vanilla EaW textures —
// NOT bundled (we must not ship proprietary game assets); they resolve
// from the user's EaW/FoC install at runtime via kGroundTextureGameLeaf
// below (see ReloadGroundTexture / IsGroundSlotAvailable). Slot 4 is the
// procedural solid colour; 5..7 are user-supplied custom paths only.
static const UINT kGroundTextureResourceIds[Engine::kGroundTextureCount] = {
    IDB_GROUND,         // 0 dirt (bundled editor default)
    0,                  // 1 grass — game-sourced (W_TEMPGRND00.DDS)
    0,                  // 2 sand  — game-sourced (W_SAND00.DDS)
    0,                  // 3 snow  — game-sourced (W_SNOW_RGH.DDS)
    0,                  // 4 solid color (procedural — see m_groundSolidColor)
    0, 0, 0,            // 5..7 — empty bundled, user-supplied only
};

// Base-colour leaf names resolved from the user's game install
// (DATA\ART\TEXTURES, with a loose-by-leaf fallback for mods that stash
// them flat) — the symmetric twin of the `_bc` normal-map leaves in
// ReloadGroundNormalTexture. NULL = the slot has no game source (dirt is
// bundled; solid/custom slots resolve by colour/path instead).
static const char* const kGroundTextureGameLeaf[Engine::kGroundTextureCount] = {
    NULL,                // 0 dirt — bundled editor default
    "W_TEMPGRND00.DDS",  // 1 grass
    "W_SAND00.DDS",      // 2 sand
    "W_SNOW_RGH.DDS",    // 3 snow
    NULL, NULL, NULL, NULL,
};

// Average colour of a ground-texture image, read from a 1×1 SCRATCH copy — the
// live texture is DEFAULT-pool (unlockable under D3D9Ex), so we re-decode a CPU
// copy just to read its mean. D3DX_FILTER_TRIANGLE (NOT _BOX, which only halves
// 2×2 for mipmaps) makes every source texel contribute equally, so the single
// 1×1 texel is the true whole-image average. One-time, only on a ground change.
// (The custom-file variant re-reads the file to average it — a tolerable cost on
// a one-time user action; bundled textures reuse their in-memory bytes.)
static COLORREF ReadScratch1x1Average(IDirect3DTexture9* pTex)
{
    COLORREF avg = RGB(128, 128, 128);
    if (pTex == NULL) return avg;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(pTex->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
    {
        DWORD argb = *(DWORD*)lr.pBits;   // D3DFMT_A8R8G8B8
        avg = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        pTex->UnlockRect(0);
    }
    pTex->Release();
    return avg;
}

static COLORREF AverageColorFromMemory(IDirect3DDevice9* pDevice, const void* data, DWORD size)
{
    IDirect3DTexture9* pTex = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemoryEx(
            pDevice, data, size, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SCRATCH, D3DX_FILTER_TRIANGLE, D3DX_FILTER_NONE, 0, NULL, NULL, &pTex)))
        return RGB(128, 128, 128);
    return ReadScratch1x1Average(pTex);
}

static COLORREF AverageColorFromFile(IDirect3DDevice9* pDevice, const std::wstring& path)
{
    IDirect3DTexture9* pTex = NULL;
    if (FAILED(D3DXCreateTextureFromFileExW(
            pDevice, path.c_str(), 1, 1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SCRATCH, D3DX_FILTER_TRIANGLE, D3DX_FILTER_NONE, 0, NULL, NULL, &pTex)))
        return RGB(128, 128, 128);
    return ReadScratch1x1Average(pTex);
}

// Internal: load a texture from a custom file path. Returns true and writes
// *ppOut on success; false leaves *ppOut untouched. pAvgOut (optional) receives
// the texture's average colour.
static bool LoadGroundTextureFromFile(IDirect3DDevice9*       pDevice,
                                       const std::wstring&     path,
                                       IDirect3DTexture9**     ppOut,
                                       COLORREF*               pAvgOut = NULL)
{
    if (pDevice == NULL || path.empty() || ppOut == NULL) return false;
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileW(pDevice, path.c_str(), &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut) *pAvgOut = AverageColorFromFile(pDevice, path);
    return true;
}

// Internal: load a bundled texture from the .exe's RCDATA resource.
// Returns true and writes *ppOut on success; false leaves *ppOut
// untouched. resourceId == 0 means "no bundled default" (e.g. an
// empty user-only slot) and is treated as failure.
static bool LoadGroundTextureFromResource(IDirect3DDevice9*    pDevice,
                                           UINT                 resourceId,
                                           IDirect3DTexture9**  ppOut,
                                           COLORREF*            pAvgOut = NULL)
{
    if (pDevice == NULL || resourceId == 0 || ppOut == NULL) return false;
    HMODULE  hMod  = GetModuleHandle(NULL);
    HRSRC    hRes  = FindResource(hMod, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    HGLOBAL  hData = (hRes != NULL) ? LoadResource(hMod, hRes) : NULL;
    void*    pData = (hData != NULL) ? LockResource(hData)     : NULL;
    DWORD    dwSize = (hRes != NULL) ? SizeofResource(hMod, hRes) : 0;
    if (pData == NULL || dwSize == 0) return false;
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemory(pDevice, pData, dwSize, &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut) *pAvgOut = AverageColorFromMemory(pDevice, pData, dwSize);
    return true;
}

// Internal: load a game-sourced ground texture by leaf name from the user's
// EaW/FoC install via the FileManager (mod roots → base paths → MEG). The
// FileManager twin of LoadGroundTextureFromResource: it reads the bytes ONCE
// and both decodes the texture and averages them into *pAvgOut, so the
// game-sourced path keeps m_groundColor in parity with the bundled path (the
// average feeds the ambient-SPH lighting floor and the viewport pill backdrop).
// Tries DATA\ART\TEXTURES\<leaf> first, then bare <leaf> (mods that stash the
// texture flat). Returns false with *ppOut untouched on any miss/decode failure
// — the caller's graceful-degradation signal (no install ⇒ slot unavailable).
static bool LoadGroundTextureViaFileManager(IDirect3DDevice9*    pDevice,
                                             IFileManager&        fileManager,
                                             const char*          leaf,
                                             IDirect3DTexture9**  ppOut,
                                             COLORREF*            pAvgOut = NULL)
{
    if (pDevice == NULL || leaf == NULL || ppOut == NULL) return false;
    IFile* file = fileManager.getFile(std::string("DATA\\ART\\TEXTURES\\") + leaf);
    if (file == NULL) file = fileManager.getFile(leaf);   // mod may stash it flat
    if (file == NULL) return false;
    std::vector<unsigned char> bytes;
    try
    {
        bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes);   // exact-byte read (size-capped); Releases the file ref
    }
    catch (...)
    {
        // Graceful-degradation boundary: ANY failure (ReadException, or a
        // std::bad_alloc from sizing the byte buffer to the archive entry)
        // means "slot unavailable" — never a crash. Broader than the sibling
        // texture loaders by design, because a ground-slot resolve miss must
        // fall back to dirt, not terminate.
        return false;
    }
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemory(pDevice, bytes.data(),
                                                 (unsigned long)bytes.size(), &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut)
        *pAvgOut = AverageColorFromMemory(pDevice, bytes.data(), (DWORD)bytes.size());
    return true;
}

// build a 1×1 procedural texture filled with the given COLORREF.
// Used by the "Solid Color" slot (kGroundSolidColorSlot). One-pixel
// tile is enough because the ground is sampled with WRAP wrap-mode —
// every texel across the entire ground reads back the same colour.
static bool CreateSolidColorTexture(IDirect3DDevice9*    pDevice,
                                     COLORREF             color,
                                     IDirect3DTexture9**  ppOut)
{
    if (pDevice == NULL || ppOut == NULL) return false;
    IDirect3DTexture9* pNew = NULL;
    // D3DPOOL_MANAGED → D3DPOOL_DEFAULT, because
    // D3D9Ex rejects the managed pool. But a DEFAULT-pool texture cannot
    // be LockRect'd unless it is ALSO created D3DUSAGE_DYNAMIC — without
    // it, LockRect returns D3DERR_INVALIDCALL, CreateSolidColorTexture
    // fails, and the solid-colour ground slot silently never applies (it
    // worked under the old MANAGED pool, which is lockable). Add the
    // dynamic usage so the 1×1 fill below is legal under D3D9Ex; the
    // texture is still recreated in Engine::Reset via ReloadGroundTexture
    // (DEFAULT/dynamic resources are lost on device reset).
    if (FAILED(pDevice->CreateTexture(1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &pNew, NULL)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(pNew->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        pNew->Release();
        return false;
    }
    DWORD argb = (DWORD)(0xFFu) << 24
               | (DWORD)GetRValue(color) << 16
               | (DWORD)GetGValue(color) <<  8
               | (DWORD)GetBValue(color);
    *(DWORD*)lr.pBits = argb;
    pNew->UnlockRect(0);
    *ppOut = pNew;
    return true;
}

bool Engine::ReloadGroundTexture()
{
    if (m_pDevice == NULL) return false;   // pre-init guard

    // Solid-color slot — procedural texture from m_groundSolidColor.
    if (m_groundTextureIndex == kGroundSolidColorSlot)
    {
        IDirect3DTexture9* pNew = NULL;
        if (!CreateSolidColorTexture(m_pDevice, m_groundSolidColor, &pNew))
            return false;
        SAFE_RELEASE(m_pGroundTexture);
        m_pGroundTexture = pNew;
        m_groundColor = m_groundSolidColor;   // solid slot: the colour IS the floor
#ifndef NDEBUG
        printf("[Ground] solid-color slot=%d color=#%02X%02X%02X\n",
               m_groundTextureIndex,
               GetRValue(m_groundSolidColor),
               GetGValue(m_groundSolidColor),
               GetBValue(m_groundSolidColor));
        fflush(stdout);
#endif
        return true;
    }

    // Resolution order: custom path → game-sourced leaf (grass/sand/snow,
    // resolved from the user's install) → bundled resource (dirt only) →
    // on all-failure, fall back to slot 0 (dirt, always loadable from RCDATA).
    IDirect3DTexture9* pNew = NULL;
    const std::wstring& path = m_groundSlotCustomPaths[m_groundTextureIndex];
    if (!path.empty())
    {
        if (!LoadGroundTextureFromFile(m_pDevice, path, &pNew, &m_groundColor))
        {
#ifndef NDEBUG
            printf("[Ground] custom path failed for slot=%d; trying game/bundled\n",
                   m_groundTextureIndex);
            fflush(stdout);
#endif
        }
    }
    if (pNew == NULL)
    {
        const char* leaf = kGroundTextureGameLeaf[m_groundTextureIndex];
        if (leaf != NULL &&
            !LoadGroundTextureViaFileManager(m_pDevice, m_fileManager, leaf, &pNew, &m_groundColor))
        {
#ifndef NDEBUG
            printf("[Ground] game texture '%s' not resolved for slot=%d; falling back\n",
                   leaf, m_groundTextureIndex);
            fflush(stdout);
#endif
            // Release-visible signal ONLY for the corrupt-install case: the slot
            // probed available (file present) yet failed to decode -- distinct from
            // "no install" (expected, silent). Without this, a user who picked Snow
            // and silently got dirt has no paper trail in a Release build.
            if (IsGroundSlotAvailable(m_groundTextureIndex))
            {
                char msg[256];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[Ground] slot=%d game texture '%s' is present but failed to decode; "
                    "using dirt (file may be corrupt).\n", m_groundTextureIndex, leaf);
                OutputDebugStringA(msg);
            }
        }
    }
    if (pNew == NULL)
    {
        UINT bundledId = kGroundTextureResourceIds[m_groundTextureIndex];
        if (bundledId != 0)
            LoadGroundTextureFromResource(m_pDevice, bundledId, &pNew, &m_groundColor);
    }
    if (pNew == NULL)
    {
#ifndef NDEBUG
        printf("[Ground] slot=%d empty/failed; falling back to default\n",
               m_groundTextureIndex);
        fflush(stdout);
#endif
        if (m_groundTextureIndex != 0)
        {
            m_groundTextureIndex = 0;
            return ReloadGroundTexture();
        }
        return false;                       // dirt itself failed → engine is in trouble
    }
    // Release the prior texture only after the new one is in hand —
    // ensures we don't have a transient null window where a paint
    // could race against us.
    SAFE_RELEASE(m_pGroundTexture);
    m_pGroundTexture = pNew;
#ifndef NDEBUG
    const char* src = !m_groundSlotCustomPaths[m_groundTextureIndex].empty() ? "custom"
                    : (kGroundTextureGameLeaf[m_groundTextureIndex] != NULL)  ? "game"
                    :                                                           "bundled";
    printf("[Ground] texture set slot=%d source=%s\n", m_groundTextureIndex, src);
    fflush(stdout);
#endif
    return true;
}

bool Engine::SetGroundTexture(int index)
{
    if (DeviceCallsBlocked()) return false;
    if (index < 0 || index >= kGroundTextureCount) return false;
    // Refuse selection of an unavailable slot — empty user slot, OR a
    // game-sourced slot (grass/sand/snow) the install can't resolve. UI
    // greys these out; this is defence in depth against a stale persisted
    // selection or a programmatic call. (Availability, not emptiness: a
    // game slot reads "empty" structurally now that it has no bundled
    // resource, yet is selectable whenever the install provides it.)
    if (!IsGroundSlotAvailable(index)) return false;
    // Fast-path: already at this slot AND we have a valid texture.
    if (index == m_groundTextureIndex && m_pGroundTexture != NULL) return true;
    m_groundTextureIndex = index;
    bool ok = ReloadGroundTexture();
    ReloadGroundNormalTexture();   // re-resolve the slot's companion _bc map
    // ReloadGroundTexture returns true when the dirt fallback loaded. That is
    // healthy renderer state, but it is not success for the requested slot.
    // Preserve both facts: GetGroundTexture() reports the reached slot and this
    // bool reports whether the request itself applied.
    return ok && m_groundTextureIndex == index;
}

bool Engine::SetGroundSlotCustomPath(int slot, const std::wstring& path)
{
    if (DeviceCallsBlocked()) return false;
    if (slot < 0 || slot >= kGroundTextureCount) return false;
    // Refuse a path that points at another machine. Enforced HERE rather than in
    // the bridge handler because the registry restore at startup calls this
    // setter too — guarding only the handler would leave a stored UNC path
    // replaying on every launch, which is the durable half of the finding
    // (2026-07 audit, an-audit-finding).
    if (!IsLocalCustomAssetPath(path)) return false;
    m_groundSlotCustomPaths[slot] = path;
    // If the mutated slot is currently selected, reload the engine's
    // ground texture so the preview reflects the change immediately.
    if (slot == m_groundTextureIndex)
    {
        // If the slot can no longer render (cleared user-supplied path on a
        // higher slot, or a game slot the install can't resolve), bounce the
        // selection back to dirt rather than leaving the engine pointing at
        // nothing. Availability-aware: clearing a custom path off a
        // game-sourced slot reverts to its install texture, not dirt.
        if (!IsGroundSlotAvailable(slot))
        {
            m_groundTextureIndex = 0;
        }
        bool ok = ReloadGroundTexture();
        ReloadGroundNormalTexture();   // re-resolve the slot's companion _bc map
        return ok;
    }
    return true;
}

const std::wstring& Engine::GetGroundSlotCustomPath(int slot) const
{
    static const std::wstring empty;
    if (slot < 0 || slot >= kGroundTextureCount) return empty;
    return m_groundSlotCustomPaths[slot];
}

bool Engine::IsGroundSlotEmpty(int slot) const
{
    if (slot < 0 || slot >= kGroundTextureCount) return true;
    if (slot == kGroundSolidColorSlot) return false;   // always populated procedurally
    if (!m_groundSlotCustomPaths[slot].empty()) return false;
    return kGroundTextureResourceIds[slot] == 0;
}

bool Engine::IsGroundSlotAvailable(int slot) const
{
    if (slot < 0 || slot >= kGroundTextureCount)  return false;
    if (slot == kGroundSolidColorSlot)            return true;    // procedural — always
    if (!m_groundSlotCustomPaths[slot].empty())   return true;    // user path (optimistic, as today)
    if (kGroundTextureResourceIds[slot] != 0)     return true;    // bundled (dirt)
    if (kGroundTextureGameLeaf[slot] != NULL)                     // game-sourced (grass/sand/snow)
    {
        EnsureGroundSlotResolvable();
        return m_groundSlotResolvable[slot];
    }
    return false;                                                 // empty user slot
}

void Engine::EnsureGroundSlotResolvable() const
{
    // Recompute only when the mod-layer set changed (see header note on why
    // GetContentRoots() is a complete invalidation key). Cheap getFile()+Release()
    // probe per game leaf — no decode.
    const std::vector<std::wstring>& roots = m_fileManager.GetContentRoots();
    if (m_groundSlotResolvableValid && roots == m_groundSlotResolvableRoots)
        return;
    for (int i = 0; i < kGroundTextureCount; ++i)
    {
        const char* leaf = kGroundTextureGameLeaf[i];
        bool resolvable = false;
        if (leaf != NULL)
        {
            // FileManager::getFile swallows its own IOExceptions and returns
            // NULL today, so this can't throw — but this probe runs inside a
            // const query reached from the snapshot builder, so a defensive
            // catch keeps a future getFile change from crashing the snapshot
            // path: the worst case is a slot reported (un)resolvable, never a
            // crash.
            try
            {
                IFile* f = m_fileManager.getFile(std::string("DATA\\ART\\TEXTURES\\") + leaf);
                if (f == NULL) f = m_fileManager.getFile(leaf);   // mod may stash it flat
                if (f != NULL) { f->Release(); resolvable = true; }
            }
            catch (...)
            {
                resolvable = false;
            }
        }
        m_groundSlotResolvable[i] = resolvable;
    }
    m_groundSlotResolvableRoots = roots;
    m_groundSlotResolvableValid = true;
#ifndef NDEBUG
    printf("[Ground] resolvable probe: grass=%d sand=%d snow=%d (mod-roots=%u)\n",
           m_groundSlotResolvable[1], m_groundSlotResolvable[2], m_groundSlotResolvable[3],
           (unsigned)roots.size());
    fflush(stdout);
#endif
}

bool Engine::SetGroundSolidColor(COLORREF color)
{
    if (DeviceCallsBlocked()) return false;
    m_groundSolidColor = color;
    // If the solid-colour slot is currently selected, regenerate the
    // texture so the colour change shows immediately.
    if (m_groundTextureIndex == kGroundSolidColorSlot)
        return ReloadGroundTexture();
    return true;
}

// Build the UV sphere vertex declaration + mesh used by the skydome
// render pass. Called once from the Engine constructor after m_pDevice is
// created. The VB/IB allocation moved into
// CreateSkydomeMeshBuffers() so Engine::Reset can recreate them after
// the device Reset (D3DPOOL_DEFAULT resources don't survive Reset).
void Engine::InitSkydomeMesh()
{
    // Vertex declaration — not pool-bound, survives device Reset.
    D3DVERTEXELEMENT9 decl[] = {
        {0, offsetof(SkydomeVertex, Position),  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, offsetof(SkydomeVertex, Normal),    D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, offsetof(SkydomeVertex, TexCoord),  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    if (FAILED(m_pDevice->CreateVertexDeclaration(decl, &m_pSkydomeDecl)))
        throw runtime_error("Unable to create skydome mesh");

    // VB + IB — pool-bound, must be recreated on device Reset.
    CreateSkydomeMeshBuffers();
}

// Allocate + fill the skydome VB and IB.
// Called from InitSkydomeMesh (engine init) and from Engine::Reset (after
// device Reset succeeds). D3DPOOL_DEFAULT means the buffers live in
// driver-managed VRAM that's lost on Reset; the procedural sphere data
// is cheap to regenerate (~256 vertices, ~1024 indices), so we just
// re-emit it every time rather than caching.
void Engine::CreateSkydomeMeshBuffers()
{
    const int lon = kSkydomeLongSegments;
    const int lat = kSkydomeLatSegments;
    const int vertCount = (lon + 1) * (lat + 1);
    const int triCount  = lon * lat * 2;
    m_skydomeIndexCount = triCount * 3;

    // Generate vertices: U wraps lon segments [0,1], V is lat segments [0,1].
    // Sphere radius is 1; the shader will push depth to the far plane.
    //
    // Axis convention: the engine is Z-up (m_eye.Up = (0,0,1)), so the
    // sphere's poles are placed on ±Z — top pole at +Z, bottom pole at
    // -Z, horizon ring on the XY plane. This matches how the game
    // renders its skydomes and means an equirectangular texture's top
    // edge (V=0) faces up and its bottom edge (V=1) faces down.
    std::vector<SkydomeVertex> verts(vertCount);
    for (int j = 0; j <= lat; ++j)
    {
        const float v     = float(j) / float(lat);
        const float theta = v * D3DX_PI;             // 0..pi (pole to pole)
        const float sinTheta = sinf(theta);
        const float cosTheta = cosf(theta);
        for (int i = 0; i <= lon; ++i)
        {
            const float u   = float(i) / float(lon);
            const float phi = u * 2.0f * D3DX_PI;   // 0..2pi
            const float sinPhi = sinf(phi);
            const float cosPhi = cosf(phi);
            SkydomeVertex& vx = verts[j * (lon + 1) + i];
            vx.Position = D3DXVECTOR3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
            vx.Normal   = vx.Position;
            vx.TexCoord = D3DXVECTOR2(u, v);
        }
    }

    std::vector<uint16_t> idx(m_skydomeIndexCount);
    int k = 0;
    for (int j = 0; j < lat; ++j)
    {
        for (int i = 0; i < lon; ++i)
        {
            uint16_t a = uint16_t(j * (lon + 1) + i);
            uint16_t b = a + 1;
            uint16_t c = uint16_t((j + 1) * (lon + 1) + i);
            uint16_t d = c + 1;
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }
    }

    // VB — D3DPOOL_DEFAULT for D3D9Ex compatibility.
    if (FAILED(m_pDevice->CreateVertexBuffer(
        UINT(verts.size() * sizeof(SkydomeVertex)),
        D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_pSkydomeVB, NULL)))
        throw runtime_error("Unable to create skydome mesh");
    void* pVB = NULL;
    if (FAILED(m_pSkydomeVB->Lock(0, 0, &pVB, 0)))
        throw runtime_error("Unable to create skydome mesh");
    memcpy(pVB, verts.data(), verts.size() * sizeof(SkydomeVertex));
    m_pSkydomeVB->Unlock();

    // IB — D3DPOOL_DEFAULT for D3D9Ex compatibility.
    if (FAILED(m_pDevice->CreateIndexBuffer(
        UINT(idx.size() * sizeof(uint16_t)),
        D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_pSkydomeIB, NULL)))
        throw runtime_error("Unable to create skydome mesh");
    void* pIB = NULL;
    if (FAILED(m_pSkydomeIB->Lock(0, 0, &pIB, 0)))
        throw runtime_error("Unable to create skydome mesh");
    memcpy(pIB, idx.data(), idx.size() * sizeof(uint16_t));
    m_pSkydomeIB->Unlock();

#ifndef NDEBUG
    fprintf(stdout, "[Skydome] sphere mesh init verts=%d tris=%d\n", vertCount, triCount);
#endif
}

// Release the skydome VB + IB ahead of
// m_pDevice->Reset. Counterpart of CreateSkydomeMeshBuffers. Symmetric
// with the existing OnLostDevice pattern used for shaders + compositor RT.
void Engine::ReleaseSkydomeMeshBuffers()
{
    SAFE_RELEASE(m_pSkydomeVB);
    SAFE_RELEASE(m_pSkydomeIB);
}

void Engine::InitSkydomeEffect()
{
    HMODULE hMod  = GetModuleHandle(NULL);
    HRSRC   hRes  = FindResource(hMod, MAKEINTRESOURCE(IDR_SHADER_SKYDOME), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hData  = LoadResource(hMod, hRes);
    DWORD   dwSize = SizeofResource(hMod, hRes);
    void*   pData  = hData ? LockResource(hData) : NULL;
    if (!pData || !dwSize) return;

    LPD3DXBUFFER pErrors = NULL;
    HRESULT hr = D3DXCreateEffect(m_pDevice, pData, dwSize, NULL, NULL, 0, NULL,
                                  &m_pSkydomeEffect, &pErrors);
    if (FAILED(hr))
    {
#ifndef NDEBUG
        if (pErrors) fprintf(stderr, "[Skydome] effect compile failed: %s\n",
                             (const char*)pErrors->GetBufferPointer());
#endif
        SAFE_RELEASE(pErrors);
        m_pSkydomeEffect = NULL;
        return;
    }
    SAFE_RELEASE(pErrors);

    m_hSkydomeWVP = m_pSkydomeEffect->GetParameterByName(NULL, "g_WorldViewProj");
    m_hSkydomeTex = m_pSkydomeEffect->GetParameterByName(NULL, "g_Skydome");
}

bool Engine::ReloadSkydomeTexture(int slot)
{
    SAFE_RELEASE(m_pSkydomeTexture);
    if (m_skydomeUsesEmbeddedResource)
    {
        if (slot == 1 &&
            LoadEmbeddedSkydomeSlotOne(m_pDevice, &m_pSkydomeTexture))
        {
            return true;
        }
        // A failed oracle reload must not leave source identity claiming that
        // an embedded texture is live. Full Reset reaches this path too.
        m_skydomeIndex = kSkydomeOffSlot;
        m_skydomeUsesEmbeddedResource = false;
        SAFE_RELEASE(m_pSkydomeTexture);
        return false;
    }
    if (slot == kSkydomeOffSlot) return true;

    if (slot > kSkydomeOffSlot && slot < kSkydomeBundledCount)
    {
        // Try the curated in-archive path first so the
        // skydome picks up real game textures (and mod overlays on top of
        // them) wherever they exist. Fall back to the bundled RCDATA
        // placeholder so the slot still renders something when the base
        // game / mod doesn't ship the file.
        const char* gamePath = kSkydomeBundledGamePaths[slot];
        if (gamePath != NULL)
        {
            m_pSkydomeTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, gamePath);
            if (m_pSkydomeTexture != NULL) return true;
        }
        HMODULE hMod   = GetModuleHandle(NULL);
        HRSRC   hRes   = FindResource(hMod, MAKEINTRESOURCE(kSkydomeBundledResources[slot]), RT_RCDATA);
        if (!hRes) return false;
        HGLOBAL hData  = LoadResource(hMod, hRes);
        DWORD   dwSize = SizeofResource(hMod, hRes);
        void*   pData  = hData ? LockResource(hData) : NULL;
        if (!pData || !dwSize) return false;
        return SUCCEEDED(D3DXCreateTextureFromFileInMemory(m_pDevice, pData, dwSize, &m_pSkydomeTexture));
    }

    if (slot >= kSkydomeFirstCustomSlot && slot < kSkydomeSlotCount)
    {
        const std::wstring& path = m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot];
        if (path.empty()) return false;
        // Custom slots now route through FileManager first, so a path like
        // "DATA\\ART\\TEXTURES\\foo.dds" resolves from the mod / base-game
        // MEGs the same way the curated slots do. If FileManager can't
        // resolve it (e.g. the user pasted an absolute path to a loose file
        // outside the game roots), fall back to direct file I/O so legacy
        // absolute-path custom slots keep working.
        std::string narrowPath = WideToAnsi(path);
        m_pSkydomeTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, narrowPath);
        if (m_pSkydomeTexture != NULL) return true;
        // D3DPOOL_MANAGED → D3DPOOL_DEFAULT.
        // D3D9Ex disallows the managed pool. Custom-slot textures are
        // re-loaded from disk via Engine::Reset → ReloadSkydomeTexture
        // (called with m_skydomeIndex) when the device is reset.
        return SUCCEEDED(D3DXCreateTextureFromFileEx(
            m_pDevice, path.c_str(),
            D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN,
            D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL,
            &m_pSkydomeTexture));
    }
    return false;
}

void Engine::RenderSkydome()
{
    // World = Translation(camera.Position) — keeps the sphere camera-locked.
    D3DXMATRIX world, wvp;
    D3DXMatrixTranslation(&world, m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);
    wvp = world * m_view * m_projection;

    // Save render state so the skydome pass doesn't pollute the rest of the frame.
    DWORD oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,      &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,     &oldCull);
    // Save the vertex declaration too. It is NOT part of the ID3DXEffect
    // state block (Begin/End won't restore it), so the skydome's declaration
    // (SkydomeVertex — position/normal/texcoord, NO diffuse-colour element)
    // would otherwise leak into the ground + particle draws that follow. With
    // no colour stream, the fixed-function pipeline defaults every vertex's
    // diffuse to white (0xFFFFFFFF) — which blows out additive particles to
    // white and breaks the alpha-blended ones. The ground is unaffected (its
    // vertices are already white), which is exactly why the bug looked like a
    // skydome-only blend issue.
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,      D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,     D3DCULL_CCW); // we're inside the sphere; Y↔Z swap in InitSkydomeMesh reversed handedness so the inside-facing triangles are now CCW

    m_pSkydomeEffect->SetMatrix (m_hSkydomeWVP, &wvp);
    m_pSkydomeEffect->SetTexture(m_hSkydomeTex, m_pSkydomeTexture);

    UINT passes = 0;
    m_pSkydomeEffect->Begin(&passes, 0);
    for (UINT i = 0; i < passes; ++i)
    {
        m_pSkydomeEffect->BeginPass(i);
        m_pDevice->SetVertexDeclaration(m_pSkydomeDecl);
        m_pDevice->SetStreamSource(0, m_pSkydomeVB, 0, sizeof(SkydomeVertex));
        m_pDevice->SetIndices(m_pSkydomeIB);
        const UINT vertCount = (kSkydomeLongSegments + 1) * (kSkydomeLatSegments + 1);
        m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                        vertCount,
                                        0,
                                        m_skydomeIndexCount / 3);
        m_pSkydomeEffect->EndPass();
    }
    m_pSkydomeEffect->End();

    // Restore the vertex declaration the skydome bound, so the ground +
    // particle draws use the engine's diffuse-colour-carrying declaration
    // again (see the save above). GetVertexDeclaration AddRef'd it.
    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();

    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,      oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,     oldCull);
}

// The blend mode a dome sub-mesh's shader expects. The game .fxo set this
// inside a no-op'd SB block (ALAMO_STATE_BLOCKS 0), so the app
// applies it. MeshAdditive* -> ONE/ONE (space starfields); MeshAlpha* ->
// SRCALPHA/INVSRCALPHA (land ring/horizon overlays); everything else (Skydome,
// MeshGloss base) -> opaque.
enum SkydomeBlend { SKYBLEND_OPAQUE, SKYBLEND_ADDITIVE, SKYBLEND_ALPHA };

static SkydomeBlend SkydomeBlendFor(const std::string& shaderName)
{
    if (_strnicmp(shaderName.c_str(), "MeshAdditive", 12) == 0) return SKYBLEND_ADDITIVE;
    if (_strnicmp(shaderName.c_str(), "MeshAlpha",     9) == 0) return SKYBLEND_ALPHA;
    return SKYBLEND_OPAQUE;
}

// Apply a sub-mesh's authored material params (index-parallel handles) to the
// effect: each param -> its sampler/uniform via the .fx Texture=(X) link;
// params absent from this shader (NULL handle) are skipped. This loop was
// byte-identical in RenderSkydomeMesh and RenderReferenceObject (DRY audit
// cpp-engine-0) — extracted so the two stay in lockstep. ONLY the param loop is
// shared; the surrounding uniform-bind blocks legitimately diverge (the
// ref-object path binds object-space eye/light + a skinned bone palette) and
// stay inline at each call site.
// Non-static: declared in engine_internal.h (also called by the
// reference-object path in engine_reference.cpp).
void ApplyAloMaterialParams(ID3DXEffect* fx,
                                   const std::vector<AloShaderParam>& params,
                                   const std::vector<D3DXHANDLE>& matHandles,
                                   const std::vector<IDirect3DTexture9*>& matTextures)
{
    for (size_t i = 0; i < params.size(); ++i)
    {
        D3DXHANDLE ph = (i < matHandles.size()) ? matHandles[i] : NULL;
        if (ph == NULL) continue;
        const AloShaderParam& p = params[i];
        switch (p.kind)
        {
            case AloShaderParam::INT:    fx->SetInt(ph, p.i); break;
            case AloShaderParam::FLOAT:  fx->SetFloat(ph, p.f[0]); break;
            case AloShaderParam::FLOAT3: fx->SetFloatArray(ph, p.f, 3); break;
            case AloShaderParam::FLOAT4:
            {
                D3DXVECTOR4 v(p.f[0], p.f[1], p.f[2], p.f[3]);
                fx->SetVector(ph, &v);
                break;
            }
            case AloShaderParam::TEXTURE:
                if (i < matTextures.size()) fx->SetTexture(ph, matTextures[i]);
                break;
        }
    }
}

// World matrix for a BILLBOARD sub-mesh (a 0x206 bone, e.g. the star dome's sun
// glow). The quad's bone places it off-centre; the game orients it to FACE THE
// CAMERA so a flat additive quad is never seen edge-on. We reproduce that: keep
// the bone's object-space position (through the dome world), but replace its
// orientation with a screen-facing basis so the quad's authored in-plane extent
// (local X/Z) maps to camera right/up. Without this the quad draws flat and
// collapses to a 1px additive-white line edge-on (the false "closure seam").
static D3DXMATRIX MakeSkydomeBillboardWorld(const D3DXVECTOR3& eyePos,
                                            const D3DXVECTOR3& eyeUp,
                                            const D3DXMATRIX&  placement,
                                            const D3DXMATRIX&  domeWorld)
{
    // Bone object-space position -> world (through the dome's scale+translate).
    D3DXVECTOR3 cObj(placement._41, placement._42, placement._43);
    D3DXVECTOR3 center; D3DXVec3TransformCoord(&center, &cObj, &domeWorld);

    // Uniform dome scale = length of domeWorld's first basis row (sf,sf,sf).
    const D3DXVECTOR3 row0(domeWorld._11, domeWorld._12, domeWorld._13);
    const float sf = D3DXVec3Length(&row0);

    // Spherical billboard basis facing the eye (point-at-eye, not view-plane: the
    // sun is a single off-centre point on a camera-locked sphere). The quad lies in
    // its bone-local X/Z plane (local Y is the normal), so map X->right, Z->up,
    // Y->normal. NOTE: deliberately NOT the engine's view-aligned m_billboard
    // (inverse view-rotation) -- that screen-aligns a center-anchored quad; this
    // off-centre point needs to point AT the eye.
    D3DXVECTOR3 normal = eyePos - center;
    if (D3DXVec3Length(&normal) < 1e-4f) normal = D3DXVECTOR3(0, 1, 0);   // sun at the eye (bone @ origin)
    D3DXVec3Normalize(&normal, &normal);
    // right = up x normal. If eyeUp is (near-)parallel to normal (camera ~directly
    // over/under the sun) OR degenerate (zero), the cross collapses -> fall back to
    // a world axis guaranteed non-parallel to normal. Detect via the cross MAGNITUDE
    // (|a x b| = sin θ), which catches both cases a dot-threshold up-guard would miss.
    D3DXVECTOR3 right;
    D3DXVec3Cross(&right, &eyeUp, &normal);
    if (D3DXVec3Length(&right) < 1e-4f) D3DXVec3Cross(&right, &D3DXVECTOR3(1, 0, 0), &normal);
    if (D3DXVec3Length(&right) < 1e-4f) D3DXVec3Cross(&right, &D3DXVECTOR3(0, 1, 0), &normal);
    D3DXVec3Normalize(&right, &right);
    D3DXVECTOR3 up; D3DXVec3Cross(&up, &normal, &right); D3DXVec3Normalize(&up, &up);

    D3DXMATRIX w; D3DXMatrixIdentity(&w);
    w._11 = right.x  * sf; w._12 = right.y  * sf; w._13 = right.z  * sf;   // local X -> right
    w._21 = normal.x * sf; w._22 = normal.y * sf; w._23 = normal.z * sf;   // local Y -> normal (verts Y==0)
    w._31 = up.x     * sf; w._32 = up.y     * sf; w._33 = up.z     * sf;   // local Z -> up
    w._41 = center.x;      w._42 = center.y;      w._43 = center.z;        // sun world position
    return w;
}

// Draw one decoded .alo dome: each sub-mesh runs its OWN named game
// shader 1:1 (Skydome.fx / MeshGloss.fxo / MeshAdditive.fx). Mirrors the
// particle per-frame binding template (engine.cpp:746) but binds the REAL world
// matrix (Skydome.fx computes world_pos/world_normal for SH) instead of the
// identity the particle path uses. Saves + restores the full render-state delta
// any sub-mesh may touch so the dome can't leak blend/zwrite/cull/decl into the
// ground + particle draws that follow.
void Engine::RenderSkydomeMesh(SkydomeMesh& mesh, const D3DXMATRIX& world)
{
    // World/WVP are now per-sub-mesh (each carries its bone placement; a billboard
    // sub-mesh gets a camera-facing world), so they are computed inside the loop.
    DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend, oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    m_pDevice->GetRenderState(D3DRS_SRCBLEND,         &oldSrcBlend);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,        &oldDestBlend);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    // Draw in two phases matching the game's Opaque-then-Transparent order: opaque
    // sub-meshes first (fill the background), then additive/alpha layers blended on
    // top -- so layering is correct regardless of the .alo's sub-mesh order.
    for (int phase = 0; phase < 2; ++phase)
    {
      const bool opaquePhase = (phase == 0);
      for (SubMeshGpu& sub : mesh.SubMeshes())
      {
        if (sub.effect == NULL || sub.vb == NULL || sub.ib == NULL || sub.decl == NULL)
            continue;
        const SkydomeBlend blend = SkydomeBlendFor(sub.shaderName);
        if ((blend == SKYBLEND_OPAQUE) != opaquePhase)
            continue;   // opaque sub-meshes in phase 0, blended in phase 1

        // Render state the shader expects (the game .fxo may NOT set it itself --
        // ALAMO_STATE_BLOCKS -- so we set it regardless). Every dome
        // layer is a camera-centred background: depth test+write OFF (drawn first,
        // ground/particles paint over it) and CULL_NONE (one triangle per view ray
        // -> no overdraw, sidesteps the unknown .alo winding). Blend per shader
        // intent: additive (MeshAdditive) adds, alpha (MeshAlpha) src-over, else opaque.
        m_pDevice->SetRenderState(D3DRS_ZENABLE,      D3DZB_FALSE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_pDevice->SetRenderState(D3DRS_CULLMODE,     D3DCULL_NONE);
        switch (blend)
        {
            case SKYBLEND_ADDITIVE:
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
                break;
            case SKYBLEND_ALPHA:
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
                break;
            default: // SKYBLEND_OPAQUE (Skydome, MeshGloss base)
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                break;
        }

        ID3DXEffect* fx = sub.effect->getD3DEffect();   // AddRef'd
        const Effect::Handles& h = sub.effect->getHandles();

        // Per-sub-mesh world. A billboard bone (the star dome's sun glow) is placed
        // at its bone position and RE-ORIENTED to face the camera, so its flat
        // additive quad never collapses to an edge-on 1px line. Every other sub-mesh
        // keeps the dome world verbatim -- byte-identical to the prior behaviour, so
        // no existing/mod dome render changes (the dome meshes are authored in object
        // space on near-identity bones; only the sun rides a 0x206 billboard bone).
        D3DXMATRIX subWorld = (sub.billboardMode != 0)
            ? MakeSkydomeBillboardWorld(m_eye.Position, m_eye.Up, sub.placement, world)
            : world;
        D3DXMATRIX subWvp = subWorld * m_view * m_projection;

        // Engine semantics: the verbatim particle binding template (engine.cpp:746)
        // but with the REAL world matrix -- the dome shaders compute world_pos /
        // world_normal for SH diffuse + (MeshGloss) specular -- not the identity the
        // particle path uses. Handles a shader doesn't declare are NULL -> no-op.
        D3DXVECTOR4 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z, 1.0f);
        fx->SetMatrix(h.hWorld,               &subWorld);
        fx->SetMatrix(h.hWorldViewProjection, &subWvp);
        fx->SetVector(h.hEyePosition,         &eyePos);
        fx->SetVector(h.hGlobalAmbient,       &m_ambient);
        fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
        fx->SetVector(h.hDirLightObjVec0,     &m_lights[0].Position);
        fx->SetVector(h.hDirLightDiffuse,     &m_lights[0].Diffuse);
        fx->SetVector(h.hDirLightSpecular,    &m_lights[0].Specular);
        fx->SetMatrixArray(h.hSphLightAll,    m_sphLightAll,  3);
        fx->SetMatrixArray(h.hSphLightFill,   m_sphLightFill, 3);
        fx->SetFloat(h.hTime,                 GetTimeF());

        // Authored material params (index-parallel handles) -> samplers via the
        // .fx Texture=(X) link. Skips params absent from this shader (NULL handle).
        ApplyAloMaterialParams(fx, sub.params, sub.matHandles, sub.matTextures);

        m_pDevice->SetVertexDeclaration(sub.decl);
        m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
        m_pDevice->SetIndices(sub.ib);

        UINT passes = 0;
        fx->Begin(&passes, 0);
        for (UINT pass = 0; pass < passes; ++pass)
        {
            fx->BeginPass(pass);
#ifndef NDEBUG
            {
                DWORD zw = 0, ab = 0, cm = 0;
                m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &zw);
                m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
                m_pDevice->GetRenderState(D3DRS_CULLMODE,         &cm);
                fprintf(stderr, "[SkyDraw] %s pass %u/%u zwrite=%lu ablend=%lu cull=%lu prims=%u\n",
                        sub.shaderName.c_str(), pass + 1, passes, zw, ab, cm, sub.primitiveCount);
            }
#endif
            m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                            sub.vertexCount, 0, sub.primitiveCount);
            fx->EndPass();
        }
        fx->End();
        fx->Release();
      }
    }

    // Restore the saved delta. Stream-source / indices are intentionally NOT
    // restored: every subsequent draw rebinds them.
    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,         oldSrcBlend);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,        oldDestBlend);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Compose entry, replacing the single RenderSkydome() call site. Draws
// the game domes (secondary behind, then primary) when a real dome is selected;
// otherwise falls back to the simple-background sphere (bundled / custom / solid).
void Engine::RenderSkydomes()
{
    const bool primaryReady   = !m_skydomePrimaryMesh.IsEmpty()   && m_skydomePrimaryMesh.HasResolved();
    const bool secondaryReady = !m_skydomeSecondaryMesh.IsEmpty() && m_skydomeSecondaryMesh.HasResolved();

    if (primaryReady || secondaryReady)
    {
        // Layering: depth is off, so the layer drawn LATER paints on top of the
        // earlier. In BOTH contexts the PRIMARY is the opaque/full-dome background
        // and is drawn FIRST; the SECONDARY composites ON TOP:
        //  Space: primary = opaque star sphere (the game's Sort_Order_Adjust=-1 back
        //         layer); secondary = ADDITIVE nebula (MeshAdditiveVColor, ONE/ONE) --
        //         it MUST add after the opaque base or the base overwrites it (the bug
        //         this fixes: secondary-first buried the nebula under the star sphere).
        //  Land:  primary = full sky-dome; secondary = partial overlay (rings/horizon).
        const bool primaryFirst = true;
        SkydomeMesh* order[2] = {
            primaryFirst ? &m_skydomePrimaryMesh   : &m_skydomeSecondaryMesh,
            primaryFirst ? &m_skydomeSecondaryMesh : &m_skydomePrimaryMesh
        };
        const bool ready[2] = {
            primaryFirst ? primaryReady   : secondaryReady,
            primaryFirst ? secondaryReady : primaryReady
        };

        D3DXMATRIX t;
        D3DXMatrixTranslation(&t, m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);
        for (int i = 0; i < 2; ++i)
        {
            if (!ready[i]) continue;
            const float sf = order[i]->ScaleFactor();
            D3DXMATRIX s, world;
            D3DXMatrixScaling(&s, sf, sf, sf);
            world = s * t;
            RenderSkydomeMesh(*order[i], world);
        }
        return;
    }

    if (m_skydomeIndex != kSkydomeOffSlot && m_pSkydomeTexture != NULL && m_pSkydomeEffect != NULL)
        RenderSkydome();
}

// Re-drive Load->Resolve->CreateBuffers for both slots from the current
// selected Names. Used on selection change and mod-switch (ReloadTextures, after
// ShaderManager::Clear ran, so getShader picks up the new mod's .fxo). Resolve +
// CreateBuffers no-op until the device is valid; Load (CPU) runs regardless.
void Engine::RebuildSkydomeMeshes()
{
    MapEnvironment env;
    ResolveMapEnvironment(EnsureSkydomeLists(), m_skydomeContext,
                          m_skydomePrimaryName, m_skydomeSecondaryName, env);

    SkydomeMesh* meshes[2]    = { &m_skydomePrimaryMesh, &m_skydomeSecondaryMesh };
    const SkydomeRef* refs[2]  = { &env.primary, &env.secondary };
    const bool has[2]         = { env.hasPrimary, env.hasSecondary };
    // The chosen Name decides None-vs-LoadFailed: an empty Name is simply
    // "no dome", a non-empty Name that doesn't resolve/load is a failure the
    // picker must surface (it otherwise silently falls back to solid colour).
    const std::string* names[2] = { &m_skydomePrimaryName, &m_skydomeSecondaryName };
    SkydomeSlotStatus* status[2] = { &m_skydomePrimaryStatus, &m_skydomeSecondaryStatus };

    for (int i = 0; i < 2; ++i)
    {
        if (names[i]->empty())
        {
            meshes[i]->Clear();   // deselected slot -> empty (no FileManager probe)
            *status[i] = SkydomeSlotStatus::None;
            continue;
        }
        if (!has[i] || refs[i]->modelPath.empty())
        {
            // Name chosen but the *Skydomes.xml lookup yielded no model path
            // (absent from this context's list / unreadable) -> load failure.
            meshes[i]->Clear();
            *status[i] = SkydomeSlotStatus::LoadFailed;
            continue;
        }
        const std::string aloPath = "Data\\Art\\Models\\" + refs[i]->modelPath;
        if (!meshes[i]->Load(m_fileManager, aloPath))   // Load() Clear()s on failure
        {
            *status[i] = SkydomeSlotStatus::LoadFailed;
            continue;
        }
        meshes[i]->SetScaleFactor(refs[i]->scaleFactor);
        if (m_pDevice != NULL)
        {
            // Mirror SetReferenceObject: Resolve() returns false only when NO
            // sub-mesh resolved a shader, so HasResolved() stays false and
            // RenderSkydomes gates the dome out (it falls back to the simple
            // background). Report that as LoadFailed, not a silent Ok that
            // renders nothing. Pre-device (m_pDevice == NULL) Load success alone
            // is Ok; the device-up rebuild path re-runs this and downgrades if
            // the shaders then fail to resolve.
            if (!meshes[i]->Resolve(m_shaderManager, m_pDevice))
            {
                meshes[i]->Clear();
                *status[i] = SkydomeSlotStatus::LoadFailed;
                continue;
            }
            meshes[i]->CreateBuffers(m_pDevice, m_fileManager);
        }
        *status[i] = SkydomeSlotStatus::Ok;
    }
}

// Public selection entry (bridge + startup restore). Stores the choice
// and rebuilds both meshes immediately.
void Engine::SetSkydomeEnvironment(SkydomeContext context,
                                   const std::string& primaryName,
                                   const std::string& secondaryName)
{
    if (DeviceCallsBlocked()) return;
    m_skydomeContext       = context;
    m_skydomePrimaryName   = primaryName;
    m_skydomeSecondaryName = secondaryName;
    RebuildSkydomeMeshes();
}

// Enumerate the primary + secondary dome Names for a battle context from
// the game/mod's skydome lists (device-free; safe before the device exists). Reads
// the EnsureSkydomeLists() cache so a picker-open right after a mod switch costs no
// extra GameObjectFiles scan.
void Engine::EnumerateSkydomeNames(SkydomeContext context,
                                   std::vector<std::string>& outPrimary,
                                   std::vector<std::string>& outSecondary)
{
    outPrimary.clear();
    outSecondary.clear();
    const int primAxis = (context == SkydomeContext::Land) ? (int)SkydomeAxis::LandPrimary   : (int)SkydomeAxis::SpacePrimary;
    const int secAxis  = (context == SkydomeContext::Land) ? (int)SkydomeAxis::LandSecondary : (int)SkydomeAxis::SpaceSecondary;

    const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& lists = EnsureSkydomeLists();
    for (const SkydomeRef& r : lists[primAxis]) outPrimary.push_back(r.name);
    for (const SkydomeRef& r : lists[secAxis])  outSecondary.push_back(r.name);
}

// compile IDR_SHADER_GROUND_LIT, cache parameter handles, select the
// best-validating technique (bump → gloss), and build the tangent-space ground
// vertex declaration. Graceful-degrade: on any failure m_pGroundEffect stays
// NULL and Render() falls back to the unlit fixed-function ground quad.
void Engine::InitGroundEffect()
{
    if (m_pGroundDecl == NULL)
    {
        static const D3DVERTEXELEMENT9 decl[] = {
            {0, offsetof(GroundVertex, Position), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, offsetof(GroundVertex, Normal),   D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
            {0, offsetof(GroundVertex, TexCoord), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, offsetof(GroundVertex, Tangent),  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
            {0, offsetof(GroundVertex, Binormal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
            D3DDECL_END()
        };
        m_pDevice->CreateVertexDeclaration(decl, &m_pGroundDecl);
    }

    HMODULE hMod   = GetModuleHandle(NULL);
    HRSRC   hRes   = FindResource(hMod, MAKEINTRESOURCE(IDR_SHADER_GROUND_LIT), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hData  = LoadResource(hMod, hRes);
    DWORD   dwSize = SizeofResource(hMod, hRes);
    void*   pData  = hData ? LockResource(hData) : NULL;
    if (!pData || !dwSize) return;

    LPD3DXBUFFER pErrors = NULL;
    HRESULT hr = D3DXCreateEffect(m_pDevice, pData, dwSize, NULL, NULL, 0, NULL,
                                  &m_pGroundEffect, &pErrors);
    if (FAILED(hr))
    {
#ifndef NDEBUG
        if (pErrors) fprintf(stderr, "[GroundLit] effect compile failed: %s\n",
                             (const char*)pErrors->GetBufferPointer());
#endif
        SAFE_RELEASE(pErrors);
        m_pGroundEffect = NULL;
        return;
    }
    SAFE_RELEASE(pErrors);

    m_hGroundWVP           = m_pGroundEffect->GetParameterByName(NULL, "g_WorldViewProj");
    m_hGroundWorld         = m_pGroundEffect->GetParameterByName(NULL, "g_World");
    m_hGroundSphFill       = m_pGroundEffect->GetParameterByName(NULL, "g_SphFill");
    m_hGroundLightObjVec   = m_pGroundEffect->GetParameterByName(NULL, "g_LightObjVec");
    m_hGroundLightDiffuse  = m_pGroundEffect->GetParameterByName(NULL, "g_LightDiffuse");
    m_hGroundLightSpecular = m_pGroundEffect->GetParameterByName(NULL, "g_LightSpecular");
    m_hGroundEyeObjPos     = m_pGroundEffect->GetParameterByName(NULL, "g_EyeObjPos");
    m_hGroundBaseTex       = m_pGroundEffect->GetParameterByName(NULL, "g_BaseTexture");
    m_hGroundNormalTex     = m_pGroundEffect->GetParameterByName(NULL, "g_NormalTexture");

    // Single vs_2_0/ps_2_0 bump technique. If the device can't validate it,
    // drop the effect entirely so Render() uses the unlit FF ground quad.
    // SetTechnique state survives device Reset (effect state, not device state).
    D3DXHANDLE tech = m_pGroundEffect->GetTechniqueByName("bump");
    if (tech == NULL || FAILED(m_pGroundEffect->ValidateTechnique(tech)))
    {
#ifndef NDEBUG
        fprintf(stderr, "[GroundLit] bump technique failed validation; using FF fallback\n");
#endif
        SAFE_RELEASE(m_pGroundEffect);
        m_pGroundEffect = NULL;
        return;
    }
    m_pGroundEffect->SetTechnique(tech);
#ifndef NDEBUG
    fprintf(stderr, "[GroundLit] effect loaded ok; technique=bump\n");
#endif
}

// 1x1 neutral tangent-space normal (0,0,1) packed RGB(128,128,255).
// Dynamic+default pool so it's lockable under D3D9Ex (see CreateSolidColorTexture).
void Engine::CreateGroundFlatNormal()
{
    SAFE_RELEASE(m_pGroundFlatNormalTexture);
    if (m_pDevice == NULL) return;
    if (FAILED(m_pDevice->CreateTexture(1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &m_pGroundFlatNormalTexture, NULL)))
    {
        m_pGroundFlatNormalTexture = NULL;
        return;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(m_pGroundFlatNormalTexture->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        // RGB = flat tangent-space normal (0,0,1); ALPHA = 0 so a slot with no
        // real _bc gloss map is matte (no specular) instead of fully glossy
        // (gloss lives in the _bc map's alpha — see GroundLit.fx).
        *(DWORD*)lr.pBits = D3DCOLOR_ARGB(0, 128, 128, 255);
        m_pGroundFlatNormalTexture->UnlockRect(0);
    }
}

// resolve the active slot's companion `<base>_bc` normal map from the
// game/mod via FileManager. Only the three vanilla-textured slots have a known
// base name; dirt/solid/empty-custom slots get the flat-normal fallback (lit,
// no relief). Re-run on slot change and after device Reset.
void Engine::ReloadGroundNormalTexture()
{
    SAFE_RELEASE(m_pGroundNormalTexture);
    if (m_pDevice == NULL) return;

    std::string normalLeaf;     // bundled-vanilla slots: known base + _bc.dds
    switch (m_groundTextureIndex)
    {
        case 1: normalLeaf = "W_TEMPGRND00_bc.dds"; break;  // grass
        case 2: normalLeaf = "W_SAND00_bc.dds";     break;  // sand
        case 3: normalLeaf = "W_SNOW_RGH_bc.dds";   break;  // snow
        default: break;
    }
    std::string customDerived;  // custom slots: <custombase>_bc.<ext> next to it
    if (normalLeaf.empty()
        && m_groundTextureIndex >= 0 && m_groundTextureIndex < kGroundTextureCount
        && !m_groundSlotCustomPaths[m_groundTextureIndex].empty())
    {
        const std::wstring& base = m_groundSlotCustomPaths[m_groundTextureIndex];
        size_t dot = base.find_last_of(L'.');
        std::wstring derived = (dot == std::wstring::npos)
            ? base + L"_bc"
            : base.substr(0, dot) + L"_bc" + base.substr(dot);
        customDerived = WideToAnsi(derived);
    }

    if (!normalLeaf.empty())
    {
        std::string path = "DATA\\ART\\TEXTURES\\" + normalLeaf;
        m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, path);
        if (m_pGroundNormalTexture == NULL)   // mod may stash it loose by leaf name
            m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, normalLeaf);
    }
    else if (!customDerived.empty())
    {
        m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, customDerived);
    }
#ifndef NDEBUG
    fprintf(stderr, "[GroundLit] slot=%d normal=%s\n", m_groundTextureIndex,
            m_pGroundNormalTexture ? "resolved" : "flat-fallback");
#endif
}

// draw the lit ground quad through m_pGroundEffect. World is identity
// (the quad is already in world space), so object space == world space for the
// per-pixel light/half vectors — matching the game's object-space bump path.
void Engine::RenderGroundLit()
{
    static const D3DXMATRIX Identity(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    D3DXMATRIX wvp = Identity * m_view * m_projection;

    static const float TEXTURE_SCALE  = 256;
    static const float MAP_SIZE       = 80;
    static const float UNITS_PER_CELL = 20;
    const float z = m_groundZ;
    const float h = UNITS_PER_CELL * MAP_SIZE / 2.0f;
    const float u = MAP_SIZE * UNITS_PER_CELL / TEXTURE_SCALE;
    const D3DXVECTOR3 N(0,0,1), T(1,0,0), B(0,1,0);
    const GroundVertex quad[4] = {
        {D3DXVECTOR3(-h,-h,z), N, D3DXVECTOR2(0,0), T, B},
        {D3DXVECTOR3( h,-h,z), N, D3DXVECTOR2(u,0), T, B},
        {D3DXVECTOR3(-h, h,z), N, D3DXVECTOR2(0,u), T, B},
        {D3DXVECTOR3( h, h,z), N, D3DXVECTOR2(u,u), T, B},
    };

    D3DXVECTOR3 lightVec(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
    D3DXVec3Normalize(&lightVec, &lightVec);
    D3DXVECTOR3 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);

    m_pGroundEffect->SetMatrix     (m_hGroundWVP,           &wvp);
    m_pGroundEffect->SetMatrix     (m_hGroundWorld,         &Identity);
    m_pGroundEffect->SetMatrixArray(m_hGroundSphFill,       m_sphLightFill, 3);
    m_pGroundEffect->SetValue      (m_hGroundLightObjVec,   &lightVec, sizeof(D3DXVECTOR3));
    m_pGroundEffect->SetVector     (m_hGroundLightDiffuse,  &m_lights[0].Diffuse);
    m_pGroundEffect->SetVector     (m_hGroundLightSpecular, &m_lights[0].Specular);
    m_pGroundEffect->SetValue      (m_hGroundEyeObjPos,     &eyePos, sizeof(D3DXVECTOR3));
    m_pGroundEffect->SetTexture    (m_hGroundBaseTex,       m_pGroundTexture);
    m_pGroundEffect->SetTexture    (m_hGroundNormalTex,
        m_pGroundNormalTexture ? m_pGroundNormalTexture : m_pGroundFlatNormalTexture);

    // The vertex declaration is NOT captured by the effect state block, so it
    // must be restored or the particle draws lose their diffuse-colour stream
    // Render states the effect changes ARE saved/restored by Begin/End.
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    UINT passes = 0;
    m_pGroundEffect->Begin(&passes, 0);
    for (UINT i = 0; i < passes; ++i)
    {
        m_pGroundEffect->BeginPass(i);
        m_pDevice->SetVertexDeclaration(m_pGroundDecl);
        m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(GroundVertex));
        m_pGroundEffect->EndPass();
    }
    m_pGroundEffect->End();

    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
}

bool Engine::SetSkydomeSlot(int newIndex)
{
    if (DeviceCallsBlocked()) return false;
    if (newIndex < 0 || newIndex >= kSkydomeSlotCount) return false;
    const bool wasUsingEmbeddedResource = m_skydomeUsesEmbeddedResource;
    if (newIndex == m_skydomeIndex && !wasUsingEmbeddedResource) return true;
    // Every ordinary selection returns to the existing FileManager-first
    // resolution chain, including re-selecting slot 1 after an oracle capture.
    m_skydomeUsesEmbeddedResource = false;
    if (!ReloadSkydomeTexture(newIndex))
    {
        // Fall back to Off on failure
        m_skydomeIndex = kSkydomeOffSlot;
        SAFE_RELEASE(m_pSkydomeTexture);
        return false;
    }
    m_skydomeIndex = newIndex;
#ifndef NDEBUG
    fprintf(stdout, "[Skydome] select slot=%d\n", newIndex);
#endif
    return true;
}

bool Engine::SetEmbeddedSkydomeSlotForCapture(int index)
{
    if (DeviceCallsBlocked() || index != 1)
    {
        m_skydomeIndex = kSkydomeOffSlot;
        m_skydomeUsesEmbeddedResource = false;
        SAFE_RELEASE(m_pSkydomeTexture);
        return false;
    }

    m_skydomeUsesEmbeddedResource = true;
    if (!ReloadSkydomeTexture(index))
    {
        // ReloadSkydomeTexture owns the shared failure-to-Off transition.
        return false;
    }
    m_skydomeIndex = index;
#ifndef NDEBUG
    fprintf(stdout, "[Skydome] capture embedded slot=%d\n", index);
#endif
    return true;
}

bool Engine::SetSkydomeCustomPath(int slot, const std::wstring& path)
{
    if (DeviceCallsBlocked()) return false;
    if (slot < kSkydomeFirstCustomSlot || slot >= kSkydomeSlotCount) return false;
    // Same guard as the ground slot above. The audit filed an-audit-finding against the
    // ground handler only; this sibling took its path exactly as unvalidated,
    // and unlike the ground slot the bridge PERSISTS it — so this is the one
    // that survives a restart. Capping one of a pair is not capping the pair.
    if (!IsLocalCustomAssetPath(path)) return false;
    m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot] = path;
    if (m_skydomeIndex == slot)
    {
        const bool reloaded = ReloadSkydomeTexture(slot);
        // Empty is a successful clear, even though there is intentionally no
        // texture to reload. Preserve real load failures for non-empty paths.
        return path.empty() || reloaded;
    }
    return true;
}

const std::wstring& Engine::GetSkydomeCustomPath(int slot) const
{
    static const std::wstring empty;
    if (slot < kSkydomeFirstCustomSlot || slot >= kSkydomeSlotCount) return empty;
    return m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot];
}

bool Engine::IsSkydomeSlotEmpty(int slot) const
{
    if (slot == kSkydomeOffSlot) return false;       // Off is "selectable", not empty
    if (slot < kSkydomeBundledCount) return false;   // bundled always populated
    if (slot < kSkydomeSlotCount)
        return m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot].empty();
    return true;
}
