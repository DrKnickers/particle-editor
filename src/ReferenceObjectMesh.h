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
//   - RSkin (1-bone "rigid skinning") sub-meshes now RENDER in BIND POSE: the
//     verts are model-space, so a uniform objectWorld skin palette (all 24 bones)
//     reproduces the rest pose (verified vs the alo-viewer). B4I4 (4-bone, distinct
//     blend-index/weight layout) is still DROPPED; SkippedSkinned() reports it.
//   - Invisible COLLISION / SHADOW sub-meshes (MeshCollision.fx / MeshShadowVolume.fx
//     / RSkinShadowVolume.fx) are dropped -- they would render as solid hulls.

#include <cstdint>
#include <map>
#include <set>
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
    AloRenderClass              renderClass = ALO_RC_OPAQUE;  // phase/blend bucket (AloClassifyShader)
    bool                        skinned = false;              // RSkin: render in bind pose (uniform objectWorld skin palette); model-space verts (no `placement`)

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
    // touches no device. Replaces any prior contents. Drops 0x402-hidden meshes +
    // skinned + occluded/heat sub-meshes; KEEPS opaque (incl. collision = flat
    // blue) + transparent (additive/alpha) in the visible SubMeshes() list, and
    // keeps shadow-volume sub-meshes (MeshShadowVolume.fx / RSkinShadowVolume.fx)
    // in a SEPARATE ShadowSubMeshes() bucket (for a later stencil-shadow pass --
    // they must NOT be in the visible list or they'd draw as solid hulls). Records
    // each kept sub-mesh's phase/blend class + bone placement, and the object-space
    // AABB over the kept VISIBLE geometry. Returns false (mesh left empty) on a
    // FileManager miss, a parse failure, or zero renderable rigid sub-meshes.
    //
    // `hideBoneNamesLower` (already lower-cased) names UNIT bones whose meshes
    // are damaged-state geometry the engine shows only when a hardpoint is destroyed
    // (a hardpoint's Damage_Decal / Damage_Particles / Collision_Mesh bones). A mesh
    // whose 0x602 connection bone is in the set is dropped, so the intact unit renders
    // without it. Empty set (the default) = no extra hiding.
    bool Load(IFileManager& fm, const std::string& aloPath,
              const std::set<std::string>& hideBoneNamesLower = {});

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

    // Release only the DEFAULT-pool buffers/textures. ShaderManager owns the
    // deduplicated effect lifecycle during a full device reset.
    void ReleaseGpuResources() { ReleaseGpuBuffers(); }

    bool IsEmpty()      const { return m_subMeshes.empty(); }
    bool HasResolved()  const;                  // >=1 sub-mesh with a non-NULL effect
    bool SkippedSkinned() const { return m_skippedSkinned; }   // dropped >=1 skinned sub-mesh

    // Object-space AABB over the KEPT (drawn) geometry, accumulated at Load.
    // Used both to draw the selection box and as the click-to-select hit target, so
    // the visual box == the pickable region. False (box untouched) when empty.
    bool GetBoundingBox(D3DXVECTOR3& outMin, D3DXVECTOR3& outMax) const;

    // Object-space matrix of a bone by name (CASE-INSENSITIVE -- hardpoint XML
    // is all-caps, the .alo is mixed-case). Used to mount a hardpoint's attach model
    // at its Attachment_Bone. False (out untouched) if the bone isn't in the skeleton.
    bool GetBoneObjectMatrix(const std::string& boneName, D3DXMATRIX& out) const;

    std::vector<RefSubMeshGpu>&       SubMeshes()       { return m_subMeshes; }
    const std::vector<RefSubMeshGpu>& SubMeshes() const { return m_subMeshes; }

    // Shadow-volume sub-meshes (MeshShadowVolume.fx / RSkinShadowVolume.fx), kept
    // OUT of the visible list so a later stencil-shadow pass can draw them without
    // them appearing as solid hulls. Shares the full GPU lifecycle with m_subMeshes.
    std::vector<RefSubMeshGpu>&       ShadowSubMeshes()       { return m_shadowSubMeshes; }
    const std::vector<RefSubMeshGpu>& ShadowSubMeshes() const { return m_shadowSubMeshes; }

private:
    std::vector<RefSubMeshGpu> m_subMeshes;
    std::vector<RefSubMeshGpu> m_shadowSubMeshes;  // shadow-volume bucket (separate render pass)
    std::map<std::string, IDirect3DVertexDeclaration9*> m_decls;  // per-format, shared
    // Object-space matrix per bone, keyed by LOWER-CASED name -- retained from
    // Load (computeBoneObjectMatrices) so an attach model can be mounted at a named
    // Attachment_Bone. Empty when the model has no skeleton.
    std::map<std::string, D3DXMATRIX> m_boneObjByName;
    bool m_skippedSkinned = false;
    D3DXVECTOR3 m_boundMin = D3DXVECTOR3(0, 0, 0);   // object-space AABB over kept geometry
    D3DXVECTOR3 m_boundMax = D3DXVECTOR3(0, 0, 0);
    bool m_hasBounds = false;

    void ReleaseGpuBuffers();
    void ReleaseEffects();
    void ReleaseDecls();

    IDirect3DVertexDeclaration9* GetOrCreateDecl(IDirect3DDevice9* dev,
                                                 const std::string& formatName);
};

#endif
