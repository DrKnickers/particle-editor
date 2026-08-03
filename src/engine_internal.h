#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H
//
// Private helpers shared between the Engine translation units
// (engine.cpp + engine_render.cpp / engine_reference.cpp /
// engine_environment.cpp) — Phase B of
// tasks/2026-07-06-heavyweight-refactor-plan.md. Rule: a helper used by more
// than one engine TU gets its declaration HERE and exactly ONE non-static
// definition in its primary-consumer TU — never a per-TU static copy (two
// same-name TU-local statics would compile and silently diverge).
// Cluster-local helpers stay static inside their TU and are deliberately
// absent here.

#include <d3d9.h>
#include <d3dx9.h>

// QPC helpers (defined in engine.cpp; used by core Reset, the render loop,
// and reference display easing).
LONGLONG EngQpcNow();
double   EngQpcUs(LONGLONG a, LONGLONG b);

// Apply a sub-mesh's authored material params (index-parallel handles) to
// the effect (defined in engine_environment.cpp; used by skydome mesh
// rendering AND reference-object rendering — kept in lockstep, DRY audit
// cpp-engine-0).
#include <vector>
struct AloShaderParam;
void ApplyAloMaterialParams(ID3DXEffect* fx,
                            const std::vector<AloShaderParam>& params,
                            const std::vector<D3DXHANDLE>& matHandles,
                            const std::vector<IDirect3DTexture9*>& matTextures);

#endif // ENGINE_INTERNAL_H
