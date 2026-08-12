#include "AloModel.h"

#include "ChunkFile.h"
#include "exceptions.h"
#include "ResourceLimits.h"

#include <string.h>   // _stricmp / _strnicmp (case-insensitive shader-name match)

#ifdef ALO_MODEL_TEST_PROBES
namespace
{
    size_t g_aloSubMeshConstructionCount = 0;
}

AloSubMesh::AloSubMesh()
{
    ++g_aloSubMeshConstructionCount;
}

void AloResetSubMeshConstructionCountForTesting()
{
    g_aloSubMeshConstructionCount = 0;
}

size_t AloSubMeshConstructionCountForTesting()
{
    return g_aloSubMeshConstructionCount;
}
#endif

// Static-mesh subset of the Alamo `.alo` chunk vocabulary. See AloModel.h for
// provenance (max2alamo-2026 writer + alo-viewer reader, both MIT). The chunk
// nesting that matters here:
//
//   0x0400  Mesh                       (container)
//     0x0401  name                     (string)
//     0x0402  info                     (data; per-mesh hidden + collision flags)
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
    const ChunkType CHUNK_MESH_INFO     = 0x0402;   // data: subMeshCount, bbox, isHidden, isCollision
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

    // Skeleton + connections (decoded for rigid multi-part placement).
    const ChunkType CHUNK_SKELETON      = 0x200;    // container
    //  0x201 info (data; boneCount + 124 reserved -- we trust the actual 0x202 count, not this)
    const ChunkType CHUNK_BONE          = 0x202;    // container, repeated
    const ChunkType CHUNK_BONE_NAME     = 0x203;    // string
    const ChunkType CHUNK_BONE_DATA     = 0x205;    // data, 56 B (parentIndex, visible, matrix[12])
    const ChunkType CHUNK_BONE_DATA_BB  = 0x206;    // data, 60 B (+ billboardMode before the matrix)
    const ChunkType CHUNK_CONNECTIONS   = 0x600;    // container
    //  0x601 counts (data) / 0x603 proxies / 0x604 dazzles -- skipped
    const ChunkType CHUNK_CONN_OBJECT   = 0x602;    // data leaf, mini-chunks

    // Bone-data leaf sizes (max2alamo alo_build.cpp: build_bone_data).
    const long kBoneData205Bytes = 56;   // parentIndex(4) + visible(4) + matrix(48)
    const long kBoneData206Bytes = 60;   // + billboardMode(4)

    // 0x402 MESH-INFO: leading bytes up to + including the two flag u32s. Writer
    // pads to 128 B (alo_build.cpp:145-161); we read only what we need + let
    // next() skip the tail. subMeshCount(4)+bboxMin(12)+bboxMax(12)+unused(4)
    // = 32 before isHidden(4) @32 + isCollision(4) @36.
    const long kMeshInfo402MinBytes = 40;
    const long kMeshInfo402SkipBytes = 32;

    // 0x602 connection mini-chunk ids (alo_build.cpp build_connections).
    const ChunkType MINI_CONN_OBJECT = 2;   // u32 objectIndex
    const ChunkType MINI_CONN_BONE   = 3;   // u32 boneIndex

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

    // Enforce the container/data KIND of the just-entered chunk (the one the
    // caller reached via next()). A crafted .alo can flip the high 'container'
    // bit on a dispatch chunk -- e.g. a 0x200 skeleton arriving as a DATA chunk,
    // or a 0x602 connection leaf as a CONTAINER. Without a kind check the
    // container-expecting sub-loops below would walk sibling chunks and consume
    // the parent's -1 terminator, double-popping ChunkReader's depth to -1
    // (Debug assert / Release OOB read at ChunkReader.cpp:52); the mini-chunk
    // walkers would hit nextMini()'s assert(m_size >= 0) at ChunkReader.cpp:11.
    // Reject the file instead (issue #487). After next(), size() == m_size, so
    // size() < 0 <=> container (m_size == -1), size() >= 0 <=> data leaf.
    inline void VerifyContainer(ChunkReader& r) { Verify(r.size() < 0); }
    inline void VerifyDataLeaf(ChunkReader& r)  { Verify(r.size() >= 0); }

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

    // Read a u32 at the reader's current position WITHIN a multi-field data
    // chunk (unlike readU32, does NOT assert the whole chunk is 4 bytes). The
    // caller has already Verify()'d the total chunk size. read() advances
    // m_position, so sequential calls walk the fields. (bone-data leaf.)
    uint32_t readU32At(ChunkReader& r)
    {
        uint32_t v = 0;
        r.read(&v, sizeof(v));
        return letohl(v);
    }

    // Read one already-entered material param chunk (its Kind from the chunk
    // id) by walking its name(1) / value(2) mini-chunks.
    AloShaderParam ReadParam(ChunkReader& r, AloShaderParam::Kind kind)
    {
        VerifyDataLeaf(r);   // param chunk is a data leaf; nextMini() needs m_size >= 0 (#487)
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

    void ReadAndAppendParam(ChunkReader& r, AloSubMesh& sm,
                            AloShaderParam::Kind kind, size_t& shaderParamsTotal)
    {
        // Count every recognized leaf before parsing/pushing it. Duplicate
        // names still consume parser work and independent vector entries.
        Verify(shaderParamsTotal < kMaxAloShaderParamsTotal);
        ++shaderParamsTotal;
        sm.params.push_back(ReadParam(r, kind));
    }

    // r is positioned inside a 0x10100 submesh-material container.
    void ReadSubMeshMaterial(ChunkReader& r, AloSubMesh& sm,
                             size_t& shaderParamsTotal)
    {
        VerifyContainer(r);   // #487
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_SHADER_NAME:   sm.shaderName = r.readString(); break;
                case CHUNK_PARAM_INT:     ReadAndAppendParam(r, sm, AloShaderParam::INT,     shaderParamsTotal); break;
                case CHUNK_PARAM_FLOAT:   ReadAndAppendParam(r, sm, AloShaderParam::FLOAT,   shaderParamsTotal); break;
                case CHUNK_PARAM_FLOAT3:  ReadAndAppendParam(r, sm, AloShaderParam::FLOAT3,  shaderParamsTotal); break;
                case CHUNK_PARAM_TEXTURE: ReadAndAppendParam(r, sm, AloShaderParam::TEXTURE, shaderParamsTotal); break;
                case CHUNK_PARAM_FLOAT4:  ReadAndAppendParam(r, sm, AloShaderParam::FLOAT4,  shaderParamsTotal); break;
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
        VerifyContainer(r);   // #487
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
        // FINAL consistency re-check (2026-07 audit, deep fuzz round 16752).
        //
        // CHUNK_VERTEX_NEW and CHUNK_INDICES each validate their blob size
        // against sm.vertexCount / sm.primitiveCount AT THE MOMENT THEY ARE
        // READ. Those counts come from CHUNK_SIZES -- and nothing stopped a
        // SECOND CHUNK_SIZES later in the same sub-mesh from overwriting them
        // after the blobs had already been validated. The result loaded
        // "cleanly" while carrying an index blob sized for the OLD count and a
        // primitiveCount reporting the NEW one; a draw would then read
        // primitiveCount*3 indices out of a shorter buffer. Chunk ORDER, not
        // chunk content, defeated the per-chunk checks.
        //
        // Re-assert both invariants against the FINAL counts, so any ordering
        // or duplication permutation is caught regardless of how it arose.
        if (!sm.rawVertexBytes.empty()
            && (unsigned long long)sm.rawVertexBytes.size()
                   != (unsigned long long)sm.vertexCount * kAloVertexStride)
            throw BadFileException();
        if (!sm.indexBytes.empty()
            && (unsigned long long)sm.indexBytes.size()
                   != (unsigned long long)sm.primitiveCount * 3 * kIndexSize)
            throw BadFileException();

        // A modern sub-mesh must have produced a vertex blob; the only
        // legitimate empty case is the legacy 0x10005 path.
        if (sm.rawVertexBytes.empty() && !sawOldVertex)
            throw BadFileException();
    }

    // r is positioned inside a 0x0400 mesh container. Per the format, each
    // sub-mesh's 0x10100 material is immediately followed by its 0x10000
    // geometry as a sibling, so geometry attaches to the most recent sub-mesh.
    void ReadMesh(ChunkReader& r, AloMesh& mesh, size_t& subMeshesTotal,
                  size_t& shaderParamsTotal)
    {
        VerifyContainer(r);   // #487
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_MESH_NAME:
                    mesh.name = r.readString();
                    break;
                case CHUNK_MESH_INFO:   // 0x402: per-mesh hidden + collision flags
                {
                    // Read only the leading 40 B (count + bbox + the two flag u32s);
                    // next() skips the reserved tail. TOLERANT: a short/odd chunk
                    // leaves the visible/non-collision defaults (never throws -- this
                    // chunk was skipped wholesale before, so a surprising shape must
                    // not break a model that loaded before).
                    if (r.size() >= kMeshInfo402MinBytes)
                    {
                        unsigned char skip[kMeshInfo402SkipBytes];
                        r.read(skip, sizeof(skip));         // subMeshCount + bbox + unused
                        mesh.hidden    = (readU32At(r) != 0);   // @ data offset 32
                        mesh.collision = (readU32At(r) != 0);   // @ data offset 36
                    }
                    break;
                }
                case CHUNK_SUBMESH_MAT:
                    // Count material containers across the whole model before
                    // allocating their owning AloSubMesh.
                    Verify(subMeshesTotal < kMaxAloSubMeshesTotal);
                    ++subMeshesTotal;
                    mesh.subMeshes.emplace_back();
                    ReadSubMeshMaterial(r, mesh.subMeshes.back(), shaderParamsTotal);
                    break;
                case CHUNK_GEOMETRY:
                    Verify(!mesh.subMeshes.empty());   // geometry without a material is malformed
                    ReadGeometry(r, mesh.subMeshes.back());
                    break;
                default:
                    // Unknown DATA chunks auto-skip; unknown containers skip().
                    if (r.size() < 0) r.skip();
                    break;
            }
        }
    }

    // r is positioned inside a 0x202 bone container. TOLERANT: an unrecognized
    // child or an unexpected bone-data size leaves the field at its default
    // (identity matrix / parentIndex 0) rather than throwing -- these chunks were
    // skipped wholesale before, so a surprising shape must not break a model
    // that loaded before. The consumer flags a degenerate placement.
    void ReadBone(ChunkReader& r, AloBone& bone)
    {
        VerifyContainer(r);   // #487
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            switch (t)
            {
                case CHUNK_BONE_NAME:
                    bone.name = r.readString();
                    break;
                case CHUNK_BONE_DATA:       // 0x205: parentIndex, visible, matrix
                case CHUNK_BONE_DATA_BB:    // 0x206: + billboardMode before the matrix
                {
                    const bool hasBillboard = (t == CHUNK_BONE_DATA_BB);
                    const long expect = hasBillboard ? kBoneData206Bytes : kBoneData205Bytes;
                    if (r.size() == expect)
                    {
                        bone.parentIndex   = readU32At(r);
                        bone.visible       = (readU32At(r) != 0);
                        bone.billboardMode = hasBillboard ? readU32At(r) : 0;
                        r.read(bone.matrix, (long)sizeof(bone.matrix));   // 48 B raw LE floats
                    }
                    // else: unexpected size -> keep defaults; next() skips the chunk.
                    break;
                }
                default:
                    if (r.size() < 0) r.skip();   // unknown container
                    break;
            }
        }
    }

    // r is positioned inside a 0x200 skeleton container. The 0x201 info leaf
    // (and its boneCount) is intentionally ignored -- we trust the actual count
    // of 0x202 children (a stub 0x201 may lie). Unexpected child shapes stay
    // tolerant, but a child-count flood is structurally malformed and rejected.
    void ReadSkeleton(ChunkReader& r, std::vector<AloBone>& bones)
    {
        VerifyContainer(r);   // #487
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            if (t == CHUNK_BONE)
            {
                Verify(bones.size() < kMaxAloBones);
                bones.emplace_back();
                ReadBone(r, bones.back());
            }
            else if (r.size() < 0)
            {
                r.skip();   // unknown container
            }
            // 0x201 info + any unknown data chunk auto-skip on the next next().
        }
    }

    // r is positioned inside a 0x600 connections container. Reads the 0x602
    // object->bone entries; skips 0x601 counts + 0x603/0x604 proxies/dazzles.
    // TOLERANT: a non-4-byte mini is skipped rather than throwing. A flood of
    // connection entries is rejected before growing the vector without bound.
    void ReadConnections(ChunkReader& r, std::vector<AloConnection>& conns)
    {
        VerifyContainer(r);   // #487
        ChunkType t;
        while ((t = r.next()) != -1)
        {
            if (t == CHUNK_CONN_OBJECT)
            {
                Verify(conns.size() < kMaxAloConnections);
                VerifyDataLeaf(r);   // 0x602 is a data leaf; nextMini() needs m_size >= 0 (#487)
                AloConnection c;
                ChunkType mt;
                while ((mt = r.nextMini()) != -1)
                {
                    if (mt == MINI_CONN_OBJECT && r.size() == (long)sizeof(uint32_t))
                        c.objectIndex = readU32(r);
                    else if (mt == MINI_CONN_BONE && r.size() == (long)sizeof(uint32_t))
                        c.boneIndex = readU32(r);
                    // unknown / odd-sized mini auto-skipped by the next nextMini().
                }
                conns.push_back(c);
            }
            else if (r.size() < 0)
            {
                r.skip();   // unknown container (defensive; proxies are data leaves)
            }
            // 0x601 counts + 0x603/0x604 (data leaves) auto-skip.
        }
    }
}

