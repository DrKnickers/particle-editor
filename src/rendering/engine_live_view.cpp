// Live engine view (#949 3b.3): uploads the frame the engine host streams and draws it over
// the scene viewport in place of our own scene. It only shows external pixels with fixed-
// function texturing; no game shader is involved, so rendering parity is unaffected.
#include "engine.h"
#include "host/AlphaCompositor.h"
#include "host/EngineLiveFrame.h"
#include "host/EngineLiveFraming.h"
#include <cstring>

namespace
{
struct LiveFrameVertex
{
    float x, y, z, rhw;
    float u, v;
};
constexpr DWORD kLiveFrameFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
// Pre-transformed D3D9 vertices address pixel centres, so corners shift by half a pixel to
// line the quad up with the viewport's pixel edges.
constexpr float kPixelCornerOffset = 0.5f;
constexpr UINT  kQuadTriangles     = 2;

void LogLiveViewFailure(const char* step, HRESULT hr)
{
    char buf[128];
    sprintf(buf, "[live-view] ERROR %s failed hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
    OutputDebugStringA(buf);
}
} // namespace

bool Engine::UploadLiveFrame(const std::uint8_t* bgrxRows, UINT widthPx, UINT heightPx, UINT pitchBytes)
{
    if (DeviceCallsBlocked() || m_pDevice == NULL) return false;
    const bool isNewSize = widthPx != m_liveFrameWidthPx || heightPx != m_liveFrameHeightPx;
    if (isNewSize) SAFE_RELEASE(m_pLiveFrameTexture);
    if (m_pLiveFrameTexture == NULL)
    {
        const HRESULT hr = m_pDevice->CreateTexture(widthPx, heightPx, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8,
                                                    D3DPOOL_DEFAULT, &m_pLiveFrameTexture, NULL);
        if (FAILED(hr))
        {
            LogLiveViewFailure("live frame texture", hr);
            return false;
        }
        m_liveFrameWidthPx  = widthPx;
        m_liveFrameHeightPx = heightPx;
    }
    D3DLOCKED_RECT lock     = {};
    const HRESULT  lockedHr = m_pLiveFrameTexture->LockRect(0, &lock, NULL, D3DLOCK_DISCARD);
    if (FAILED(lockedHr))
    {
        LogLiveViewFailure("live frame lock", lockedHr);
        return false;
    }
    const size_t rowBytes = size_t(widthPx) * host::engine_live::kBytesPerPixel;
    for (UINT row = 0; row < heightPx; ++row)
        std::memcpy(static_cast<std::uint8_t*>(lock.pBits) + size_t(row) * lock.Pitch,
                    bgrxRows + size_t(row) * pitchBytes, rowBytes);
    m_pLiveFrameTexture->UnlockRect(0);
    return true;
}

void Engine::ReleaseLiveFrameTexture()
{
    SAFE_RELEASE(m_pLiveFrameTexture);
    SAFE_RELEASE(m_pLiveFrameStateBlock);
    m_liveFrameWidthPx  = 0;
    m_liveFrameHeightPx = 0;
}

bool Engine::RenderLiveFrame()
{
    // The state block restores the render, texture and sampler states and shaders the draw
    // below changes. State blocks do not hold surfaces, so the depth-stencil is restored by hand.
    if (m_pLiveFrameStateBlock == NULL)
    {
        const HRESULT createdHr = m_pDevice->CreateStateBlock(D3DSBT_ALL, &m_pLiveFrameStateBlock);
        if (FAILED(createdHr))
        {
            LogLiveViewFailure("live frame state block", createdHr);
            m_liveViewActive = false; // fall back to our own scene rather than fail every frame
            return false;
        }
    }
    m_pLiveFrameStateBlock->Capture();
    IDirect3DSurface9* previousDepth = NULL;
    m_pDevice->GetDepthStencilSurface(&previousDepth);
    m_pDevice->BeginScene();
    if (m_pAlphaCompositor && m_pAlphaCompositor->GetRenderTarget())
        m_pDevice->SetRenderTarget(0, m_pAlphaCompositor->GetRenderTarget());
    m_pDevice->SetDepthStencilSurface(NULL);
    // XRGB clears alpha to opaque; the compositor reads this RT's alpha as viewport opacity.
    m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    IDirect3DSurface9* target = NULL;
    m_pDevice->GetRenderTarget(0, &target);
    D3DSURFACE_DESC targetDesc = {};
    if (target) target->GetDesc(&targetDesc);
    SAFE_RELEASE(target);
    const float areaX = m_sceneViewportActive ? float(m_sceneViewportX) : 0.0f;
    const float areaY = m_sceneViewportActive ? float(m_sceneViewportY) : 0.0f;
    const float areaW = m_sceneViewportActive ? float(m_sceneViewportW) : float(targetDesc.Width);
    const float areaH = m_sceneViewportActive ? float(m_sceneViewportH) : float(targetDesc.Height);
    // The host draws a 4:3 picture stretched to whatever size its window has, so the frame is
    // shown as 4:3 whatever its pixel size, cropped to cover the viewport without distortion.
    const host::engine_live::FrameCrop crop    = host::engine_live::CoverCropForViewport(areaW, areaH);
    const float                        left    = areaX - kPixelCornerOffset;
    const float                        top     = areaY - kPixelCornerOffset;
    const LiveFrameVertex              quad[4] = {{left, top, 0, 1, crop.u0, crop.v0},
                                                  {left + areaW, top, 0, 1, crop.u1, crop.v0},
                                                  {left, top + areaH, 0, 1, crop.u0, crop.v1},
                                                  {left + areaW, top + areaH, 0, 1, crop.u1, crop.v1}};

    m_pDevice->SetVertexShader(NULL);
    m_pDevice->SetPixelShader(NULL);
    m_pDevice->SetFVF(kLiveFrameFvf);
    m_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,
                              D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    m_pDevice->SetTexture(0, m_pLiveFrameTexture);
    m_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    m_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, kQuadTriangles, quad, sizeof(LiveFrameVertex));
    m_pDevice->EndScene();
    m_pLiveFrameStateBlock->Apply();
    m_pDevice->SetDepthStencilSurface(previousDepth);
    SAFE_RELEASE(previousDepth);

    // Same rule as Render: with a compositor the host presents these pixels through DComp.
    if (!m_pAlphaCompositor) NotifyPresentResult(m_pDevice->Present(NULL, NULL, NULL, NULL));
    return true;
}
