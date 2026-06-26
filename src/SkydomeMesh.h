#ifndef SKYDOMEMESH_H
#define SKYDOMEMESH_H

// Skydome render core -- the GPU-side companion to AloModel.
//
// Holds a decoded `.alo` dome as a flat list of SubMeshGpu, each running its
// OWN named game shader 1:1 (Skydome.fx / MeshGloss.fxo / MeshAdditive.fx, ...)
// loaded straight from the game/mod via ShaderManager -- never a fork or an
// approximation (render-parity hard rule). The transcoded geometry + material
// params are cached on the CPU so a device reset refills the D3DPOOL_DEFAULT
// VB/IB with a plain memcpy -- no re-parse, no FileManager hit.
//
// Lifecycle (driven by the engine):
//   Load        -- decode + transcode (CPU only; no device touch)
//   Resolve     -- getShader per sub-mesh + per-format vertex decl + by-name
//                  material handles (needs a valid device)
//   CreateBuffers -- DEFAULT-pool VB/IB + material textures (load AND reset)
//   OnLostDevice  -- release VB/IB + textures + effect->OnLostDevice (keep refs)
//   OnResetEffects + CreateBuffers -- the two-phase device-reset refill
//
// Decoupled from the engine via the IShaderManager / IFileManager interfaces so
// it is unit-testable with mocks, the same way AloModel / SkydomeEnvironment are.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <d3d9.h>
#include <d3dx9.h>

#include "AloModel.h"   // AloShaderParam

class Effect;
class IShaderManager;
class IFileManager;

// GPU-resident state for one `.alo` sub-mesh, plus the cached CPU data needed
// to rebuild it after a device reset without re-parsing the model.
struct SubMeshGpu
{
    SubMeshGpu() { D3DXMatrixIdentity(&placement); }

    // --- cached CPU data (survives device lost/reset) ---
    std::string                 shaderName;        // 0x10101, e.g. "Skydome.fx"
    std::string                 vertexFormatName;  // 0x10002, e.g. "alD3dVertNU2C"
    std::vector<AloShaderParam> params;            // authored material params
    std::vector<unsigned char>  vertexBytes;       // transcoded to `stride` (runtime decl)
    std::vector<unsigned char>  indexBytes;        // uint16 triangle list
    uint32_t                    stride = 0;        // runtime vertex stride (== decl size)
    uint32_t                    vertexCount = 0;
    uint32_t                    primitiveCount = 0;

    // --- object-space placement (from the .alo 0x200 skeleton + 0x600 connections) ---
    // This sub-mesh's parent mesh's bone, accumulated to object space (identity if
    // the dome has no skeleton / no connection). The vanilla star domes carry a
    // sun BILLBOARD sub-mesh on a 0x206 bone: `billboardMode != 0` flags it so the
    // draw replaces this matrix's orientation with a camera-facing one. Without
    // that, the flat additive sun quad is drawn at the dome centre, un-billboarded,
    // and collapses to a 1px additive-white line when viewed edge-on (the artifact
    // long misread as a "closure-meridian seam"). See Engine::RenderSkydomeMesh.
    D3DXMATRIX                  placement;
    uint32_t                    billboardMode = 0;

    // --- GPU handles (released on device lost; refilled on reset) ---
    Effect*                         effect = nullptr;  // owned ref (ShaderManager::getShader); NULL => skipped
    IDirect3DVertexBuffer9*         vb     = nullptr;  // D3DPOOL_DEFAULT
    IDirect3DIndexBuffer9*          ib     = nullptr;  // D3DPOOL_DEFAULT, INDEX16
    IDirect3DVertexDeclaration9*    decl   = nullptr;  // borrowed from SkydomeMesh::m_decls; survives reset
    std::vector<IDirect3DTexture9*> matTextures;       // parallel to `params` (NULL for non-TEXTURE), D3DPOOL_DEFAULT
    std::vector<D3DXHANDLE>          matHandles;        // parallel to `params`; handles into effect->getD3DEffect()
};

