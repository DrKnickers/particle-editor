#include "../src/SkydomeDewrap.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>
#include <cstring>
using std::vector;

static int g_fail = 0;
static void ok(bool c, const char* m) { printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m); if (!c) ++g_fail; }
static bool close(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }

// Synthetic NU2-style vertex: stride 32, pos@0/normal@12 (6 floats), uv@24.
// pos/normal are seeded from `marker` so we can prove a duplicate cloned them.
static const uint32_t STRIDE = 32, UVOFF = 24;
static void addV(vector<unsigned char>& b, float u, float v, float marker) {
    size_t base = b.size(); b.resize(base + STRIDE);
    for (int i = 0; i < 6; ++i) { float f = marker + i; std::memcpy(&b[base + i * 4], &f, 4); }
    std::memcpy(&b[base + UVOFF], &u, 4); std::memcpy(&b[base + UVOFF + 4], &v, 4);
}
static float getU(const vector<unsigned char>& b, uint32_t v) { float u; std::memcpy(&u, &b[(size_t)v * STRIDE + UVOFF], 4); return u; }
static float getMarker(const vector<unsigned char>& b, uint32_t v) { float f; std::memcpy(&f, &b[(size_t)v * STRIDE], 4); return f; }
static void tri(vector<unsigned char>& idx, uint16_t a, uint16_t b, uint16_t c) {
    for (uint16_t x : { a, b, c }) { size_t base = idx.size(); idx.resize(base + 2); std::memcpy(&idx[base], &x, 2); }
}
static uint16_t getI(const vector<unsigned char>& idx, uint32_t i) { uint16_t x; std::memcpy(&x, &idx[(size_t)i * 2], 2); return x; }
// Vertex with an explicit position (pos@0) for the re-UV / pole-collapse tests.
static void addVP(vector<unsigned char>& b, float x, float y, float z) {
    size_t base = b.size(); b.resize(base + STRIDE);
    float p[3] = { x, y, z }; std::memcpy(&b[base], p, 12);
}
static float getV(const vector<unsigned char>& b, uint32_t i) { float v; std::memcpy(&v, &b[(size_t)i * STRIDE + UVOFF + 4], 4); return v; }

