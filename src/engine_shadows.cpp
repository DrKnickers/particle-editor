// engine_shadows.cpp — the reference-object shadow cluster of the Engine class,
// moved verbatim out of engine_reference.cpp (a translation-unit split). SAME class, same header
// (engine.h); cluster-local statics moved with their consumers.

#include <cctype>      // tolower (hardpoint bone matching)
#include <cmath>       // fabsf (manipulator math)
#include <cstdio>      // fprintf/snprintf (shadow-leak probe, readouts)
#include <cstdlib>     // getenv (ALO_* probes)

#include "engine.h"
#include "engine_internal.h"
#include "exceptions.h"
#include "utils.h"
#include "Resources/resource.h"
#include "EmitterInstance.h"      // EmitterInstance::Vertex (world-line/tri/ribbon draws)
#include "GizmoSizing.h"          // pure screen-uniform gizmo-handle formula
#include "PlaneHandle.h"          // pure ground-plane handle math
#include "GizmoRibbon.h"          // camera-facing ribbon quad expansion
#include "RingFade.h"             // ring back-face alpha falloff
#include "SelectionBoxStyle.h"    // selection-box bracket/dash geometry
#include "ReferenceObjectWorld.h" // pure reference-object world-matrix builder

using namespace std;

void Engine::RenderReferenceShadows()
{
    if (!m_modelShadowsEnabled || !m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;

    bool any = !m_referenceObjectMesh.ShadowSubMeshes().empty();
    for (size_t i = 0; !any && i < m_referenceAttachments.size(); ++i)
        any = !m_referenceAttachments[i]->mesh.ShadowSubMeshes().empty();
    if (!any) return;

    // [redbug] One-shot marker proving the shadow pass actually DRAWS volumes (not
    // merely that the shadow-volume shader was loaded during ref-object resolution,
    // which happens regardless of whether this pass runs). The regression guard
    // greps for this so it can't false-PASS on a
    // shader-load-without-draw. Past the early-returns above the function always
    // draws, so this fires exactly when a real shadow pass executes. Gated by
    // ALO_SHADER_DIAG + once-per-process so production stays silent.
    {
        static int  s_diag   = -1;
        static bool s_logged = false;
        if (s_diag < 0) s_diag = (getenv("ALO_SHADER_DIAG") != nullptr) ? 1 : 0;
        if (s_diag && !s_logged) {
            size_t vols = m_referenceObjectMesh.ShadowSubMeshes().size();
            for (size_t i = 0; i < m_referenceAttachments.size(); ++i)
                vols += m_referenceAttachments[i]->mesh.ShadowSubMeshes().size();
            printf("[redbug-shadow] RenderReferenceShadows drawing %zu shadow volume(s)\n", vols);
            fflush(stdout);
            s_logged = true;
        }
    }

    const D3DXMATRIX objectWorld = ReferenceObjectDisplayWorld();

    // Extrusion distance for the silhouette volume. alo-viewer uses a large fixed
    // 4000; scale up for big meshes so the volume reliably clears the ground plane.
    float extrusionDist = 4000.0f;
    {
        D3DXVECTOR3 objMin, objMax;
        if (m_referenceObjectMesh.GetBoundingBox(objMin, objMax)) {
            D3DXVECTOR3 d = objMax - objMin;
            extrusionDist = max(4000.0f, 2.0f * D3DXVec3Length(&d));
        }
    }

    // Eye-space depth push for the shadow volume (replaces the old clip-space NDC
    // bias). The volume is shoved a CONSTANT distance DEEPER in EYE space before
    // projection, so coincident near-cap faces z-fail deterministically (kills the
    // self-shadow flicker) WITHOUT the camera-distance drift the clip-Z bias caused.
    //
    // Why eye-space, not clip-space: the old `z_clip += kBias*w` was a CONSTANT
    // offset in NDC. But under this editor's fixed-near=1 / infinite-far projection
    // (z_ndc = 1 - 2/d), a constant NDC nudge maps to a WORLD-depth recession of the
    // volume of ~ (d^2/2)*kBias — QUADRATIC in camera distance d. So the shadow
    // contact held position up close but slid/peter-panned as the camera zoomed out
    // (the reported bug). A constant eye-space translate is a constant WORLD-depth
    // offset at every distance, so the contact holds position at all zooms. The push
    // does introduce a sub-percent screen-space shift of the volume (a deeper vertex
    // projects slightly toward the principal point) but it SHRINKS with distance and
    // is negligible vs the d^2 drift it removes.
    //
    // Sign/magnitude tunable: too small -> self-shadow flicker returns; too large ->
    // the shadow visibly detaches from the model base up close.
    float kShadowEyePush = 2.0f;   // world units to push the volume DEEPER (the fix)
    float kOldClipZBias  = 0.0f;   // 0 = use the eye push (default, shipped behaviour)
#ifndef NDEBUG
    // [shadow-repro] Diagnostic overrides (Debug-only, inert unless set):
    //   ALO_SHADOW_ZPUSH = eye-space push distance in world units (A/B the magnitude).
    //   ALO_SHADOW_ZBIAS = revert to the OLD clip-Z NDC bias with this value, so the
    //     pre-fix camera-distance drift can be reproduced for before/after capture.
    //     ALO_SHADOW_ZBIAS=0 disables BOTH (the no-bias baseline; flicker expected).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZPUSH", b, sizeof(b)) > 0) kShadowEyePush = (float)atof(b); }
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZBIAS", b, sizeof(b)) > 0) { kOldClipZBias = (float)atof(b); kShadowEyePush = 0.0f; } }
#endif
    // RH eye space looks down -Z, so DEEPER = more negative z_eye => translate by
    // -push. Inserted between view and projection: vpPush = view * zpush * proj.
    // (zbiasClip is identity in the shipped path; only the Debug A/B sets it.)
    D3DXMATRIX zpush; D3DXMatrixTranslation(&zpush, 0.0f, 0.0f, -kShadowEyePush);
    D3DXMATRIX zbiasClip; D3DXMatrixIdentity(&zbiasClip); zbiasClip._43 = kOldClipZBias;
    D3DXMATRIX viewProjPush = m_view * zpush * m_projection * zbiasClip;

    // Shadow tint (the multiplicative ZERO/SRCCOLOR darken colour, from m_shadow =
    // the Lighting panel's Sun Shadow Color).
    float shR = m_shadow.x, shG = m_shadow.y, shB = m_shadow.z;
#ifndef NDEBUG
    // [shadow-repro] Debug override so the faint default tint can be forced dark
    // enough to MEASURE the edge feather / contact line in headless captures.
    // ALO_SHADOW_TINT = a single grey level [0..1] (0 = black, fully dark shadow).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_TINT", b, sizeof(b)) > 0) { float v=(float)atof(b); shR=shG=shB=v; } }
#endif

    // --- save every state we touch ---
    DWORD oCW,oZF,oZW,oZE,oSE,oTSS,oSR,oSM,oSWM,oCull,oSFn,oSP,oSZF,oSFa,
          oCcwFn,oCcwP,oCcwZF,oCcwFa,oAB,oSB,oDB,oLit,oFVF,oATE;
    DWORD oTexCOP,oTexCA1,oTexAOP,oTexAA1;
    m_pDevice->GetRenderState(D3DRS_COLORWRITEENABLE,&oCW); m_pDevice->GetRenderState(D3DRS_ZFUNC,&oZF);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,&oZW); m_pDevice->GetRenderState(D3DRS_ZENABLE,&oZE);
    m_pDevice->GetRenderState(D3DRS_STENCILENABLE,&oSE); m_pDevice->GetRenderState(D3DRS_TWOSIDEDSTENCILMODE,&oTSS);
    m_pDevice->GetRenderState(D3DRS_STENCILREF,&oSR); m_pDevice->GetRenderState(D3DRS_STENCILMASK,&oSM);
    m_pDevice->GetRenderState(D3DRS_STENCILWRITEMASK,&oSWM); m_pDevice->GetRenderState(D3DRS_CULLMODE,&oCull);
    m_pDevice->GetRenderState(D3DRS_STENCILFUNC,&oSFn); m_pDevice->GetRenderState(D3DRS_STENCILPASS,&oSP);
    m_pDevice->GetRenderState(D3DRS_STENCILZFAIL,&oSZF); m_pDevice->GetRenderState(D3DRS_STENCILFAIL,&oSFa);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILFUNC,&oCcwFn); m_pDevice->GetRenderState(D3DRS_CCW_STENCILPASS,&oCcwP);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILZFAIL,&oCcwZF); m_pDevice->GetRenderState(D3DRS_CCW_STENCILFAIL,&oCcwFa);
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE,&oAB); m_pDevice->GetRenderState(D3DRS_SRCBLEND,&oSB);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,&oDB); m_pDevice->GetRenderState(D3DRS_LIGHTING,&oLit);
    m_pDevice->GetRenderState(D3DRS_ALPHATESTENABLE,&oATE);
    m_pDevice->GetFVF(&oFVF);
    m_pDevice->GetTextureStageState(0,D3DTSS_COLOROP,&oTexCOP); m_pDevice->GetTextureStageState(0,D3DTSS_COLORARG1,&oTexCA1);
    m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAOP,&oTexAOP); m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAARG1,&oTexAA1);
    IDirect3DVertexDeclaration9* oDecl=NULL; m_pDevice->GetVertexDeclaration(&oDecl);
    IDirect3DVertexShader9* oVS=NULL; m_pDevice->GetVertexShader(&oVS);
    IDirect3DPixelShader9*  oPS=NULL; m_pDevice->GetPixelShader(&oPS);
    IDirect3DBaseTexture9*  oTex0=NULL; m_pDevice->GetTexture(0,&oTex0);
    IDirect3DVertexBuffer9* oStream0=NULL; UINT oStreamOffset=0, oStreamStride=0;
    m_pDevice->GetStreamSource(0, &oStream0, &oStreamOffset, &oStreamStride);
    IDirect3DIndexBuffer9*  oIndices=NULL; m_pDevice->GetIndices(&oIndices);

    // [soft-shadows] Soft path is available only when the toggle is on AND the
    // blur effect + mask RT both came up. Otherwise fall back to the shipped hard
    // darken quad (no regression). Decided once, up front, so the save/restore of
    // the extra resources (RT / depth-stencil / viewport / samplers 0-3) is paired.
    const bool soft = m_softShadowsEnabled && m_shadowBlurReady
                   && m_pShadowBlurEffect != NULL && m_pShadowMask != NULL;

    // Extra save for the soft path's RT detour + sampler binds. AddRef'd handles
    // released in the restore tail; sampler filter/address states restored too.
    IDirect3DSurface9* oRT0 = NULL;  IDirect3DSurface9* oDS = NULL;
    D3DVIEWPORT9 oViewport;
    IDirect3DBaseTexture9* oTexS[4] = { NULL,NULL,NULL,NULL };
    DWORD oMinF[4], oMagF[4], oMipF[4], oAddrU[4], oAddrV[4];
    if (soft)
    {
        m_pDevice->GetRenderTarget(0, &oRT0);          // AddRef'd
        m_pDevice->GetDepthStencilSurface(&oDS);       // AddRef'd (may be NULL)
        m_pDevice->GetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->GetTexture(s, &oTexS[s]);       // AddRef'd
            m_pDevice->GetSamplerState(s, D3DSAMP_MINFILTER, &oMinF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MAGFILTER, &oMagF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MIPFILTER, &oMipF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSU, &oAddrU[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSV, &oAddrV[s]);
        }
    }

    // ============ VOLUME PASS — write stencil (single-pass two-sided z-fail) ============
    // Mirrors alo-viewer: one CULLMODE=NONE pass with TWOSIDEDSTENCILMODE so CW faces
    // INCR and CCW faces DECR the stencil on z-fail in a single draw.
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
    m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,TRUE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,1);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_INCR);   // CW faces incr on zfail
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,D3DSTENCILOP_DECR);// CCW faces decr on zfail
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,D3DSTENCILOP_KEEP);

    // Lambda draws every shadow sub-mesh for the main mesh + all attachments.
    // Technique t0_zfail is kept for its VS (extrudes silhouette to infinity);
    // the technique's own stencil state-block is overridden by our hand-coded ops.
    auto drawVolumes = [&]()
    {
        for (size_t mi = 0; mi <= m_referenceAttachments.size(); ++mi)
        {
            ReferenceObjectMesh& refMesh = (mi==0) ? m_referenceObjectMesh
                                                   : m_referenceAttachments[mi-1]->mesh;
            const D3DXMATRIX worldBase   = (mi==0) ? objectWorld
                                                   : (m_referenceAttachments[mi-1]->boneMatrix * objectWorld);
            for (RefSubMeshGpu& sub : refMesh.ShadowSubMeshes())
            {
                if (sub.effect==NULL || sub.vb==NULL || sub.ib==NULL || sub.decl==NULL) continue;
                if (!sub.effect->isShadowVolume()) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' resolved to a non-shadow effect - skipped\n", sub.shaderName.c_str());
#endif
                    continue;
                }
                D3DXMATRIX world = sub.placement * worldBase;
                D3DXMATRIX invWorld;
                if (!D3DXMatrixInverse(&invWorld,NULL,&world)) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': singular world matrix — identity used for light direction (shadow direction will be wrong)\n", sub.shaderName.c_str());
#endif
                    D3DXMatrixIdentity(&invWorld);
                }
                const D3DXVECTOR3 lightDir3(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
                D3DXVECTOR3 lightObj3; D3DXVec3TransformNormal(&lightObj3,&lightDir3,&invWorld);
                const D3DXVECTOR4 lightObj(lightObj3.x,lightObj3.y,lightObj3.z,0.0f);

                ID3DXEffect* fx = sub.effect->getD3DEffect();
                const Effect::Handles& h = sub.effect->getHandles();
                if (FAILED(fx->SetTechnique("t0_zfail"))) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' has no t0_zfail technique - skipped\n", sub.shaderName.c_str());
#endif
                    fx->Release(); continue;
                }
                // Eye-space-pushed transforms (constant world-depth recession — see
                // the kShadowEyePush note above; replaces the old clip-Z bias that
                // drifted ~d^2 with camera distance). Rigid VS reads WorldViewProjection,
                // skinned VS reads ViewProjection; both ride the same pushed projection.
                D3DXMATRIX wvpB = world * viewProjPush;
                D3DXMATRIX vpB  = viewProjPush;
                fx->SetMatrix(h.hWorldViewProjection, &wvpB);
                fx->SetMatrix(h.hViewProjection,      &vpB);
                fx->SetVector(h.hDirLightObjVec0,     &lightObj);
                fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
                if (sub.skinned && h.hSkinMatrixArray) {
                    D3DXMATRIX palette[24]; for (int b=0;b<24;++b) palette[b]=world;
                    fx->SetMatrixArray(h.hSkinMatrixArray, palette, 24);
                }
                D3DXHANDLE hExtr = fx->GetParameterBySemantic(NULL, "SHADOW_EXTRUSION_DISTANCE");
                if (hExtr) { D3DXVECTOR4 ex(extrusionDist,extrusionDist,extrusionDist,extrusionDist); fx->SetVector(hExtr, &ex); }

                m_pDevice->SetVertexDeclaration(sub.decl);
                m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
                m_pDevice->SetIndices(sub.ib);
                UINT passes=0;
                if (FAILED(fx->Begin(&passes,0)) || passes==0) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': fx->Begin failed or returned 0 passes — skipping sub-mesh\n", sub.shaderName.c_str());
#endif
                    fx->End(); fx->Release(); continue;
                }
                for (UINT p=0;p<passes;++p) {
                    fx->BeginPass(p);
                    m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,sub.vertexCount,0,sub.primitiveCount);
                    fx->EndPass();
                }
                fx->End(); fx->Release();
            }
        }
    };

    // Single two-sided pass: CW faces INCR, CCW faces DECR on z-fail (states set above).
    drawVolumes();

    if (soft)
    {
        // ============ SOFT PATH — mask-to-alpha + blurred multiply composite ============
        // Port of FoC's doSoftShadows: bake the stencil region into a screen-space
        // mask's ALPHA, then run StencilDarkenFinalBlur (4-tap blur + tint) as a
        // ZERO/SRCCOLOR multiply onto the scene. NO depth-bias states anywhere.
        const DWORD tint = D3DCOLOR_COLORVALUE(shR, shG, shB, 1.0f);

        // The mask RT to render the stencil mask into: the matching-MSAA surface
        // when MSAA is active (the stencil test needs the multisampled depth still
        // bound), else the non-MS mask texture's top surface.
        IDirect3DSurface9* pMaskTop  = NULL;       // m_pShadowMask L0, AddRef'd
        m_pShadowMask->GetSurfaceLevel(0, &pMaskTop);
        IDirect3DSurface9* pRenderInto = NULL;     // borrowed (no extra ref to release)
        if (m_msaaActive && m_pShadowMaskMsaa) { pRenderInto = m_pShadowMaskMsaa; }
        else                                    { pRenderInto = pMaskTop; }

        // --- mask-to-alpha (hand-coded StencilDarkenToAlpha state block) ---
        // Bind the mask RT but KEEP the current depth-stencil (the stencil written
        // by the volume pass must persist for the NOTEQUAL test).
        m_pDevice->SetRenderTarget(0, pRenderInto);
        D3DVIEWPORT9 maskVp = { 0, 0,
            m_presentationParameters.BackBufferWidth,
            m_presentationParameters.BackBufferHeight, 0.0f, 1.0f };
        m_pDevice->SetViewport(&maskVp);
        // Clear ALPHA to white (=1, "no shadow") everywhere; the gated quad punches
        // alpha=0 ("shadow") into the stencil region. RGB is irrelevant (blur reads
        // only .a) but a full white clear keeps the target well-defined.
        m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);

        m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);   // shadow where stencil != 0
        m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
        m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
        m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
        m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_ALWAYS);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA); // ALPHA only
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

        // Full-screen XYZRHW quad whose diffuse ALPHA = 0 -> writes alpha 0 into the
        // shadow region (stencil != 0), leaving the cleared white (1) elsewhere.
        const float mx0 = -0.5f, my0 = -0.5f;
        const float mx1 = (float)m_presentationParameters.BackBufferWidth  - 0.5f;
        const float my1 = (float)m_presentationParameters.BackBufferHeight - 0.5f;
        struct PTVtx { float x,y,z,rhw; DWORD c; };
        const DWORD shadowAlpha0 = 0x00000000;   // RGBA, alpha 0
        const PTVtx maskQuad[4] = {
            {mx0,my0,0,1,shadowAlpha0},{mx1,my0,0,1,shadowAlpha0},
            {mx0,my1,0,1,shadowAlpha0},{mx1,my1,0,1,shadowAlpha0} };
        m_pDevice->SetVertexShader(NULL);
        m_pDevice->SetPixelShader(NULL);
        m_pDevice->SetTexture(0,NULL);
        m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
        m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,maskQuad,sizeof(PTVtx));

        // --- MSAA resolve: multisampled mask surface -> the sampled texture ---
        if (m_msaaActive && m_pShadowMaskMsaa && pMaskTop)
        {
            m_pDevice->StretchRect(m_pShadowMaskMsaa, NULL, pMaskTop, NULL, D3DTEXF_NONE);
        }

        // --- restore scene RT + depth-stencil + viewport before the composite ---
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);

        // --- blur composite (StencilDarkenFinalBlur, ZERO/SRCCOLOR multiply) ---
        // Bind the resolved mask to sampler stages 0-3 (the .fx's sampler0..3 all
        // read the same mask; the VS spreads 4 tap offsets across them). LINEAR
        // filtering + clamp: the mask alpha is a HARD 0/1 step, so POINT-sampled
        // taps land on discrete texels and stair-step the edge (reads crisp/hard
        // even with a wide blurAmt). Bilinear lets each tap straddle the edge texel
        // so the 4-tap cross resolves into a smooth feather (bug-1 fix; pairs with
        // the wider blurAmt below). MinF/MagF are saved/restored in the tail.
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->SetTexture(s, m_pShadowMask);
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        }

        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,FALSE);
        // Depth-test the composite so the shadow only darkens SCENE GEOMETRY, never
        // the cleared-far background. The blurred mask bleeds the shadow a few texels
        // past the model/ground silhouette; without this, that bleed darkened the
        // empty background (black bands in the sky around the model). The quad sits at
        // NDC z=1.0 (far) with ZFUNC=GREATER, so it passes only where the depth buffer
        // holds geometry (depth < 1.0) and is rejected on the cleared background
        // (depth == 1.0). ZWRITE stays off (the depth-stencil oDS is bound here).
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_GREATER);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
        m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);

        // Clip-space full-screen quad (the VS multiplies by m_worldViewProj, which
        // we drive to identity). Vertex COLOR0 = tint (the ps lerps tint<->white by
        // the mask alpha). UV v increases downward (top of screen = v 0).
        //
        // CRITICAL: the shadow mask was rendered into the FULL backbuffer (the
        // mask-to-alpha viewport is 0,0..W,H), but this composite draws into the
        // SCENE VIEWPORT (oViewport) — a SUB-RECT of the backbuffer in the live
        // editor (the React panels inset the 3D view). So the quad's UVs must span
        // the scene viewport's sub-region of the mask, NOT 0..1, or the shadow is
        // scaled+offset off the object (the "floating silhouette" bug). With an
        // edge-to-edge clip(±1)→viewport and UV→sub-rect mapping, pixel centers land
        // on texel centers exactly — no separate half-texel nudge needed. In
        // --capture the viewport IS the full backbuffer, so this reduces to 0..1
        // (which is why the bug never showed in headless capture).
        const float Wbb = (float)m_presentationParameters.BackBufferWidth;
        const float Hbb = (float)m_presentationParameters.BackBufferHeight;
        float u0 = (float)oViewport.X / Wbb;
        float u1 = (float)(oViewport.X + oViewport.Width)  / Wbb;
        float v0 = (float)oViewport.Y / Hbb;
        float v1 = (float)(oViewport.Y + oViewport.Height) / Hbb;
