#ifndef ALOMODEL_H
#define ALOMODEL_H

// Static-mesh + material decoder for Alamo `.alo` models.
//
// Pure-data leaf module: depends only on the editor's ChunkReader + IFile
// (no engine / D3D coupling), so the skydome render core,
// (game-object import) and all consume it. It decodes the static-mesh +
// material subset of the `.alo` chunk vocabulary, PLUS (for rigid
// multi-part placement) the skeleton (0x200) bones + the connections (0x600)
// object->bone bindings -- everything else (lights 0x1300, proxies/dazzles
// 0x603/0x604, per-vertex skinning, any unrecognized chunk) is tolerantly
// skipped. The skeleton/connection readers NEVER throw on an unexpected shape
// (those chunks were skipped wholesale before, so every previously-loading
// model must keep loading). The raw 144-byte on-disk vertex blob is cached
// verbatim so a consumer can recover tangent / extra-UV / per-vertex-bone
// fields without re-parsing. Building the runtime bone matrices + resolving
// the parent/bone-index root-sentinel convention is the CONSUMER's job (it
// needs D3D math + empirical verification), not this pure-data layer's.
//
// Format authority: the maintainer's own MIT exporter
// DrKnickers/max2alamo-2026 (alamo_format/src/alo_build.cpp), cross-checked
// against GlyphXTools/alo-viewer (MIT) and validated byte-for-byte against the
// vanilla EaW + FoC corpus. Portions derived from those MIT references.

#include <cstdint>
#include <string>
#include <vector>

class IFile;

// One authored material parameter (sub-mesh chunks 0x10102-0x10106).
struct AloShaderParam
{
    enum Kind { INT, FLOAT, FLOAT3, FLOAT4, TEXTURE };
    Kind        kind = FLOAT;
    std::string name;                  // HLSL name, e.g. "CloudScrollRate", "BaseTexture"
    int         i = 0;                 // INT
    float       f[4] = { 0, 0, 0, 0 }; // FLOAT (f[0]) / FLOAT3 (f[0..2]) / FLOAT4 (f[0..3])
    std::string tex;                   // TEXTURE: bare filename (consumer prefixes the art dir)
};

// One renderable sub-mesh: a named game shader, its material params, and the
// raw on-disk geometry blobs (kept verbatim -- 144-byte MASTER_VERTEX stride
// and 16-bit triangle-list indices -- so transcode / GPU upload is a separate
// consumer step). A sub-mesh that used the legacy 0x10005 vertex chunk is left
// with empty rawVertexBytes (out-of-scope convert path; consumer skips it).
struct AloSubMesh
{
    std::string                 shaderName;        // 0x10101, e.g. "Skydome.fx"
    std::string                 vertexFormatName;  // 0x10002, e.g. "alD3dVertNU2C"
    std::vector<AloShaderParam> params;
    uint32_t                    vertexCount = 0;
    uint32_t                    primitiveCount = 0; // triangles; index count = *3
    std::vector<unsigned char>  rawVertexBytes;    // vertexCount * kAloVertexStride
    std::vector<unsigned char>  indexBytes;        // primitiveCount * 3 * sizeof(uint16)
};

struct AloMesh
{
    std::string             name;       // 0x401
    std::vector<AloSubMesh> subMeshes;
};

// One skeleton bone (chunk 0x202). places each rigid sub-mesh by its
// bone's accumulated object-space transform. Stored VERBATIM from disk -- the
// consumer builds the runtime matrix and resolves the root-sentinel convention:
//   - `matrix` is the raw 4x3 transform in COLUMN-MAJOR order (3 columns of 4
//     floats: col0 @ matrix[0..3], col1 @ [4..7], col2 @ [8..11]); it is
//     PARENT-LOCAL (the consumer accumulates up the parent chain for object
//     space). Identity = {1,0,0,0, 0,1,0,0, 0,0,1,0}.
//   - `parentIndex` is the raw on-disk value (the root bone's sentinel -- e.g.
//     0xFFFFFFFF -- is NOT normalized here; the docs note an "importer subtracts
//     1" variant, so the consumer pins the convention against a known model).
struct AloBone
{
    std::string name;                                       // 0x203
    uint32_t    parentIndex   = 0;                          // raw; root sentinel not normalized
    bool        visible       = true;
    uint32_t    billboardMode = 0;                          // 0 for a 0x205 (no-billboard) bone
    float       matrix[12]    = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };  // 4x3 column-major, verbatim
};

// One object->bone connection (chunk 0x602): which bone places a given object.
// `objectIndex` indexes the combined meshes++lights array; v1 imports meshes
// only (lights skipped), so objectIndex == the mesh ordinal. `boneIndex` is the
// raw on-disk value (same root-sentinel caveat as AloBone::parentIndex).
struct AloConnection
{
    uint32_t objectIndex = 0;
    uint32_t boneIndex   = 0;
};

struct AloModel
{
    std::vector<AloMesh>       meshes;       // one per 0x400 chunk
    std::vector<AloBone>       bones;        // 0x202 under the 0x200 skeleton (empty if none)
    std::vector<AloConnection> connections;  // 0x602 under the 0x600 connections (empty if none)
};

// On-disk vertex stride is a fixed 144-byte MASTER_VERTEX for EVERY vertex
// format (the format name in 0x10002 only selects which fields the GPU decl
// reads; unused fields are zero-filled). Offsets within the record, from
// max2alamo append_vertex():
//   pos@0, normal@12, uv0@24, uv1-3@32/40/48, tangent@56, binormal@68,
//   color(float4)@80, reserved@96, boneIdx[4]@112, weights[4]@128.
static const long kAloVertexStride = 144;
static const long kAloColorOffset  = 80;   // float4 RGBA (RGB@80, A@92)

// Sub-mesh classification helpers (pure string checks) shared by the
// renderer (ReferenceObjectMesh skips these sub-meshes) and the catalog
// (GameObjectCatalog greys out a model that has no renderable rigid sub-mesh).
// Kept on this pure-data leaf so the picker's "skinned / unsupported" verdict
// stays in lockstep with what the renderer actually skips -- if the set of
// skipped formats/shaders ever changes, both consumers move together.
//
//   AloIsSkinnedVertexFormat -- a 0x10002 format the rigid path can't upload
//     (per-vertex bone indices/weights need a bone-matrix palette v1 lacks):
//     "alD3dVertRSkin*" / "alD3dVertB4I4*".
//   AloIsNonVisibleShader -- a 0x10101 shader that is collision / shadow-volume
//     scaffolding, never drawn as visible geometry.
bool AloIsSkinnedVertexFormat(const std::string& vertexFormatName);
bool AloIsNonVisibleShader(const std::string& shaderName);

// Parse the static-mesh + material subset of an `.alo` from `file` (caller
// retains ownership; the parser AddRef/Releases the file internally). Returns
// a model with one AloMesh per 0x400 chunk.
//
// Throws (never returns partially built):
//   WrongFileException -- no mesh (0x400) chunks present (not a model .alo)
//   BadFileException   -- malformed: nesting past ChunkReader's depth guard,
//                         vertex stride != 144, count/payload mismatch,
//                         vertexCount > 0xFFFF, or a bad/over-read string
//   ReadException      -- truncated file
AloModel LoadAloModel(IFile* file);

#endif
