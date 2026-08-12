// Skydome render core implementation. See SkydomeMesh.h.

#include "SkydomeMesh.h"

#include <cstdio>
#include <cstring>

#include "Effect.h"
#include "managers.h"   // IShaderManager, IFileManager
#include "files.h"      // IFile, ReadAndReleaseCapped
#include "ResourceLimits.h" // kMaxTextureAssetBytes
#include "exceptions.h" // wexception (Load boundary catch)
#include "AssetPathSafety.h"

namespace
{
    // Null-safe release used for both Effect* (RefCounted) and IDirect3D*9.
    template <typename T> void relptr(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // The runtime vertex layouts the dome shaders consume. The on-disk record is
    // always a 144B MASTER_VERTEX (AloModel); we transcode the fields each format
    // declares into a compact interleaved buffer matching the decl. Canonical
    // prefix-superset order (name: N=+Normal, U2=+UV float2, C=+Color); verified
    // against max2alamo's shader->format table + the shader VS_INPUT structs:
    //   alD3dVertN    POS@0 NORMAL@12                  (24B)  no-UV: shadow/collision/solid
    //   alD3dVertNU2  POS@0 NORMAL@12 UV@24            (32B)  MeshGloss/MeshAdditive/MeshAlpha
    //   alD3dVertNU2C POS@0 NORMAL@12 UV@24 COLOR@32   (36B)  Skydome.fx
    // (On-disk source offsets: pos@0, normal@12, uv0@24, color float4@80 -- AloModel.h.)
    enum RuntimeFormat { RF_N, RF_NU2, RF_NU2C };

    RuntimeFormat classifyFormat(const std::string& name)
    {
        if (name == "alD3dVertN")   return RF_N;     // Pos+Normal (no UV)
        if (name == "alD3dVertNU2") return RF_NU2;   // Pos+Normal+UV
        return RF_NU2C;                              // Pos+Normal+UV+Color: Skydome.fx + unknown fallback
    }

    uint32_t strideFor(RuntimeFormat f)
    {
        switch (f) { case RF_N: return 24; case RF_NU2: return 32; default: return 36; }
    }

    // Append the decl elements for `f` to `out` (stream 0, offsets matching the
    // transcode below). Caller appends D3DDECL_END(). Every dome format carries a
    // normal; UV and color are appended in canonical order.
    void declElementsFor(RuntimeFormat f, std::vector<D3DVERTEXELEMENT9>& out)
    {
        const D3DVERTEXELEMENT9 pos  = { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 };
        const D3DVERTEXELEMENT9 norm = { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 };
        out.push_back(pos);
        out.push_back(norm);
        if (f == RF_NU2 || f == RF_NU2C)
        {
            const D3DVERTEXELEMENT9 uv = { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
            out.push_back(uv);
        }
        if (f == RF_NU2C)
        {
            const D3DVERTEXELEMENT9 col = { 0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
            out.push_back(col);
        }
        const D3DVERTEXELEMENT9 end = D3DDECL_END();
        out.push_back(end);
    }

    // Transcode one 144B on-disk vertex (`src`) into the compact runtime record
    // (`dst`, size strideFor(f)). Field offsets per AloModel.h / max2alamo.
    void transcodeVertex(RuntimeFormat f, const unsigned char* src, unsigned char* dst)
    {
        memcpy(dst + 0,  src + 0,  12);           // POSITION float3 @0
        memcpy(dst + 12, src + 12, 12);           // NORMAL   float3 @12
        if (f == RF_NU2 || f == RF_NU2C)
            memcpy(dst + 24, src + 24, 8);        // TEXCOORD0 float2 @24
        if (f == RF_NU2C)
        {
            float c[4];
            memcpy(c, src + kAloColorOffset, 16); // on-disk COLOR float4 RGBA @80
            D3DCOLOR col = D3DCOLOR_COLORVALUE(c[0], c[1], c[2], c[3]);
            memcpy(dst + 32, &col, 4);            // D3DCOLOR @32
        }
    }

    // Load a texture from an exact FileManager path. NULL on miss / read error.
    IDirect3DTexture9* loadTextureExact(IDirect3DDevice9* dev, IFileManager& fm, const std::string& path)
    {
        IFile* file = fm.getFile(path);
        if (!file) return nullptr;
        std::vector<unsigned char> bytes;
        try { bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes); }
        catch (const ReadException&) { return nullptr; }
        if (bytes.empty()) return nullptr;
        IDirect3DTexture9* tex = nullptr;
        HRESULT hr = D3DXCreateTextureFromFileInMemory(dev, bytes.data(), (UINT)bytes.size(), &tex);
        return SUCCEEDED(hr) ? tex : nullptr;
    }

    // .alo material textures are bare leaf names; the MEG CRC-matches the full
    // Data\Art\Textures\ path. Try that first, then the bare name (loose mod
    // files). Mirrors Engine::LoadTextureViaFileManager + the curated-slot prefix.
    //
    // Extension fallback: the .alo names the SOURCE texture (e.g.
    // "W_SkyBlue_clear.tga") but the packed game ships the COMPILED ".dds" -- so
    // every candidate is also tried with the extension swapped to .dds, mirroring
    // the engine's TextureManager::getTexture (main.cpp: try as-named, then swap to
    // ".DDS"). Lowercase ".dds" here is fine: case is irrelevant on both resolution
    // paths -- MegaFile::getFile uppercases the path before the CRC match
    // (MegaFiles.cpp), and the loose-disk lookup is NTFS case-insensitive
    // (managers.cpp). Without this every dome texture misses and the dome draws black.
    IDirect3DTexture9* loadMaterialTexture(IDirect3DDevice9* dev, IFileManager& fm, const std::string& bareName)
    {
        for (const std::string& c : SafeTextureCandidates(bareName))
        {
            IDirect3DTexture9* tex = loadTextureExact(dev, fm, c);
            if (tex) return tex;
        }
        return nullptr;
    }

    // Build a bone's local D3DXMATRIX from the 12 on-disk floats (column-major:
    // col0@[0..3], col1@[4..7], col2@[8..11]); row 3 = translation. Mirrors
    // ReferenceObjectMesh::boneLocalMatrix (same row-vector RH Z-up object space).
    D3DXMATRIX boneLocalMatrix(const float m[12])
    {
        D3DXMATRIX r;
        r._11 = m[0]; r._12 = m[4]; r._13 = m[8];  r._14 = 0.0f;
        r._21 = m[1]; r._22 = m[5]; r._23 = m[9];  r._24 = 0.0f;
        r._31 = m[2]; r._32 = m[6]; r._33 = m[10]; r._34 = 0.0f;
        r._41 = m[3]; r._42 = m[7]; r._43 = m[11]; r._44 = 1.0f;
        return r;
    }

    // Accumulate each bone's object-space matrix up the parent chain (obj[i] =
    // local[i] * obj[parent]); parents precede children, so one forward pass
    // suffices. Mirrors ReferenceObjectMesh::computeBoneObjectMatrices.
    void computeBoneObjectMatrices(const std::vector<AloBone>& bones,
                                   std::vector<D3DXMATRIX>& out)
    {
        const size_t n = bones.size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            D3DXMATRIX local = boneLocalMatrix(bones[i].matrix);
            const uint32_t p = bones[i].parentIndex;
            if (p < i) D3DXMatrixMultiply(&out[i], &local, &out[p]);
            else       out[i] = local;
        }
    }
}

SkydomeMesh::~SkydomeMesh()
{
    ReleaseGpuBuffers();
    ReleaseEffects();
    ReleaseDecls();
}

bool SkydomeMesh::HasResolved() const
{
    for (const SubMeshGpu& s : m_subMeshes)
        if (s.effect != nullptr) return true;
    return false;
}

// Are there LIVE GPU buffers behind this mesh right now? (2026-07 audit.)
//
// HasResolved above answers a different question — "did a shader bind?" — and
// every existing skydome assertion is of that bookkeeping kind: which slot is
// selected, what the DTO says the status is. None of them looks at whether the
// device actually holds anything, so deleting CreateBuffers left the whole suite
// green and the viewport black. This is the predicate that cannot be true
// without real resources: the DEFAULT-pool VB and IB must both exist AND the
// sub-mesh must have indices to draw.
//
// Deliberately ANY rather than ALL: a sub-mesh whose effect never resolved is
// skipped at draw by design (per-sub-mesh degrade), so requiring every sub-mesh
// to have buffers would report false for a dome that renders perfectly well.
bool SkydomeMesh::HasGpuBuffers() const
{
    for (const SubMeshGpu& s : m_subMeshes)
        if (s.vb != nullptr && s.ib != nullptr && s.primitiveCount > 0) return true;
    return false;
}

void SkydomeMesh::Clear()
{
    ReleaseGpuBuffers();
    ReleaseEffects();
    ReleaseDecls();
    m_subMeshes.clear();
}

bool SkydomeMesh::Load(IFileManager& fm, const std::string& aloPath)
{
    Clear();   // replace semantics: tear down any prior contents

    IFile* file = fm.getFile(aloPath);
    if (!file) return false;

    AloModel model;
    bool ok = false;
    try { model = LoadAloModel(file); ok = true; }   // AddRef/Releases internally
    catch (const wexception&) { ok = false; }        // malformed / truncated / wrong-format
    file->Release();                                 // release our getFile ref
    if (!ok) return false;

    // Bone object-space matrices + per-mesh placement. The vanilla star domes
    // carry the dome on a near-identity bone and a SUN on a 0x206 billboard bone
    // (translated off-centre); the connections map mesh ordinal -> bone. Without
    // this placement the sun quad draws at the dome centre, un-billboarded, and
    // its flat additive geometry collapses to a 1px white line edge-on.
    std::vector<D3DXMATRIX> boneObj;
    computeBoneObjectMatrices(model.bones, boneObj);

    // Flatten every sub-mesh of every 0x400 mesh into the GPU list, transcoding
    // the raw 144B vertices into the compact runtime layout. A 0x10005 (legacy)
    // sub-mesh has empty rawVertexBytes (AloModel drops it) -> skip it here.
    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx)
    {
        const AloMesh& mesh = model.meshes[meshIdx];

        // This mesh's object-space placement + billboard flag = its 0x602
        // connection bone (identity / no-billboard if absent or out of range).
        D3DXMATRIX placement; D3DXMatrixIdentity(&placement);
        uint32_t   billboardMode = 0;
        for (const AloConnection& c : model.connections)
        {
            if (c.objectIndex != meshIdx) continue;
            if (c.boneIndex < boneObj.size())        placement     = boneObj[c.boneIndex];
            if (c.boneIndex < model.bones.size())    billboardMode = model.bones[c.boneIndex].billboardMode;
            break;
        }

        for (const AloSubMesh& sm : mesh.subMeshes)
        {
            if (sm.rawVertexBytes.empty() || sm.vertexCount == 0 || sm.primitiveCount == 0)
                continue;

            const RuntimeFormat f = classifyFormat(sm.vertexFormatName);
            const uint32_t stride = strideFor(f);

            SubMeshGpu gpu;
            gpu.shaderName       = sm.shaderName;
            gpu.vertexFormatName = sm.vertexFormatName;
            gpu.params           = sm.params;
            gpu.stride           = stride;
            gpu.vertexCount      = sm.vertexCount;
            gpu.primitiveCount   = sm.primitiveCount;
            gpu.indexBytes       = sm.indexBytes;
            gpu.placement        = placement;
            gpu.billboardMode    = billboardMode;

            gpu.vertexBytes.resize((size_t)stride * sm.vertexCount);
            for (uint32_t v = 0; v < sm.vertexCount; ++v)
            {
                transcodeVertex(f,
                                sm.rawVertexBytes.data() + (size_t)v * kAloVertexStride,
                                gpu.vertexBytes.data()   + (size_t)v * stride);
            }
            // The dome's authored UVs are uploaded verbatim -- faithful to the asset.
            // A tiled UV-sphere dome (e.g. the vanilla space starfields) bakes a
            // closure-meridian seam; we render it as the game does. A UV re-map was
            // tried (#247) and removed: any lat-long re-projection that erases the seam
            // introduces a pole singularity + aspect distortion + a tiling tradeoff that
            // the non-lat-long authored unwrap avoids -- so faithful is the higher
            // fidelity choice (investigation: docs/superpowers/specs/2026-06-23-skydome-seam-faithful.md).

#ifndef NDEBUG
            if (m_subMeshes.empty() && f == RF_NU2C && sm.vertexCount > 0)
            {
                float c[4];
                memcpy(c, sm.rawVertexBytes.data() + kAloColorOffset, 16);
                fprintf(stderr, "[AloVtx] %s sub0 fmt=%s v=%u p=%u color0=(%.3f,%.3f,%.3f,%.3f)\n",
                        aloPath.c_str(), sm.vertexFormatName.c_str(), sm.vertexCount,
                        sm.primitiveCount, c[0], c[1], c[2], c[3]);
            }
#endif
            m_subMeshes.push_back(std::move(gpu));
        }
    }