AloModel LoadAloModel(IFile* file)
{
    ChunkReader r(file);
    AloModel model;
    size_t subMeshesTotal = 0;
    size_t shaderParamsTotal = 0;

    ChunkType t;
    while ((t = r.next()) != -1)
    {
        if (t == CHUNK_MESH)
        {
            // Aggregate cap (2026-07 audit). Every 0x0400 container costs a
            // Mesh whether or not it carries a payload, and this loop was bounded
            // only by the file size — unlike the bone and connection loops below,
            // which have had their caps all along.
            Verify(model.meshes.size() < kMaxAloMeshes);
            model.meshes.emplace_back();
            ReadMesh(r, model.meshes.back(), subMeshesTotal, shaderParamsTotal);
        }
        else if (t == CHUNK_SKELETON)
        {
            ReadSkeleton(r, model.bones);          // rigid multi-part placement
        }
        else if (t == CHUNK_CONNECTIONS)
        {
            ReadConnections(r, model.connections); // object->bone bindings
        }
        else if (r.size() < 0)
        {
            // 0x1300 lights + any other unknown container.
            r.skip();
        }
        // Any unexpected root DATA chunk is auto-skipped by the next next().
    }

    if (model.meshes.empty())
        throw WrongFileException();   // not a mesh-bearing .alo

    return model;
}

