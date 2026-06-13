#ifndef ALOMODEL_H
#define ALOMODEL_H

// Static-mesh + material decoder for Alamo `.alo` models.
//
// Pure-data leaf module: depends only on the editor's ChunkReader + IFile
// (no engine / D3D coupling), so the skydome render core,
// (game-object import) and all consume it. It decodes only the
// static-mesh subset of the `.alo` chunk vocabulary -- skeleton (0x200),
// lights (0x1300), connections/proxies (0x600) and any unrecognized chunk
// are tolerantly skipped. Skinning / animation are out of scope; the raw
// 144-byte on-disk vertex blob is cached verbatim so a later consumer can
// recover bone / extra-UV / tangent fields without re-parsing.
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

struct AloModel
{
    std::vector<AloMesh> meshes;        // one per 0x400 chunk
};

// On-disk vertex stride is a fixed 144-byte MASTER_VERTEX for EVERY vertex
// format (the format name in 0x10002 only selects which fields the GPU decl
// reads; unused fields are zero-filled). Offsets within the record, from
// max2alamo append_vertex():
//   pos@0, normal@12, uv0@24, uv1-3@32/40/48, tangent@56, binormal@68,
//   color(float4)@80, reserved@96, boneIdx[4]@112, weights[4]@128.
static const long kAloVertexStride = 144;
static const long kAloColorOffset  = 80;   // float4 RGBA (RGB@80, A@92)

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
