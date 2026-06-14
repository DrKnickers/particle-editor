// Reference-object render core implementation. See ReferenceObjectMesh.h.
// Structurally cloned from SkydomeMesh.cpp; diverges in (1) an extended
// transcoder covering the tangent/binormal unit formats, (2) visibility filtering
// (skip 0x402-hidden meshes + skinned + shadow/occluded/heat sub-meshes, keep
// collision = drawn blue), (3) per-sub-mesh rigid bone placement + phase/blend
// classification, and (4) an object-space AABB for the selection box + pick.

#include "ReferenceObjectMesh.h"

#include <cstdio>
#include <cstring>

#include "Effect.h"
#include "managers.h"   // IShaderManager, IFileManager
#include "files.h"      // IFile, ReadAndRelease
#include "exceptions.h" // wexception (Load boundary catch)

namespace
{
    template <typename T> void relptr(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // Runtime vertex layouts the unit shaders consume. On-disk is always the
    // fixed 144B MASTER_VERTEX (pos@0, normal@12, uv0@24, tangent@56, binormal@68,
    // color float4@80 -- AloModel.h); each format selects which fields the decl
    // reads. The "U3U3" suffixes are TANGENT0 + BINORMAL0 (FLOAT3), NOT extra UVs
    // -- verified against MeshBumpColorize's VS_INPUT_MESH (BumpColorize.fxh).
    //   alD3dVertN        POS NORMAL                              (24B)  collision/shadow
    //   alD3dVertNU2      POS NORMAL UV0                          (32B)
    //   alD3dVertNU2C     POS NORMAL UV0 COLOR                    (36B)
    //   alD3dVertNU2U3    POS NORMAL UV0 TANGENT                  (44B)
    //   alD3dVertNU2U3U3  POS NORMAL UV0 TANGENT BINORMAL         (56B)  <- the unit plurality
    //   alD3dVertNU2U3U3C POS NORMAL UV0 TANGENT BINORMAL COLOR   (60B)
    enum RuntimeFormat { RF_N, RF_NU2, RF_NU2C, RF_NU2U3, RF_NU2U3U3, RF_NU2U3U3C };

    // An RSkin (1-bone "rigid skinning") sub-mesh: its geometry is identical
    // to the rigid `alD3dVertNU2U3U3...` set, but the VS_INPUT NORMAL is a float4
    // whose .w carries the bone index (`m_skinMatrixArray[Normal.w]`). We render
    // these in BIND POSE with a uniform `objectWorld` skin palette (so the index is
    // irrelevant -> .w = 0), which only needs the NORMAL widened to float4 (+4 bytes,
    // shifting every later field). B4I4 (4-bone) uses a different layout -> still
    // deferred (skipped in Load). isSkinned below == "is RSkin" here.
    bool isRSkinFormat(const std::string& name)  { return name.rfind("alD3dVertRSkin", 0) == 0; }

    RuntimeFormat classifyFormat(const std::string& name)
    {
        // Strip an RSkin / B4I4 prefix so the geometry suffix classifies like its
        // rigid counterpart (e.g. "alD3dVertRSkinNU2U3U3" -> RF_NU2U3U3).
        std::string g = name;
        if      (g.rfind("alD3dVertRSkin", 0) == 0) g = "alD3dVert" + g.substr(14);
        else if (g.rfind("alD3dVertB4I4",  0) == 0) g = "alD3dVert" + g.substr(13);
        if (g == "alD3dVertN")         return RF_N;
        if (g == "alD3dVertNU2")       return RF_NU2;
        if (g == "alD3dVertNU2C")      return RF_NU2C;
        if (g == "alD3dVertNU2U3")     return RF_NU2U3;
        if (g == "alD3dVertNU2U3U3")   return RF_NU2U3U3;
        if (g == "alD3dVertNU2U3U3C")  return RF_NU2U3U3C;
        return RF_NU2C;   // unknown rigid format -> the safe pos/normal/uv/color subset
    }

    // Skinned shifts every field after the NORMAL by +4 (float3 normal -> float4).
    uint32_t skinShift(bool skinned)  { return skinned ? 4u : 0u; }

    uint32_t strideFor(RuntimeFormat f, bool skinned)
    {
        uint32_t base;
        switch (f)
        {
            case RF_N:         base = 24; break;
            case RF_NU2:       base = 32; break;
            case RF_NU2C:      base = 36; break;
            case RF_NU2U3:     base = 44; break;
            case RF_NU2U3U3:   base = 56; break;
            default:           base = 60; break;   // RF_NU2U3U3C
        }
        return base + skinShift(skinned);
    }

    bool hasUV(RuntimeFormat f)       { return f != RF_N; }
    bool hasColor(RuntimeFormat f)    { return f == RF_NU2C || f == RF_NU2U3U3C; }
    bool hasTangent(RuntimeFormat f)  { return f == RF_NU2U3 || f == RF_NU2U3U3 || f == RF_NU2U3U3C; }
    bool hasBinormal(RuntimeFormat f) { return f == RF_NU2U3U3 || f == RF_NU2U3U3C; }

    // Runtime offsets of the optional fields (after pos@0 + normal@12; +4 if skinned).
    uint32_t uvOff(bool skinned)                  { return 24 + skinShift(skinned); }
    uint32_t tangentOff(bool skinned)             { return 32 + skinShift(skinned); }   // after uv0
    uint32_t binormalOff(bool skinned)            { return 44 + skinShift(skinned); }   // after tangent
    uint32_t colorOff(RuntimeFormat f, bool skinned) { return (hasBinormal(f) ? 56u : 32u) + skinShift(skinned); }

    void declElementsFor(RuntimeFormat f, bool skinned, std::vector<D3DVERTEXELEMENT9>& out)
    {
        auto push = [&](WORD off, BYTE type, BYTE usage, BYTE idx) {
            const D3DVERTEXELEMENT9 e = { 0, off, type, D3DDECLMETHOD_DEFAULT, usage, idx };
            out.push_back(e);
        };
        push(0,  D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
        // RSkin: NORMAL is float4 (xyz + bone index in .w).
        push(12, skinned ? D3DDECLTYPE_FLOAT4 : D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
        if (hasUV(f))       push((WORD)uvOff(skinned),         D3DDECLTYPE_FLOAT2,   D3DDECLUSAGE_TEXCOORD, 0);
        if (hasTangent(f))  push((WORD)tangentOff(skinned),    D3DDECLTYPE_FLOAT3,   D3DDECLUSAGE_TANGENT,  0);
        if (hasBinormal(f)) push((WORD)binormalOff(skinned),   D3DDECLTYPE_FLOAT3,   D3DDECLUSAGE_BINORMAL, 0);
        if (hasColor(f))    push((WORD)colorOff(f, skinned),   D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR,    0);
        const D3DVERTEXELEMENT9 end = D3DDECL_END();
        out.push_back(end);
    }

    // Transcode one 144B on-disk vertex into the compact runtime record.
    void transcodeVertex(RuntimeFormat f, bool skinned, const unsigned char* src, unsigned char* dst)
    {
        memcpy(dst + 0,  src + 0,  12);                       // POSITION @0
        memcpy(dst + 12, src + 12, 12);                       // NORMAL.xyz @12
        if (skinned)                                          // NORMAL.w = bone index; uniform
        {                                                     // bind-pose palette -> any index ok -> 0.
            const float zero = 0.0f;
            memcpy(dst + 24, &zero, 4);
        }
        if (hasUV(f))      memcpy(dst + uvOff(skinned),       src + 24, 8);   // TEXCOORD0 (on-disk uv0@24)
        if (hasTangent(f)) memcpy(dst + tangentOff(skinned),  src + 56, 12);  // TANGENT0  (on-disk @56)
        if (hasBinormal(f))memcpy(dst + binormalOff(skinned), src + 68, 12);  // BINORMAL0 (on-disk @68)
        if (hasColor(f))
        {
            float c[4];
            memcpy(c, src + kAloColorOffset, 16);             // on-disk color float4 RGBA @80
            D3DCOLOR col = D3DCOLOR_COLORVALUE(c[0], c[1], c[2], c[3]);
            memcpy(dst + colorOff(f, skinned), &col, 4);
        }
    }

    // v1 deferral detectors live on the AloModel pure-data leaf
    // (AloIsSkinnedVertexFormat / AloIsNonVisibleShader) so GameObjectCatalog's
    // picker grey-out stays in lockstep with what this renderer skips.

    // Texture resolution: bare leaf name -> Data\Art\Textures\, with the
    // engine's .tga->.dds fallback (game ships compiled .dds). Mirrors
    // SkydomeMesh::loadMaterialTexture (#165).
    IDirect3DTexture9* loadTextureExact(IDirect3DDevice9* dev, IFileManager& fm, const std::string& path)
    {
        IFile* file = fm.getFile(path);
        if (!file) return nullptr;
        std::vector<unsigned char> bytes;
        try { bytes = ReadAndRelease(file); }
        catch (const ReadException&) { return nullptr; }   // missing/short read -> untextured
        if (bytes.empty()) return nullptr;
        IDirect3DTexture9* tex = nullptr;
        HRESULT hr = D3DXCreateTextureFromFileInMemory(dev, bytes.data(), (UINT)bytes.size(), &tex);
        return SUCCEEDED(hr) ? tex : nullptr;
    }
    IDirect3DTexture9* loadMaterialTexture(IDirect3DDevice9* dev, IFileManager& fm, const std::string& bareName)
    {
        if (bareName.empty()) return nullptr;
        std::string asDds = bareName;
        const size_t dot = asDds.rfind('.');
        if (dot != std::string::npos) asDds = asDds.substr(0, dot) + ".dds";
        const std::string candidates[] = {
            "Data\\Art\\Textures\\" + bareName,
            "Data\\Art\\Textures\\" + asDds,
            bareName,
            asDds,
        };
        for (const std::string& c : candidates)
        {
            IDirect3DTexture9* tex = loadTextureExact(dev, fm, c);
            if (tex) return tex;
        }
        return nullptr;
    }

    // Build a bone's local D3DXMATRIX from the 12 on-disk floats (column-major:
    // col0@[0..3], col1@[4..7], col2@[8..11]). Row i (0..2) = (m[i], m[i+4],
    // m[i+8], 0); row 3 = translation (m[3], m[7], m[11], 1). NO transpose
    // (the editor is row-vector RH Z-up, same as the .alo object space).
    D3DXMATRIX boneLocalMatrix(const float m[12])
    {
        D3DXMATRIX r;
        r._11 = m[0]; r._12 = m[4]; r._13 = m[8];  r._14 = 0.0f;
        r._21 = m[1]; r._22 = m[5]; r._23 = m[9];  r._24 = 0.0f;
        r._31 = m[2]; r._32 = m[6]; r._33 = m[10]; r._34 = 0.0f;
        r._41 = m[3]; r._42 = m[7]; r._43 = m[11]; r._44 = 1.0f;
        return r;
    }

    // Accumulate each bone's object-space matrix up the parent chain:
    //   obj[i] = local[i] * obj[parent[i]]   (row-vector: child * parent)
    // parentIndex 0xFFFFFFFF (or out of range) = root. Parents precede children
    // (the .alo guarantees parent < self), so one forward pass suffices.
    void computeBoneObjectMatrices(const std::vector<AloBone>& bones,
                                   std::vector<D3DXMATRIX>& out)
    {
        const size_t n = bones.size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            D3DXMATRIX local = boneLocalMatrix(bones[i].matrix);
            const uint32_t p = bones[i].parentIndex;
            if (p < i)   // valid earlier parent (also excludes 0xFFFFFFFF roots)
                D3DXMatrixMultiply(&out[i], &local, &out[p]);
            else
                out[i] = local;
        }
    }
}

ReferenceObjectMesh::~ReferenceObjectMesh()
{
    ReleaseGpuBuffers();
    ReleaseEffects();
    ReleaseDecls();
}

bool ReferenceObjectMesh::HasResolved() const
{
    for (const RefSubMeshGpu& s : m_subMeshes)
        if (s.effect != nullptr) return true;
    return false;
}

void ReferenceObjectMesh::Clear()
{
    ReleaseGpuBuffers();
    ReleaseEffects();
    ReleaseDecls();
    m_subMeshes.clear();
    m_skippedSkinned = false;
    m_boundMin = m_boundMax = D3DXVECTOR3(0, 0, 0);
    m_hasBounds = false;
}

bool ReferenceObjectMesh::Load(IFileManager& fm, const std::string& aloPath)
{
    Clear();

    IFile* file = fm.getFile(aloPath);
    if (!file) return false;

    AloModel model;
    bool ok = false;
    try { model = LoadAloModel(file); ok = true; }
    catch (const wexception&) { ok = false; }
    file->Release();
    if (!ok) return false;

    // Bone object-space matrices + a fast mesh-ordinal -> bone-index lookup from
    // the connections (objectIndex == mesh ordinal: meshes precede lights, and
    // we import meshes only). No skeleton -> every mesh places at identity.
    std::vector<D3DXMATRIX> boneObj;
    computeBoneObjectMatrices(model.bones, boneObj);

    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
    {
        const AloMesh& mesh = model.meshes[meshIdx];

        // Respect the per-mesh 0x402 hidden flag: a hidden mesh is not drawn
        // (game/alo-viewer gate visibility here). Removes hidden collision boxes +
        // the hidden shield/heat distortion meshes that were warping the view.
        if (mesh.hidden)
            continue;

        // This mesh's rigid placement = its bone's object-space matrix (identity
        // if no skeleton / no connection / out-of-range bone).
        D3DXMATRIX placement;
        D3DXMatrixIdentity(&placement);
        for (const AloConnection& c : model.connections)
        {
            if (c.objectIndex == meshIdx && c.boneIndex < boneObj.size())
            {
                placement = boneObj[c.boneIndex];
                break;
            }
        }

        for (const AloSubMesh& sm : mesh.subMeshes)
        {
            if (sm.rawVertexBytes.empty() || sm.vertexCount == 0 || sm.primitiveCount == 0)
                continue;

            // Phase/blend class. Shadow + occluded are never visible geometry.
            // Heat is a deferred two-stage scene-composite (D3) -> log + skip in v1.
            // Opaque (incl. MeshCollision = flat blue) + additive/alpha are KEPT and
            // drawn (the latter two in the transparent pass).
            const AloRenderClass rc = AloClassifyShader(sm.shaderName);
            if (rc == ALO_RC_SHADOW || rc == ALO_RC_OCCLUDED)
                continue;
            if (rc == ALO_RC_HEAT)
            {
#ifndef NDEBUG
                fprintf(stderr, "[RefObj] skip Heat sub-mesh \"%s\" -- faithful Heat phase deferred (S46/D3)\n",
                        sm.shaderName.c_str());
#endif
                continue;
            }
            // RSkin (1-bone) renders in BIND POSE (model-space verts +
            // uniform objectWorld skin palette). B4I4 (4-bone) still deferred.
            const bool skinned = isRSkinFormat(sm.vertexFormatName);
            if (AloIsSkinnedVertexFormat(sm.vertexFormatName) && !skinned)
            {
                m_skippedSkinned = true;
                continue;
            }

            const RuntimeFormat f = classifyFormat(sm.vertexFormatName);
            const uint32_t stride = strideFor(f, skinned);

            RefSubMeshGpu gpu;
            gpu.renderClass      = rc;
            gpu.skinned          = skinned;
            gpu.shaderName       = sm.shaderName;
            gpu.vertexFormatName = sm.vertexFormatName;
            gpu.params           = sm.params;
            gpu.stride           = stride;
            gpu.vertexCount      = sm.vertexCount;
            gpu.primitiveCount   = sm.primitiveCount;
            gpu.indexBytes       = sm.indexBytes;
            // Skinned verts are model-space bind pose -> NO per-mesh bone placement
            // (the uniform objectWorld skin palette positions them at draw). Rigid
            // verts are bone-local -> placed by their mesh's bone matrix.
            if (skinned) D3DXMatrixIdentity(&gpu.placement);
            else         gpu.placement = placement;

            gpu.vertexBytes.resize((size_t)stride * sm.vertexCount);
            for (uint32_t v = 0; v < sm.vertexCount; ++v)
            {
                transcodeVertex(f, skinned,
                                sm.rawVertexBytes.data() + (size_t)v * kAloVertexStride,
                                gpu.vertexBytes.data()   + (size_t)v * stride);
            }
            m_subMeshes.push_back(std::move(gpu));
        }
    }

    // Object-space AABB over the kept (drawn) geometry -- the selection box
    // + the click-to-select hit target, so the drawn box == the pickable region.
    // Position is float3 @0 in every runtime format; place each vertex by its
    // sub-mesh bone matrix to match exactly what RenderReferenceObject draws.
    for (const RefSubMeshGpu& g : m_subMeshes)
    {
        const unsigned char* vb = g.vertexBytes.data();
        for (uint32_t v = 0; v < g.vertexCount; ++v)
        {
            const float* p = reinterpret_cast<const float*>(vb + (size_t)v * g.stride);
            D3DXVECTOR3 pos(p[0], p[1], p[2]), placed;
            D3DXVec3TransformCoord(&placed, &pos, &g.placement);
            if (!m_hasBounds) { m_boundMin = m_boundMax = placed; m_hasBounds = true; }
            else
            {
                if (placed.x < m_boundMin.x) m_boundMin.x = placed.x;
                if (placed.y < m_boundMin.y) m_boundMin.y = placed.y;
                if (placed.z < m_boundMin.z) m_boundMin.z = placed.z;
                if (placed.x > m_boundMax.x) m_boundMax.x = placed.x;
                if (placed.y > m_boundMax.y) m_boundMax.y = placed.y;
                if (placed.z > m_boundMax.z) m_boundMax.z = placed.z;
            }
        }
    }

    return !m_subMeshes.empty();
}

bool ReferenceObjectMesh::GetBoundingBox(D3DXVECTOR3& outMin, D3DXVECTOR3& outMax) const
{
    if (!m_hasBounds) return false;
    outMin = m_boundMin;
    outMax = m_boundMax;
    return true;
}

bool ReferenceObjectMesh::Resolve(IShaderManager& sm, IDirect3DDevice9* dev)
{
    if (dev == nullptr) return false;

    bool anyResolved = false;
    for (RefSubMeshGpu& gpu : m_subMeshes)
    {
        relptr(gpu.effect);
        // getShader (a missing/uncompilable .fxo) and GetOrCreateDecl can
        // throw a wexception; the env hook + the picker path (SetReferenceObject)
        // call Resolve with NO try/catch, so an unresolvable sub-mesh would
        // std::terminate the whole editor (a Mod capital ship whose shader
        // the editor can't load is enough). Degrade THIS sub-mesh to unresolved
        // (skipped at draw) and keep going, matching the nullptr-skip below.
        try
        {
            gpu.effect = sm.getShader(dev, gpu.shaderName);   // ext-tolerant .fx -> .FXO; cached + AddRef'd
            if (gpu.effect == nullptr) continue;

            gpu.decl = GetOrCreateDecl(dev, gpu.vertexFormatName);

            gpu.matHandles.assign(gpu.params.size(), nullptr);
            ID3DXEffect* fx = gpu.effect->getD3DEffect();      // AddRef'd
            for (size_t i = 0; i < gpu.params.size(); ++i)
                gpu.matHandles[i] = fx->GetParameterByName(nullptr, gpu.params[i].name.c_str());
#ifndef NDEBUG
            D3DXHANDLE cur = fx->GetCurrentTechnique();
            D3DXTECHNIQUE_DESC td; memset(&td, 0, sizeof(td));
            if (cur) fx->GetTechniqueDesc(cur, &td);
            fprintf(stderr, "[RefObj] resolved %-20s fmt=%-18s technique=%s\n",
                    gpu.shaderName.c_str(), gpu.vertexFormatName.c_str(), td.Name ? td.Name : "(none)");
#endif
            fx->Release();
            anyResolved = true;
        }
        catch (wexception& we)
        {
#ifndef NDEBUG
            fwprintf(stderr, L"[RefObj] resolve FAILED %hs / %hs: %ls -- sub-mesh skipped\n",
                     gpu.shaderName.c_str(), gpu.vertexFormatName.c_str(), we.what());
#else
            (void)we;   // Release (/WX): `we` is only read by the NDEBUG log above
#endif
            relptr(gpu.effect);   // leave effect/decl null -> skipped at draw
        }
    }
    return anyResolved;
}

void ReferenceObjectMesh::CreateBuffers(IDirect3DDevice9* dev, IFileManager& fm)
{
    if (dev == nullptr) return;

    for (RefSubMeshGpu& gpu : m_subMeshes)
    {
        if (gpu.effect == nullptr) continue;

        relptr(gpu.vb);
        relptr(gpu.ib);
        for (IDirect3DTexture9*& t : gpu.matTextures) relptr(t);
        gpu.matTextures.clear();

        const UINT vbBytes = (UINT)gpu.vertexBytes.size();
        const UINT ibBytes = (UINT)gpu.indexBytes.size();
        if (vbBytes == 0 || ibBytes == 0) continue;

        if (SUCCEEDED(dev->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, 0,
                                              D3DPOOL_DEFAULT, &gpu.vb, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(gpu.vb->Lock(0, 0, &p, 0))) { memcpy(p, gpu.vertexBytes.data(), vbBytes); gpu.vb->Unlock(); }
            else relptr(gpu.vb);
        }
        if (SUCCEEDED(dev->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                             D3DPOOL_DEFAULT, &gpu.ib, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(gpu.ib->Lock(0, 0, &p, 0))) { memcpy(p, gpu.indexBytes.data(), ibBytes); gpu.ib->Unlock(); }
            else relptr(gpu.ib);
        }

        gpu.matTextures.assign(gpu.params.size(), nullptr);
        for (size_t i = 0; i < gpu.params.size(); ++i)
            if (gpu.params[i].kind == AloShaderParam::TEXTURE)
                gpu.matTextures[i] = loadMaterialTexture(dev, fm, gpu.params[i].tex);
    }
}

void ReferenceObjectMesh::OnLostDevice()
{
    for (RefSubMeshGpu& gpu : m_subMeshes)
    {
        relptr(gpu.vb);
        relptr(gpu.ib);
        for (IDirect3DTexture9*& t : gpu.matTextures) relptr(t);
        gpu.matTextures.clear();
        if (gpu.effect) gpu.effect->OnLostDevice();
    }
}

void ReferenceObjectMesh::OnResetEffects()
{
    for (RefSubMeshGpu& gpu : m_subMeshes)
        if (gpu.effect) gpu.effect->OnResetDevice();
}

void ReferenceObjectMesh::ReleaseGpuBuffers()
{
    for (RefSubMeshGpu& gpu : m_subMeshes)
    {
        relptr(gpu.vb);
        relptr(gpu.ib);
        for (IDirect3DTexture9*& t : gpu.matTextures) relptr(t);
        gpu.matTextures.clear();
    }
}

void ReferenceObjectMesh::ReleaseEffects()
{
    for (RefSubMeshGpu& gpu : m_subMeshes)
        relptr(gpu.effect);
}

void ReferenceObjectMesh::ReleaseDecls()
{
    for (std::map<std::string, IDirect3DVertexDeclaration9*>::iterator it = m_decls.begin();
         it != m_decls.end(); ++it)
        if (it->second) it->second->Release();
    m_decls.clear();
}

IDirect3DVertexDeclaration9* ReferenceObjectMesh::GetOrCreateDecl(IDirect3DDevice9* dev,
                                                                  const std::string& formatName)
{
    std::map<std::string, IDirect3DVertexDeclaration9*>::iterator it = m_decls.find(formatName);
    if (it != m_decls.end()) return it->second;

    std::vector<D3DVERTEXELEMENT9> elems;
    declElementsFor(classifyFormat(formatName), isRSkinFormat(formatName), elems);

    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->CreateVertexDeclaration(elems.data(), &decl)))
        decl = nullptr;
    m_decls[formatName] = decl;
    return decl;
}