class SkydomeMesh
{
public:
    SkydomeMesh() = default;
    ~SkydomeMesh();
    SkydomeMesh(const SkydomeMesh&) = delete;             // owns GPU refs
    SkydomeMesh& operator=(const SkydomeMesh&) = delete;

    // Decode + transcode the `.alo` at `aloPath` (resolved via `fm`). CPU only:
    // touches no device. Replaces any prior contents (Clear() first). Returns
    // false (mesh left empty) on a FileManager miss, a parse failure, or zero
    // renderable sub-meshes. A 0x10005 (legacy-vertex) sub-mesh is dropped here.
    // The dome's authored UVs are uploaded verbatim (faithful to the asset; any
    // closure-meridian seam the asset bakes is rendered as the game shows it).
    bool Load(IFileManager& fm, const std::string& aloPath);

    // Release all GPU resources + cached CPU data and empty the mesh. Used to
    // clear a deselected slot without a wasted FileManager probe.
    void Clear();

    // Resolve each sub-mesh's game shader (sm.getShader, ext-tolerant .fx->.FXO),
    // build/share the per-format vertex decl, and cache by-name material handles.
    // Needs a valid device. Per-sub-mesh degrade: a shader miss leaves that
    // sub-mesh's effect NULL (skipped at draw); siblings still resolve. Returns
    // true if at least one sub-mesh resolved.
    bool Resolve(IShaderManager& sm, IDirect3DDevice9* dev);

    // Create the DEFAULT-pool VB/IB (memcpy the cached transcoded blobs) and load
    // each TEXTURE param via "Data\\Art\\Textures\\"+bareName (bare-name fallback
    // for loose mod files). Called at first load AND on device reset (refills
    // from cache). Skips sub-meshes whose effect is NULL. Idempotent-safe:
    // releases any existing buffers/textures first.
    void CreateBuffers(IDirect3DDevice9* dev, IFileManager& fm);

    // Device-lost: release the DEFAULT-pool VB/IB + material textures and call
    // effect->OnLostDevice per sub-mesh. Keeps the Effect* refs and the shared
    // decls (decls are not pool-bound; released only in the dtor). Idempotent.
    void OnLostDevice();

    // Device-reset phase 1: per sub-mesh effect->OnResetDevice. Split from
    // CreateBuffers so the engine can do all-effects-then-all-buffers across both
    // domes (matches the existing reset dance). The buffer
    // refill is CreateBuffers (phase 2).
    void OnResetEffects();

    bool   IsEmpty()      const { return m_subMeshes.empty(); }
    bool   HasResolved()  const;                 // >=1 sub-mesh with a non-NULL effect
    float  ScaleFactor()  const { return m_scaleFactor; }
    void   SetScaleFactor(float s) { m_scaleFactor = s; }

    std::vector<SubMeshGpu>&       SubMeshes()       { return m_subMeshes; }
    const std::vector<SubMeshGpu>& SubMeshes() const { return m_subMeshes; }

private:
    std::vector<SubMeshGpu> m_subMeshes;
    // Per-format vertex declarations, shared across sub-meshes, owned here.
    // Not D3DPOOL-bound: survive device reset (released on Load/Clear replace and
    // in the dtor, NEVER on device-lost).
    std::map<std::string, IDirect3DVertexDeclaration9*> m_decls;
    float m_scaleFactor = 1.0f;

    void ReleaseGpuBuffers();   // VB/IB + matTextures (the lost-device set)
    void ReleaseEffects();      // SAFE_RELEASE the owned Effect* refs
    void ReleaseDecls();        // Load/Clear replace + dtor (never on device-lost)

    // Build (or fetch the cached) vertex decl for `formatName`. Returns a
    // borrowed pointer owned by m_decls, or NULL if the device call fails.
    // (Stride is gpu.stride, set authoritatively in Load -- not recomputed here.)
    IDirect3DVertexDeclaration9* GetOrCreateDecl(IDirect3DDevice9* dev,
                                                 const std::string& formatName);
};

#endif
