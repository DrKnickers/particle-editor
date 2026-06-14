#ifndef REFERENCEOBJECTMESH_H
#define REFERENCEOBJECTMESH_H

// Inert reference-object render core -- the GPU-side companion to AloModel
// for imported game objects (a turret / vehicle / structure dropped into the
// preview as a scale reference). Loads a real `.alo` and renders each rigid
// sub-mesh with its OWN named game shader 1:1 via ShaderManager (the no-fork
// render-parity rule), but -- unlike SkydomeMesh -- as SOLID, depth-tested,
// backface-culled geometry placed at a fixed world transform, with each rigid
// sub-mesh positioned by its skeleton bone's accumulated object-space matrix
// (rigid multi-part: a turret's barrel/base/treads each ride a different bone).
//
// Structurally CLONED from SkydomeMesh (same Load -> Resolve -> CreateBuffers ->
// OnLostDevice -> OnResetEffects lifecycle + the DEFAULT-pool VB/IB + cached-CPU
// refill-on-reset model) rather than sharing it: the dome transcoder is a
// 3-format subset that is wrong for the tangent/binormal unit formats, and the
// dome render state is INVERTED (depth/cull/blend), so a clone is cleaner than
// risky surgery on the shipped (render-unverified) dome code.
//
// v1 deferrals, enforced at Load:
//   - SKINNED sub-meshes (vertexFormatName starts "alD3dVertRSkin" / "alD3dVertB4I4")
//     are DROPPED -- they need a bone-matrix palette the editor has no source for.
//     SkippedSkinned() reports it so the picker can show "skinned -- not supported".
//   - Invisible COLLISION / SHADOW sub-meshes (MeshCollision.fx / MeshShadowVolume.fx
//     / RSkinShadowVolume.fx) are dropped -- they would render as solid hulls.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <d3d9.h>
#include <d3dx9.h>

#include "AloModel.h"   // AloShaderParam, AloModel (bones/connections)

class Effect;
class IShaderManager;
class IFileManager;

// GPU-resident state for one rigid `.alo` sub-mesh, plus the cached CPU data to
// rebuild it after a device reset and the bone placement that positions it.
struct RefSubMeshGpu
{
    // --- cached CPU data (survives device lost/reset) ---
    std::string                 shaderName;        // 0x10101, e.g. "MeshBumpColorize.fx"
    std::string                 vertexFormatName;  // 0x10002, e.g. "alD3dVertNU2U3U3"
    std::vector<AloShaderParam> params;            // authored material params
    std::vector<unsigned char>  vertexBytes;       // transcoded to `stride` (runtime decl)
    std::vector<unsigned char>  indexBytes;        // uint16 triangle list
    uint32_t                    stride = 0;        // runtime vertex stride (== decl size)
    uint32_t                    vertexCount = 0;
    uint32_t                    primitiveCount = 0;

    // Object-space placement = this sub-mesh's parent mesh's bone, accumulated up
    // the parent chain (rigid). Identity when the model has no usable skeleton.
    // The engine multiplies this by the live position/rotation world at draw.
    D3DXMATRIX                  placement;

    // --- GPU handles (released on device lost; refilled on reset) ---
    Effect*                         effect = nullptr;  // owned ref (ShaderManager::getShader); NULL => skipped
    IDirect3DVertexBuffer9*         vb     = nullptr;  // D3DPOOL_DEFAULT
    IDirect3DIndexBuffer9*          ib     = nullptr;  // D3DPOOL_DEFAULT, INDEX16
    IDirect3DVertexDeclaration9*    decl   = nullptr;  // borrowed from m_decls; survives reset
    std::vector<IDirect3DTexture9*> matTextures;       // parallel to params (NULL for non-TEXTURE)
    std::vector<D3DXHANDLE>          matHandles;        // parallel to params; handles into effect

    RefSubMeshGpu() { D3DXMatrixIdentity(&placement); }
};

class ReferenceObjectMesh
{
public:
    ReferenceObjectMesh() = default;
    ~ReferenceObjectMesh();
    ReferenceObjectMesh(const ReferenceObjectMesh&) = delete;
    ReferenceObjectMesh& operator=(const ReferenceObjectMesh&) = delete;

    // Decode + transcode the `.alo` at `aloPath` (resolved via `fm`). CPU only:
    // touches no device. Replaces any prior contents. Drops skinned + collision/
    // shadow sub-meshes; computes each kept sub-mesh's bone placement matrix from
    // the model's skeleton + connections. Returns false (mesh left empty) on a
    // FileManager miss, a parse failure, or zero renderable rigid sub-meshes.
    bool Load(IFileManager& fm, const std::string& aloPath);

    // Release all GPU + cached CPU data and empty the mesh.
    void Clear();

    // Resolve each sub-mesh's game shader + per-format vertex decl + by-name
    // material handles (needs a valid device). Per-sub-mesh degrade. Returns true
    // if at least one sub-mesh resolved.
    bool Resolve(IShaderManager& sm, IDirect3DDevice9* dev);

    // Create the DEFAULT-pool VB/IB (memcpy the cached transcoded blobs) and load
    // each TEXTURE param. Called at first load AND on device reset.
    void CreateBuffers(IDirect3DDevice9* dev, IFileManager& fm);

    void OnLostDevice();      // release VB/IB + textures + effect->OnLostDevice
    void OnResetEffects();    // effect->OnResetDevice (phase 1 of the two-phase reset)

    bool IsEmpty()      const { return m_subMeshes.empty(); }
    bool HasResolved()  const;                  // >=1 sub-mesh with a non-NULL effect
    bool SkippedSkinned() const { return m_skippedSkinned; }   // dropped >=1 skinned sub-mesh

    std::vector<RefSubMeshGpu>&       SubMeshes()       { return m_subMeshes; }
    const std::vector<RefSubMeshGpu>& SubMeshes() const { return m_subMeshes; }

private:
    std::vector<RefSubMeshGpu> m_subMeshes;
    std::map<std::string, IDirect3DVertexDeclaration9*> m_decls;  // per-format, shared
    bool m_skippedSkinned = false;

    void ReleaseGpuBuffers();
    void ReleaseEffects();
    void ReleaseDecls();

    IDirect3DVertexDeclaration9* GetOrCreateDecl(IDirect3DDevice9* dev,
                                                 const std::string& formatName);
};

#endif