    return !m_subMeshes.empty();
}

bool SkydomeMesh::Resolve(IShaderManager& sm, IDirect3DDevice9* dev)
{
    if (dev == nullptr) return false;

    bool anyResolved = false;
    for (SubMeshGpu& gpu : m_subMeshes)
    {
        // Drop+reacquire so a mod-switch (ShaderManager::Clear already ran) picks
        // up the new mod's .fxo rather than a stale held ref.
        relptr(gpu.effect);

        // NOTE: on a total miss ShaderManager::getShader returns its default
        // placeholder Effect (AddRef'd, non-NULL), NOT NULL -- so a genuinely
        // absent shader resolves to the placeholder and renders with placeholder
        // semantics rather than being skipped. Vanilla dome shaders (Skydome.fx /
        // MeshGloss / MeshAdditive) all resolve, so this only bites exotic mods.
        gpu.effect = sm.getShader(dev, gpu.shaderName);   // ext-tolerant .fx -> .FXO; cached + AddRef'd
        if (gpu.effect == nullptr) continue;              // defensive: per-sub-mesh degrade

        gpu.decl = GetOrCreateDecl(dev, gpu.vertexFormatName);

        // Cache material-param handles, index-parallel to gpu.params.
        gpu.matHandles.assign(gpu.params.size(), nullptr);
        ID3DXEffect* fx = gpu.effect->getD3DEffect();      // AddRef'd
        for (size_t i = 0; i < gpu.params.size(); ++i)
            gpu.matHandles[i] = fx->GetParameterByName(nullptr, gpu.params[i].name.c_str());
#ifndef NDEBUG
        D3DXHANDLE cur = fx->GetCurrentTechnique();
        D3DXTECHNIQUE_DESC td; memset(&td, 0, sizeof(td));
        if (cur) fx->GetTechniqueDesc(cur, &td);
        fprintf(stderr, "[SkyDraw] resolved %-16s technique=%s\n",
                gpu.shaderName.c_str(), td.Name ? td.Name : "(none)");
#endif
        fx->Release();

        anyResolved = true;
    }
    return anyResolved;
}