int main() {
    // --- No-op for a non-tiled mesh (u-span < 1.5): land dome / nebula / billboard.
    {
        vector<unsigned char> vb, ib;
        addV(vb, 0.0f, 0.5f, 100); addV(vb, 0.5f, 0.5f, 200); addV(vb, 1.0f, 0.5f, 300);
        tri(ib, 0, 1, 2);
        uint32_t n = skydome::DewrapSphereUVs(vb, STRIDE, UVOFF, ib);
        ok(n == 3, "no-op: span<1.5 -> vcount unchanged");
        ok(close(getU(vb, 1), 0.5f), "no-op: u left untouched (no scale)");
        ok(ib.size() == 6, "no-op: index buffer unchanged");
    }

    // --- Tiled sphere: u = {0, 1.6, 3.2, 4.8, 6.4}, span 6.4 -> N=6, scale=0.9375.
    //     Triangles: T0(0,1,2) T1(2,3,4) local; T2(4,0,1) straddles the closure.
    {
        vector<unsigned char> vb, ib;
        addV(vb, 0.0f, 0.5f, 10); addV(vb, 1.6f, 0.5f, 20); addV(vb, 3.2f, 0.5f, 30);
        addV(vb, 4.8f, 0.5f, 40); addV(vb, 6.4f, 0.5f, 50);
        tri(ib, 0, 1, 2); tri(ib, 2, 3, 4); tri(ib, 4, 0, 1);   // T0, T1, T2(wrap)
        uint32_t n = skydome::DewrapSphereUVs(vb, STRIDE, UVOFF, ib);

        // (2) integer-tiling scale applied to every original vert.
        ok(close(getU(vb, 0), 0.0f), "scale: v0 u=0");
        ok(close(getU(vb, 1), 1.5f), "scale: v1 1.6 -> 1.5");
        ok(close(getU(vb, 2), 3.0f), "scale: v2 3.2 -> 3.0 (integer tiling)");
        ok(close(getU(vb, 4), 6.0f), "scale: v4 6.4 -> 6.0 (closure == N)");

        // (3) two unique low-u verts (v0,v1) duplicated for the wrap triangle.
        ok(n == 7, "dewrap: 2 duplicate verts appended (vcount 5 -> 7)");
        ok(getI(ib, 0) == 0 && getI(ib, 1) == 1 && getI(ib, 2) == 2, "T0 (local) indices untouched");
        ok(getI(ib, 3) == 2 && getI(ib, 4) == 3 && getI(ib, 5) == 4, "T1 (local) indices untouched");
        ok(getI(ib, 6) == 4, "T2: high-u vert (v4) kept");
        ok(getI(ib, 7) == 5 && getI(ib, 8) == 6, "T2: low-u verts repointed to duplicates");

        // (4) duplicates carry u+N and a clone of the original's other fields.
        ok(close(getU(vb, 5), 6.0f), "dup of v0: u = 0 + 6");
        ok(close(getU(vb, 6), 7.5f), "dup of v1: u = 1.5 + 6");
        ok(close(getMarker(vb, 5), 10.0f), "dup of v0 cloned pos/normal (marker 10)");
        ok(close(getMarker(vb, 6), 20.0f), "dup of v1 cloned pos/normal (marker 20)");

        // (5) originals are unchanged (still used by the local triangles).
        ok(close(getU(vb, 0), 0.0f) && close(getMarker(vb, 0), 10.0f), "original v0 intact for T0");

        // (6) the wrap triangle now spans locally [6.0,7.5] -> no backward smear.
        float tu0 = getU(vb, getI(ib, 6)), tu1 = getU(vb, getI(ib, 7)), tu2 = getU(vb, getI(ib, 8));
        float sp = std::max({ tu0, tu1, tu2 }) - std::min({ tu0, tu1, tu2 });
        ok(sp <= 3.0f, "T2 u-span now local (<= N/2), seam removed");
    }

    // --- SphericalReUV: u=atan2(y,x)/(2pi)*tilesU, v=asin(z/r)/pi*tilesV (tiles 6,6).
    {
        vector<unsigned char> vb, ib;
        addVP(vb,  1.0f, 0.0f, 0.0f);   // +X: lon 0     -> u 0,   lat 0   -> v 0
        addVP(vb,  0.0f, 1.0f, 0.0f);   // +Y: lon +pi/2 -> u 1.5, lat 0   -> v 0
        addVP(vb, -1.0f, 0.0f, 0.0f);   // -X: lon  pi   -> u 3.0
        addVP(vb,  0.0f, 0.0f, 1.0f);   // +Z: lat +pi/2 -> v 3.0 (north pole)
        skydome::SphericalReUV(vb, STRIDE, 0, UVOFF, 6.0f, 6.0f);
        ok(close(getU(vb, 0), 0.0f) && close(getV(vb, 0), 0.0f), "reUV: +X -> u0 v0");
        ok(close(getU(vb, 1), 1.5f), "reUV: +Y -> u = tilesU/4");
        ok(close(std::fabs(getU(vb, 2)), 3.0f), "reUV: -X -> u = tilesU/2 (seam meridian)");
        ok(close(getV(vb, 3), 3.0f), "reUV: +Z -> v = tilesV/2 (pole)");
    }

    // --- CollapsePoleUVs: a pole vert + two high-lat ring verts -> pole u collapses
    //     to the ring mean (duplicated), so the cap triangle no longer spans longitude.
    {
        vector<unsigned char> vb, ib;
        addVP(vb, 0.0f, 0.0f, 1.0f);    // v0: north pole (|lat|=90 > 75)
        addVP(vb, 0.30f, 0.02f, 0.95f); // v1: high-lat ring (|lat|~72) -- treat as non-pole
        addVP(vb, 0.02f, 0.30f, 0.95f); // v2: high-lat ring
        tri(ib, 0, 1, 2);
        skydome::SphericalReUV(vb, STRIDE, 0, UVOFF, 6.0f, 6.0f);
        const float u1 = getU(vb, 1), u2 = getU(vb, 2);
        const uint32_t n = skydome::CollapsePoleUVs(vb, STRIDE, 0, UVOFF, ib, 75.0f);
        ok(n == 4, "pole: pole vert duplicated (vcount 3 -> 4)");
        ok(getI(ib, 0) == 3, "pole: triangle's pole vertex repointed to the duplicate");
        ok(getI(ib, 1) == 1 && getI(ib, 2) == 2, "pole: ring verts untouched");
        ok(close(getU(vb, 3), (u1 + u2) * 0.5f), "pole: duplicate u = mean(ring u) -> no longitude span");
        ok(close(getU(vb, 0), 0.0f), "pole: original pole vert (u=0) preserved for other tris");
    }

    printf(g_fail ? "\n=== skydome dewrap: %d FAIL ===\n" : "\n=== skydome dewrap: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
