// SkydomeDewrap.h -- remove the closure-meridian seam from a tiled UV-sphere
// skydome so its star texture reads continuous across the longitude wrap.
//
// DELIBERATE NON-FAITHFUL DIVERGENCE. The stock skydome assets (e.g. Mod
// w_stars_low.alo) bake a visible seam at the UV closure: the texture tiles a
// NON-integer number of times in u (~6.47x) AND the triangles bridging the last
// u-ring back to the first interpolate u backward across the whole texture. The
// game and the reference alo-viewer show the same seam -- it is asset-inherent,
// not an editor bug (verified 2026-06-16: identical UV load + draw + shader).
// The user opted to render the dome seamless anyway, accepting the divergence.
// Gate this behind the user's "Smooth skydome seams" toggle; do NOT silently
// re-assert 1:1 fidelity here.
//
// Two corrections, both required (each alone was tried before and failed):
//   1. SCALE u so the dome tiles an INTEGER number of times -> the texture phase
//      at the closure (u=N) matches the start (u=0). Fixes the content mismatch.
//   2. DE-WRAP the bridging triangles: a triangle whose u-span exceeds half the
//      tiled range straddles the closure; its low-u verts get u += N (via a
//      DUPLICATED vertex, so non-wrap triangles keep the original u) -> the
//      triangle interpolates locally instead of backward across the texture.
// With WRAP addressing the sampled texels are unchanged; only the interpolation
// domain and the integer-tiling phase change.
//
// Pure + dependency-free so the shipped math IS the unit-tested math
// (GizmoSizing.h / PlaneHandle.h precedent). Operates in place on the compact
// interleaved GPU vertex byte buffer (UV float2 at `uvOffset`) and the uint16
// triangle-list index byte buffer that SkydomeMesh::Load has already built.

#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>