#ifndef NDEBUG
        // [shadow-repro] ALO_SHADOW_VPFIX=0 reverts to the old full-mask 0..1 UVs
        // (the pre-fix "floating silhouette" behaviour) for a before/after A/B.
        { char b[8]; if (GetEnvironmentVariableA("ALO_SHADOW_VPFIX", b, sizeof(b)) > 0 && atof(b) == 0.0) { u0=0.0f; u1=1.0f; v0=0.0f; v1=1.0f; } }
#endif
        struct BlurVtx { float x,y,z; float nx,ny,nz; DWORD c; float u,v; };
        // z = 1.0 (NDC far) so the ZFUNC=GREATER depth test above darkens only
        // geometry pixels (depth < 1.0), not the cleared-far background.
        const BlurVtx blurQuad[4] = {
            { -1.0f,  1.0f, 1.0f, 0,0,1, tint, u0, v0 },  // top-left
            {  1.0f,  1.0f, 1.0f, 0,0,1, tint, u1, v0 },  // top-right
            { -1.0f, -1.0f, 1.0f, 0,0,1, tint, u0, v1 },  // bottom-left
            {  1.0f, -1.0f, 1.0f, 0,0,1, tint, u1, v1 },  // bottom-right
        };

        ID3DXEffect* bfx = m_pShadowBlurEffect->getD3DEffect();   // AddRef'd
        D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
        if (m_hShadowBlurWvp)  bfx->SetMatrix(m_hShadowBlurWvp, &ident);
        // blurAmt is the 4-tap cross half-spread in UV. The game's authored 0.0015
        // is only ~2 texels at the editor's RT size — reads HARD with POINT sampling.
        // The shader is only a 4-tap cross, so a WIDE spread (e.g. 0.009 ≈ 11 texels)
        // makes the 4 taps read as discrete offset copies — a plus-shaped GHOST/halo.
        // ~0.003 (≈4 texels) keeps the taps blending; paired with the LINEAR sampling
        // above that gives a soft edge without ghosting. (For a genuinely WIDE soft
        // shadow the 4-tap must become multi-pass / more taps — a separate change.)
        // Tunable via ALO_SHADOW_BLURAMT.
        float blurAmt = 0.003f;