IDirect3DVertexDeclaration9* SkydomeMesh::GetOrCreateDecl(IDirect3DDevice9* dev,
                                                          const std::string& formatName)
{
    std::map<std::string, IDirect3DVertexDeclaration9*>::iterator it = m_decls.find(formatName);
    if (it != m_decls.end()) return it->second;

    std::vector<D3DVERTEXELEMENT9> elems;
    declElementsFor(classifyFormat(formatName), elems);

    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->CreateVertexDeclaration(elems.data(), &decl)))
        decl = nullptr;
    m_decls[formatName] = decl;   // cache even NULL so we don't retry every frame
    return decl;
}

void SkydomeMesh::CreateBuffers(IDirect3DDevice9* dev, IFileManager& fm)
{
    if (dev == nullptr) return;

    for (SubMeshGpu& gpu : m_subMeshes)
    {
        if (gpu.effect == nullptr) continue;   // unresolved -> never drawn

        // Idempotent refill: drop any existing GPU resources first.
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
            if (SUCCEEDED(gpu.vb->Lock(0, 0, &p, 0)))
            {
                memcpy(p, gpu.vertexBytes.data(), vbBytes);
                gpu.vb->Unlock();
            }
            else
            {
                relptr(gpu.vb);   // never let a created-but-unfilled VB reach the draw
            }
        }
        if (SUCCEEDED(dev->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                             D3DPOOL_DEFAULT, &gpu.ib, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(gpu.ib->Lock(0, 0, &p, 0)))
            {
                memcpy(p, gpu.indexBytes.data(), ibBytes);
                gpu.ib->Unlock();
            }
            else
            {
                relptr(gpu.ib);
            }
        }

        // Material textures: one slot per param (NULL for non-TEXTURE), so the
        // draw can index by-param without a separate cursor.
        gpu.matTextures.assign(gpu.params.size(), nullptr);
        for (size_t i = 0; i < gpu.params.size(); ++i)
        {
            if (gpu.params[i].kind == AloShaderParam::TEXTURE)
                gpu.matTextures[i] = loadMaterialTexture(dev, fm, gpu.params[i].tex);
        }
    }
}

