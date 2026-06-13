#include "AloModel.h"

#include "ChunkFile.h"
#include "exceptions.h"

// Static-mesh subset of the Alamo `.alo` chunk vocabulary. See AloModel.h for
// provenance (max2alamo-2026 writer + alo-viewer reader, both MIT). The chunk
// nesting that matters here:
//
//   0x0400  Mesh                       (container)
//     0x0401  name                     (string)
//     0x0402  info                     (data; skipped)
//     0x10100 SubMesh material         (container)   -- SIBLING of 0x10000
//       0x10101 shader name            (string)
//       0x10102..0x10106 params        (data; name(1)/value(2) mini-chunks)
//     0x10000 SubMesh geometry         (container)
//       0x10001 sizes                  (data; u32 vertexCount, u32 faceCount, ...)
//       0x10002 vertex format name     (string)
//       0x10007 vertex data (rev 2)    (data; 144 B / vertex)
//       0x10005 vertex data (legacy)   (data; out of scope -> sub-mesh skipped)
//       0x10004 index data             (data; u16 triangle list)
namespace
{
    const ChunkType CHUNK_MESH          = 0x0400;
    const ChunkType CHUNK_MESH_NAME     = 0x0401;
    const ChunkType CHUNK_SUBMESH_MAT   = 0x10100;
    const ChunkType CHUNK_SHADER_NAME   = 0x10101;
    const ChunkType CHUNK_PARAM_INT     = 0x10102;
    const ChunkType CHUNK_PARAM_FLOAT   = 0x10103;
    const ChunkType CHUNK_PARAM_FLOAT3  = 0x10104;
    const ChunkType CHUNK_PARAM_TEXTURE = 0x10105;
    const ChunkType CHUNK_PARAM_FLOAT4  = 0x10106;
    const ChunkType CHUNK_GEOMETRY      = 0x10000;
    const ChunkType CHUNK_SIZES         = 0x10001;
    const ChunkType CHUNK_VERTEX_FMT    = 0x10002;
    const ChunkType CHUNK_VERTEX_OLD    = 0x10005;
    const ChunkType CHUNK_VERTEX_NEW    = 0x10007;
    const ChunkType CHUNK_INDICES       = 0x10004;

    const long kIndexSize = 2;  // uint16 triangle-list indices

    // Bounds for file-derived counts, to cap allocation and keep the 16-bit
    // index space valid. kAloMaxPrimitives is generous (1M triangles) so real
    // detailed meshes (game objects) load while pathological counts don't.
    const uint32_t kAloMaxVertices   = 0xFFFF;
    const uint32_t kAloMaxPrimitives = 0x100000;

    // Material-param mini-chunk ids (max2alamo alo_build.cpp).
    const ChunkType MINI_PARAM_NAME  = 1;
    const ChunkType MINI_PARAM_VALUE = 2;

    // Throw on a violated structural invariant (mirrors ParticleSystem.cpp).
    inline void Verify(bool cond)
    {
        if (!cond) throw BadFileException();
    }

    // Typed readers over the current data chunk OR its current mini-chunk;
    // ChunkReader::size() abstracts which, and read() is valid inside a leaf
    // chunk's mini-chunks (m_size stays >= 0 there). Mirrors the read helpers
    // in ParticleSystem.cpp: integers are little-endian, floats raw-copied
    // (IEEE-754 LE == native on x86/x64, the editor's only targets).
    uint32_t readU32(ChunkReader& r)
    {
        Verify(r.size() == (long)sizeof(uint32_t));
        uint32_t v = 0;
        r.read(&v, sizeof(v));
        return letohl(v);
    }

    float readF32(ChunkReader& r)
    {
        Verify(r.size() == (long)sizeof(float));
        float v = 0.0f;
        r.read(&v, sizeof(v));
        return v;
    }

    // Read one already-entered material param chunk (its Kind from the chunk
    // id) by walking its name(1) / value(2) mini-chunks.
    AloShaderParam ReadParam(ChunkReader& r, AloShaderParam::Kind kind)
    {
        AloShaderParam p;
        p.kind = kind;
        ChunkType mt;
        while ((mt = r.nextMini()) != -1)
        {
            if (mt == MINI_PARAM_NAME)
            {
                p.name = r.readString();
            }
            else if (mt == MINI_PARAM_VALUE)
            {
                switch (kind)
                {
                    case AloShaderParam::INT:     p.i = (int)readU32(r); break;
                    case AloShaderParam::FLOAT:   p.f[0] = readF32(r);   break;
                    case AloShaderParam::FLOAT3:  Verify(r.size() == 12); r.read(p.f, 12); break;
                    case AloShaderParam::FLOAT4:  Verify(r.size() == 16); r.read(p.f, 16); break;
                    case AloShaderParam::TEXTURE: p.tex = r.readString(); break;
                }
            }
            // Unknown mini-chunk: nextMini() skips it on the next iteration.
        }
        return p;
    }