namespace skydome {

// Replace each vertex's UV with a COHERENT spherical mapping computed from its
// position. The stock skydome UVs are incoherent (~31% of shared edges carry
// arbitrary u-mismatches scattered across all longitudes -- verified via dump),
// so no closure fix can de-seam them; we re-map from scratch instead. Pairs with
// DewrapSphereUVs, which then removes the single atan2 cut this introduces.
// DELIBERATE non-faithful divergence; the caller gates it behind the toggle.
//   u = atan2(y,x)/(2pi) * tilesU   (longitude, wraps at the -x meridian)
//   v = asin(z/r)/pi    * tilesV    (latitude, no wrap -> poles)
// Absolute offset is irrelevant under WRAP sampling; only tile counts matter.
inline void SphericalReUV(std::vector<unsigned char>& vtx, uint32_t stride,
                          uint32_t posOffset, uint32_t uvOffset,
                          float tilesU, float tilesV)
{
    if (stride == 0 || posOffset + 12 > stride || uvOffset + 8 > stride) return;
    const uint32_t vcount = (uint32_t)(vtx.size() / stride);
    const float kPi = 3.14159265358979f;
    for (uint32_t i = 0; i < vcount; ++i)
    {
        float p[3]; std::memcpy(p, &vtx[(size_t)i * stride + posOffset], 12);
        const float r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        float zr = (r > 1e-6f) ? p[2] / r : 0.0f;
        zr = (zr > 1.0f) ? 1.0f : (zr < -1.0f ? -1.0f : zr);
        const float u = (std::atan2(p[1], p[0]) / (2.0f * kPi)) * tilesU;
        const float v = (std::asin(zr) / kPi) * tilesV;
        std::memcpy(&vtx[(size_t)i * stride + uvOffset],     &u, 4);
        std::memcpy(&vtx[(size_t)i * stride + uvOffset + 4], &v, 4);
    }
}

// Collapse the longitude (u) span of pole-cap triangles so the spherical map's
// lat-long singularity doesn't streak the texture radially at the poles. For each
// triangle touching a pole (a vert with |latitude| >= poleLatDeg) but not entirely
// at it, the pole vert's u is duplicated and set to the mean u of the triangle's
// non-pole (ring) verts -- so the cap samples a local texel instead of the whole
// longitude band. Run AFTER SphericalReUV, BEFORE DewrapSphereUVs (the latter's
// uniform u-scale preserves pole_u == mean(ring_u)). Returns the new vertex count.
inline uint32_t CollapsePoleUVs(std::vector<unsigned char>& vtx, uint32_t stride,
                                uint32_t posOffset, uint32_t uvOffset,
                                std::vector<unsigned char>& idx, float poleLatDeg)
{
    if (stride == 0 || posOffset + 12 > stride || uvOffset + 8 > stride) return (uint32_t)(vtx.size() / stride);
    uint32_t vcount = (uint32_t)(vtx.size() / stride);
    const uint32_t tris = (uint32_t)(idx.size() / 2) / 3;
    const float kPi = 3.14159265358979f;
    const float sinThresh = std::sin(poleLatDeg * kPi / 180.0f);

    auto getU = [&](uint32_t v) { float u; std::memcpy(&u, &vtx[(size_t)v * stride + uvOffset], 4); return u; };
    auto isPole = [&](uint32_t v) {
        float p[3]; std::memcpy(p, &vtx[(size_t)v * stride + posOffset], 12);
        const float r = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        return r > 1e-6f && std::fabs(p[2] / r) >= sinThresh;
    };
    auto getI = [&](uint32_t i) { uint16_t x; std::memcpy(&x, &idx[(size_t)i * 2], 2); return x; };
    auto setI = [&](uint32_t i, uint16_t x) { std::memcpy(&idx[(size_t)i * 2], &x, 2); };

    for (uint32_t t = 0; t < tris; ++t)
    {
        const uint16_t vi[3] = { getI(t * 3), getI(t * 3 + 1), getI(t * 3 + 2) };
        const bool pole[3] = { isPole(vi[0]), isPole(vi[1]), isPole(vi[2]) };
        const int npole = (pole[0] ? 1 : 0) + (pole[1] ? 1 : 0) + (pole[2] ? 1 : 0);
        if (npole == 0 || npole == 3) continue;   // interior, or degenerate all-at-pole
        float sum = 0.0f; int cnt = 0;
        for (int k = 0; k < 3; ++k) if (!pole[k]) { sum += getU(vi[k]); ++cnt; }
        const float meanU = sum / (float)cnt;     // ring-side longitude
        for (int k = 0; k < 3; ++k)
        {
            if (!pole[k]) continue;
            if (vcount >= 0xFFFFu) break;
            const uint16_t d = (uint16_t)vcount++;
            const size_t base = vtx.size();
            vtx.resize(base + stride);
            std::memcpy(&vtx[base], &vtx[(size_t)vi[k] * stride], stride); // clone
            std::memcpy(&vtx[base + uvOffset], &meanU, 4);                 // pole u -> ring mean
            setI(t * 3 + k, d);
        }
    }
    return vcount;
}

// Returns the (possibly grown) vertex count. No-op (returns the original count)
// for meshes that do not tile in u -- land domes, nebula billboards, anything
// whose u-extent is under ~1.5 tiles -- so it is safe to call on every sub-mesh.
inline uint32_t DewrapSphereUVs(std::vector<unsigned char>& vtx, uint32_t stride,
                                uint32_t uvOffset, std::vector<unsigned char>& idx)
{
    if (stride == 0 || vtx.size() < stride || uvOffset + 8 > stride) return 0;
    uint32_t vcount = (uint32_t)(vtx.size() / stride);
    const uint32_t icount = (uint32_t)(idx.size() / sizeof(uint16_t));
    if (vcount == 0 || icount < 3) return vcount;

    auto getU = [&](uint32_t v) { float u; std::memcpy(&u, &vtx[(size_t)v * stride + uvOffset], 4); return u; };
    auto setU = [&](uint32_t v, float u) { std::memcpy(&vtx[(size_t)v * stride + uvOffset], &u, 4); };
    auto getI = [&](uint32_t i) { uint16_t x; std::memcpy(&x, &idx[(size_t)i * 2], 2); return x; };
    auto setI = [&](uint32_t i, uint16_t x) { std::memcpy(&idx[(size_t)i * 2], &x, 2); };

    // 1. u-range across the mesh.
    // NOTE: parenthesised (std::min)/(std::max) -- <windows.h> defines min/max macros
    // in this TU, which would otherwise hijack the unparenthesised calls.
    float uMin = FLT_MAX, uMax = -FLT_MAX;
    for (uint32_t v = 0; v < vcount; ++v) { float u = getU(v); uMin = (std::min)(uMin, u); uMax = (std::max)(uMax, u); }
    const float span = uMax - uMin;
    if (span < 1.5f) return vcount;            // not a multi-tile wrap -> leave alone

    // 2. scale u about uMin so the dome tiles an integer number of times.
    const float N = std::round(span);
    if (N < 1.0f) return vcount;
    const float scale = N / span;
    for (uint32_t v = 0; v < vcount; ++v) setU(v, uMin + (getU(v) - uMin) * scale);
    const float loThresh = uMin + N * 0.5f;    // verts below this are the "low-u" side

    // 3. de-wrap the bridging triangles, duplicating their low-u verts (+N).
    std::map<uint16_t, uint16_t> dup;          // original vert -> its (+N) duplicate
    const uint32_t tris = icount / 3;
    for (uint32_t t = 0; t < tris; ++t)
    {
        const uint16_t vi[3] = { getI(t * 3), getI(t * 3 + 1), getI(t * 3 + 2) };
        const float u0 = getU(vi[0]), u1 = getU(vi[1]), u2 = getU(vi[2]);
        const float tmin = (std::min)(u0, (std::min)(u1, u2));
        const float tmax = (std::max)(u0, (std::max)(u1, u2));
        if (tmax - tmin <= N * 0.5f) continue; // local triangle -> not a wrap

        for (int k = 0; k < 3; ++k)
        {
            const uint16_t v = vi[k];
            if (getU(v) >= loThresh) continue; // high-u side stays put
            auto it = dup.find(v);
            uint16_t d;
            if (it != dup.end()) d = it->second;
            else
            {
                if (vcount >= 0xFFFFu) continue; // uint16 index space exhausted -> leave wrapped
                d = (uint16_t)vcount++;        // append a duplicate with u += N
                const size_t base = vtx.size();
                vtx.resize(base + stride);
                std::memcpy(&vtx[base], &vtx[(size_t)v * stride], stride); // clone all fields
                const float du = getU(v) + N;                              // then offset u by +N
                std::memcpy(&vtx[base + uvOffset], &du, 4);
                dup[v] = d;
            }
            setI(t * 3 + k, d);
        }
    }
    return vcount;
}

} // namespace skydome