#ifndef NDEBUG
        // [shadow-repro] Diagnostic blur-width override (A/B the feather width
        // without recompiling). Inert unless ALO_SHADOW_BLURAMT is set; Debug-only.
        { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_BLURAMT", b, sizeof(b)) > 0) blurAmt = (float)atof(b); }
#endif
        if (m_hShadowBlurAmt)  bfx->SetFloat(m_hShadowBlurAmt, blurAmt);
        bfx->SetTechnique(m_hShadowBlurTech);
        // The blur uses a vertex shader (vs_1_1) reading POSITION/COLOR0/TEXCOORD0,
        // so emit via an FVF with a NON-transformed float4 POSITION (the VS applies
        // the identity m_worldViewProj). D3DFVF_XYZRHW would skip the VS entirely.
        // The NORMAL slot is required: the game's compiled vs_1_1 was authored
        // against the VERTEX_MESH_NU2C layout (pos/normal/uv/color), so its input
        // declaration expects a normal between POSITION and TEXCOORD0. Omitting it
        // shifts the texcoord register the VS reads, so every pixel sampled one
        // mask texel -> the whole frame got the shadow tint instead of just the
        // cast region. (Matches alo-viewer's m_sceneQuad FVF.)
        m_pDevice->SetVertexShader(NULL);   // FVF path -> fixed-function VS slot; effect's VS binds in BeginPass
        m_pDevice->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1);
        UINT bpasses = 0;
        if (SUCCEEDED(bfx->Begin(&bpasses, 0)) && bpasses > 0)
        {
            for (UINT bp = 0; bp < bpasses; ++bp)
            {
                bfx->BeginPass(bp);
                m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, blurQuad, sizeof(BlurVtx));
                bfx->EndPass();
            }
        }
        bfx->End();
        bfx->Release();

        SAFE_RELEASE(pMaskTop);
    }
    else
    {
    // ============ DARKEN PASS (hard fallback) ============
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
    m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

    D3DVIEWPORT9 vp; m_pDevice->GetViewport(&vp);
    const float x0=(float)vp.X-0.5f, y0=(float)vp.Y-0.5f;
    const float x1=(float)(vp.X+vp.Width)-0.5f, y1=(float)(vp.Y+vp.Height)-0.5f;
    const DWORD tint = D3DCOLOR_COLORVALUE(shR,shG,shB,1.0f);
    struct PTVtx { float x,y,z,rhw; DWORD c; };
    const PTVtx quad[4] = { {x0,y0,0,1,tint},{x1,y0,0,1,tint},{x0,y1,0,1,tint},{x1,y1,0,1,tint} };
    m_pDevice->SetVertexShader(NULL);
    m_pDevice->SetPixelShader(NULL);
    m_pDevice->SetTexture(0,NULL);
    m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(PTVtx));
    }

    // --- restore ---
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,oSE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,oTSS);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,oCW); m_pDevice->SetRenderState(D3DRS_ZFUNC,oZF);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,oZW); m_pDevice->SetRenderState(D3DRS_ZENABLE,oZE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,oSR); m_pDevice->SetRenderState(D3DRS_STENCILMASK,oSM);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,oSWM); m_pDevice->SetRenderState(D3DRS_CULLMODE,oCull);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,oSFn); m_pDevice->SetRenderState(D3DRS_STENCILPASS,oSP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,oSZF); m_pDevice->SetRenderState(D3DRS_STENCILFAIL,oSFa);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,oCcwFn); m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,oCcwP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,oCcwZF); m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,oCcwFa);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,oAB); m_pDevice->SetRenderState(D3DRS_SRCBLEND,oSB);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,oDB); m_pDevice->SetRenderState(D3DRS_LIGHTING,oLit);
    m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,oATE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,oTexCOP); m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,oTexCA1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,oTexAOP); m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,oTexAA1);
    // [red-bug fix] FVF and an explicit vertex declaration share ONE device slot in
    // D3D9, and GetFVF reports the SAME code (0x252) for BOTH the engine's explicit
    // ParticleElements decl (COLOR at offset 40) and the FVF-canonical layout
    // (COLOR at offset 24) because they share a component set (XYZ|NORMAL|DIFFUSE|TEX2).
    // The old unconditional SetFVF(oFVF) therefore overwrote the just-restored explicit
    // particle decl with the FVF-canonical layout, whose offsets do NOT match the
    // 44-byte EmitterInstance::Vertex. The inheriting particle DrawIndexedPrimitiveUP
    // (EmitterInstance::Render binds no decl/FVF of its own) then read the particle
    // COLOR out of the UV0 bytes -> additive source ~= 0 -> the additive emitter
    // vanished, and it persisted until a device reset because nothing rebinds
    // m_pDeclaration. Restore whichever vertex format was actually active, never both:
    // at the shadow-pass entry this engine always has an explicit decl bound, so oDecl
    // is non-null in practice; the SetFVF branch is a fallback for a pure-FVF device
    // (where GetVertexDeclaration returns null). Guarded headless by a
    // regression check (ALO_DUMP_RSTATE decl-element dump).
    if (oDecl) { m_pDevice->SetVertexDeclaration(oDecl); oDecl->Release(); }
    else       { m_pDevice->SetFVF(oFVF); }
    m_pDevice->SetVertexShader(oVS); if (oVS) oVS->Release();
    m_pDevice->SetPixelShader(oPS);  if (oPS) oPS->Release();
    m_pDevice->SetTexture(0, oTex0); if (oTex0) oTex0->Release();
    m_pDevice->SetStreamSource(0, oStream0, oStreamOffset, oStreamStride);
    if (oStream0) oStream0->Release();
    m_pDevice->SetIndices(oIndices);
    if (oIndices) oIndices->Release();

    // [soft-shadows] Restore the extra resources the soft path detoured through:
    // RT(0) + depth-stencil + viewport (already re-bound before the composite, but
    // re-assert here for the case the composite was skipped), sampler stages 0-3
    // textures + filter/address states. Stage-0 texture is owned by the existing
    // tail above; here we restore stages 1-3 + every stage's sampler states, and
    // release every AddRef'd handle. No-op when the hard path ran.
    if (soft)
    {
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            if (s != 0) m_pDevice->SetTexture(s, oTexS[s]);   // stage 0 done above
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, oMinF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, oMagF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, oMipF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU, oAddrU[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV, oAddrV[s]);
            if (oTexS[s]) oTexS[s]->Release();
        }
        if (oRT0) oRT0->Release();
        if (oDS)  oDS->Release();
    }
}