void SkydomeMesh::OnLostDevice()
{
    for (SubMeshGpu& gpu : m_subMeshes)
    {
        relptr(gpu.vb);
        relptr(gpu.ib);
        for (IDirect3DTexture9*& t : gpu.matTextures) relptr(t);
        gpu.matTextures.clear();
        if (gpu.effect) gpu.effect->OnLostDevice();
    }
}

void SkydomeMesh::OnResetEffects()
{
    for (SubMeshGpu& gpu : m_subMeshes)
        if (gpu.effect) gpu.effect->OnResetDevice();
}

void SkydomeMesh::ReleaseGpuBuffers()
{
    for (SubMeshGpu& gpu : m_subMeshes)
    {
        relptr(gpu.vb);
        relptr(gpu.ib);
        for (IDirect3DTexture9*& t : gpu.matTextures) relptr(t);
        gpu.matTextures.clear();
    }
}

void SkydomeMesh::ReleaseEffects()
{
    for (SubMeshGpu& gpu : m_subMeshes)
        relptr(gpu.effect);
}

void SkydomeMesh::ReleaseDecls()
{
    for (std::map<std::string, IDirect3DVertexDeclaration9*>::iterator it = m_decls.begin();
         it != m_decls.end(); ++it)
    {
        if (it->second) it->second->Release();
    }
    m_decls.clear();
}