// --- Sub-mesh classification (shared by ReferenceObjectMesh + GameObjectCatalog) ---

bool AloIsSkinnedVertexFormat(const std::string& vertexFormatName)
{
    return vertexFormatName.rfind("alD3dVertRSkin", 0) == 0 ||
           vertexFormatName.rfind("alD3dVertB4I4", 0) == 0;
}

AloRenderClass AloClassifyShader(const std::string& shaderName)
{
    const char* s = shaderName.c_str();
    auto eq  = [&](const char* n) { return _stricmp(s, n) == 0; };
    auto pre = [&](const char* p) { return _strnicmp(s, p, (int)strlen(p)) == 0; };

    // Scaffolding phases first (exact names) -- never drawn as opaque/transparent.
    if (eq("MeshShadowVolume.fx") || eq("RSkinShadowVolume.fx")) return ALO_RC_SHADOW;
    if (eq("MeshOccludedUnit.fx") || eq("RSkinOccludedUnit.fx")) return ALO_RC_OCCLUDED;
    if (eq("MeshHeat.fx")         || eq("RSkinHeat.fx"))         return ALO_RC_HEAT;

    // MeshShield: Transparent/additive (ONE/ONE) but NOT named MeshAdditive* --
    // the one name-vs-blend mismatch in the FoC corpus; pin it explicitly.
    if (eq("MeshShield.fx")) return ALO_RC_ADDITIVE;

    // Prefix families: additive (ONE/ONE) and alpha (SRCALPHA/INVSRCALPHA).
    if (pre("MeshAdditive") || pre("RSkinAdditive")) return ALO_RC_ADDITIVE;
    if (pre("MeshAlpha")    || pre("RSkinAlpha"))    return ALO_RC_ALPHA;

    // Everything else (MeshGloss/BumpColorize/GlossColorize/SolidColor/Collision/
    // LightVisualize + the RSkin gloss/bump variants) is opaque (AlphaBlendEnable
    // FALSE in its SB block); MeshCollision.fx is opaque flat blue.
    return ALO_RC_OPAQUE;
}

bool AloIsNonVisibleShader(const std::string& shaderName)
{
    // Not drawn in the visible (opaque + transparent) passes. Collision is drawn
    // (opaque blue) so it is deliberately NOT here; Heat is v1-deferred (skipped),
    // so it counts as non-visible until the Heat phase ships.
    const AloRenderClass rc = AloClassifyShader(shaderName);
    return rc == ALO_RC_SHADOW || rc == ALO_RC_OCCLUDED || rc == ALO_RC_HEAT;
}