    // r is positioned inside a 0x10100 submesh-material container.
    void ReadSubMeshMaterial(ChunkReader& r, AloSubMesh& sm)
    {
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_SHADER_NAME:   sm.shaderName = r.readString(); break;
                case CHUNK_PARAM_INT:     sm.params.push_back(ReadParam(r, AloShaderParam::INT));     break;
                case CHUNK_PARAM_FLOAT:   sm.params.push_back(ReadParam(r, AloShaderParam::FLOAT));   break;
                case CHUNK_PARAM_FLOAT3:  sm.params.push_back(ReadParam(r, AloShaderParam::FLOAT3));  break;
                case CHUNK_PARAM_TEXTURE: sm.params.push_back(ReadParam(r, AloShaderParam::TEXTURE)); break;
                case CHUNK_PARAM_FLOAT4:  sm.params.push_back(ReadParam(r, AloShaderParam::FLOAT4));  break;
                default:
                    // Unknown CONTAINER must be skipped explicitly (next() would
                    // descend into it); unknown DATA chunks are auto-skipped by
                    // the next next() call.
                    if (r.size() < 0) r.skip();
                    break;
            }
        }
    }

    // r is positioned inside a 0x10000 submesh-geometry container.
    void ReadGeometry(ChunkReader& r, AloSubMesh& sm)
    {
        bool sawOldVertex = false;
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_SIZES:
                {
                    // Fixed 128-byte chunk; only the leading two u32 matter.
                    Verify(r.size() >= 8);
                    uint32_t counts[2] = { 0, 0 };
                    r.read(counts, sizeof(counts));   // next() skips the reserved tail
                    sm.vertexCount    = letohl(counts[0]);
                    sm.primitiveCount = letohl(counts[1]);
                    break;
                }
                case CHUNK_VERTEX_FMT:
                    sm.vertexFormatName = r.readString();
                    break;
                case CHUNK_VERTEX_NEW:
                {
                    const long sz = r.size();
                    Verify(sm.vertexCount > 0 && sm.vertexCount <= kAloMaxVertices);  // 16-bit index space
                    // 64-bit product: MSVC `long` is 32-bit even on x64, so a 32-bit
                    // product could wrap and let a crafted count slip past this check.
                    Verify((unsigned long long)sz == (unsigned long long)sm.vertexCount * kAloVertexStride);
                    sm.rawVertexBytes.resize((size_t)sz);
                    r.read(sm.rawVertexBytes.data(), sz);
                    break;
                }
                case CHUNK_VERTEX_OLD:
                    // Legacy 128-byte vertex: out of scope. Leave rawVertexBytes
                    // empty (consumer skips this sub-mesh); next() skips the data.
                    sawOldVertex = true;
                    break;
                case CHUNK_INDICES:
                {
                    const long sz = r.size();
                    Verify(sm.primitiveCount <= kAloMaxPrimitives);   // bound the allocation below
                    // 64-bit product (see CHUNK_VERTEX_NEW): primitiveCount is otherwise
                    // unbounded, so a 32-bit overflow could bypass this size check.
                    Verify((unsigned long long)sz == (unsigned long long)sm.primitiveCount * 3 * kIndexSize);
                    sm.indexBytes.resize((size_t)sz);
                    if (sz > 0) r.read(sm.indexBytes.data(), sz);
                    break;
                }
                default:
                    // 0x1200 collision (container) -> skip; 0x10006 skin-remap
                    // (data) -> auto-skipped.
                    if (r.size() < 0) r.skip();
                    break;
            }
        }
        // A modern sub-mesh must have produced a vertex blob; the only
        // legitimate empty case is the legacy 0x10005 path.
        if (sm.rawVertexBytes.empty() && !sawOldVertex)
            throw BadFileException();
    }

    // r is positioned inside a 0x0400 mesh container. Per the format, each
    // sub-mesh's 0x10100 material is immediately followed by its 0x10000
    // geometry as a sibling, so geometry attaches to the most recent sub-mesh.
    void ReadMesh(ChunkReader& r, AloMesh& mesh)
    {
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_MESH_NAME:
                    mesh.name = r.readString();
                    break;
                case CHUNK_SUBMESH_MAT:
                    mesh.subMeshes.emplace_back();
                    ReadSubMeshMaterial(r, mesh.subMeshes.back());
                    break;
                case CHUNK_GEOMETRY:
                    Verify(!mesh.subMeshes.empty());   // geometry without a material is malformed
                    ReadGeometry(r, mesh.subMeshes.back());
                    break;
                default:
                    // 0x402 info (data) auto-skips; unknown containers skip().
                    if (r.size() < 0) r.skip();
                    break;
            }
        }
    }
}

AloModel LoadAloModel(IFile* file)
{
    ChunkReader r(file);
    AloModel model;

    ChunkType t;
    while ((t = r.next()) != -1)
    {
        if (t == CHUNK_MESH)
        {
            model.meshes.emplace_back();
            ReadMesh(r, model.meshes.back());
        }
        else if (r.size() < 0)
        {
            // 0x200 skeleton / 0x1300 lights / 0x600 connections (containers).
            r.skip();
        }
        // Any unexpected root DATA chunk is auto-skipped by the next next().
    }

    if (model.meshes.empty())
        throw WrongFileException();   // not a mesh-bearing .alo

    return model;
}
