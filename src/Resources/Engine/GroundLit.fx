///////////////////////////////////////////////////////////////////////////////
// GroundLit.fx — bump-mapped terrain lighting for the editor's ground plane.
//
// Faithful in spirit to the game's TerrainMeshBump.fx "bump+spec" path
// (reference/foc-shaders/TerrainMeshBump.fx), adapted for the editor's ground:
//   - Cloud-shadow and fog-of-war texture multiplies REMOVED (no editor
//     equivalent — see tasks/todo.md for scope).
//   - Self-contained: the one AlamoEngine.fxh helper we use (SPH fill) is
//     inlined, because the editor loads this as a standalone RCDATA blob via
//     D3DXCreateEffect (no #include resolution at runtime).
//
// PER-PIXEL world-space evaluation (the key difference from the game shader).
// The game computes the tangent-space light/half vectors PER-VERTEX and
// interpolates them across a finely-tessellated terrain mesh. Our ground is a
// single 1600x1600 quad (4 verts); interpolating a per-vertex half-vector over
// that span smears the specular into a broad overbright wash. Because the
// ground is FLAT and its tangent basis is world-axis-aligned (T=+X, B=+Y,
// N=+Z), the tangent-space normal map IS already a world-space normal, so we
// evaluate the whole lighting model per-pixel in world space — exact, no
// tessellation, and identical in result to what the game's fine mesh converges
// to on flat ground.
//
// Lighting model (matches the game):
//   - SUN (light 0) per-pixel: dot3 diffuse off the normal map + gloss specular.
//   - FILL lights per-vertex via spherical harmonics (m_sphLightFill).
//   - Specular gated by the GLOSS held in the alpha of the `_bc` bump-map
//     texture (this game stores terrain gloss there — see TerrainRenderBump.fx
//     `spec … * diffuseTexel.a`), i.e. our g_NormalTexture's alpha. Diffuse is
//     x2 (the game's MODULATE2X terrain brightness); specular is x1, matching
//     TerrainRenderBump (NOT the x2 that TerrainMeshBump uses).
///////////////////////////////////////////////////////////////////////////////

// --- Matrices -------------------------------------------------------------
float4x4 g_WorldViewProj;
float4x4 g_World;

// --- Lighting state (bound from the engine's live light/SPH data) ---------
float4x4 g_SphFill[3];     // Engine::m_sphLightFill — fill lights as SPH
float3   g_LightObjVec;    // sun light vector (object == world space here)
float4   g_LightDiffuse;   // sun diffuse  (m_lights[0].Diffuse)
float4   g_LightSpecular;  // sun specular (m_lights[0].Specular)
float3   g_EyeObjPos;      // camera position (object == world space here)

// --- Material (editor ground uses neutral terrain defaults) ----------------
static const float3 Diffuse  = float3(1.0f, 1.0f, 1.0f);
static const float3 Specular = float3(1.0f, 1.0f, 1.0f);
static const float3 Emissive = float3(0.0f, 0.0f, 0.0f);

// --- Textures --------------------------------------------------------------
texture g_BaseTexture;
texture g_NormalTexture;

sampler BaseSampler = sampler_state
{
    texture   = (g_BaseTexture);
    AddressU  = WRAP;
    AddressV  = WRAP;
    MipFilter = LINEAR;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

sampler NormalSampler = sampler_state
{
    texture   = (g_NormalTexture);
    AddressU  = WRAP;
    AddressV  = WRAP;
    MipFilter = LINEAR;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
};

// Inlined AlamoEngine.fxh helper: fill-light diffuse via spherical harmonics.
float3 Sph_Compute_Diffuse_Light_Fill(float3 world_normal)
{
    float3 diff_light = (float3)0;
    float4 tmp_normal = float4(world_normal, 1);
    diff_light.x = dot(tmp_normal, mul(g_SphFill[0], tmp_normal));
    diff_light.y = dot(tmp_normal, mul(g_SphFill[1], tmp_normal));
    diff_light.z = dot(tmp_normal, mul(g_SphFill[2], tmp_normal));
    return diff_light;
}

///////////////////////////////////////////////////////////////////////////////
struct VS_INPUT
{
    float4 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float2 Tex      : TEXCOORD0;
    float3 Tangent  : TANGENT0;   // unused (basis is world-aligned) — kept for decl
    float3 Binormal : BINORMAL0;  // unused
};

struct VS_OUTPUT
{
    float4 Pos      : POSITION;
    float4 Diff     : COLOR0;     // per-vertex SPH fill
    float2 Tex0     : TEXCOORD0;  // base
    float2 Tex1     : TEXCOORD1;  // normal map
    float3 WorldPos : TEXCOORD2;  // perspective-correct → exact per-pixel V vector
};

VS_OUTPUT bump_vs(VS_INPUT In)
{
    VS_OUTPUT Out = (VS_OUTPUT)0;
    Out.Pos  = mul(In.Pos, g_WorldViewProj);
    Out.Tex0 = In.Tex;
    Out.Tex1 = In.Tex;

    float3 world_pos    = mul(In.Pos, g_World).xyz;
    float3 world_normal = normalize(mul(In.Normal, (float3x3)g_World));
    Out.WorldPos = world_pos;
    Out.Diff     = float4(Sph_Compute_Diffuse_Light_Fill(world_normal) * Diffuse + Emissive, 1.0f);
    return Out;
}

float4 bump_ps(VS_OUTPUT In) : COLOR
{
    float4 base = tex2D(BaseSampler,   In.Tex0);
    float4 nrm  = tex2D(NormalSampler, In.Tex1);

    // The ground's tangent basis is world-aligned (T=+X, B=+Y, N=+Z), so the
    // tangent-space normal map is already a world-space normal.
    float3 N = normalize(2.0f * (nrm.rgb - 0.5f));
    float3 L = normalize(g_LightObjVec);
    float3 V = normalize(g_EyeObjPos - In.WorldPos);
    float3 H = normalize(L + V);

    float ndotl = dot(N, L);
    float ndoth = saturate(dot(N, H));
    if (ndotl < 0.0f) ndotl = -0.25f * ndotl;     // soft wrap on the dark side

    // Gloss is in the normal/_bc map's alpha; specular is x1 (TerrainRenderBump).
    // The flat-fallback normal carries alpha 0, so a slot with no real _bc map
    // is matte (no specular) rather than fully glossy.
    float3 diff = base.rgb * (ndotl * Diffuse * g_LightDiffuse.rgb + In.Diff.rgb) * 2.0f;
    float3 spec = g_LightSpecular.rgb * Specular * pow(ndoth, 16) * nrm.a;
    return float4(diff + spec, 1.0f);
}

///////////////////////////////////////////////////////////////////////////////
// Technique. Single vs_2_0/ps_2_0 path — the flat-normal fallback texture makes
// the bump shader degrade to flat sun lighting when a slot has no companion
// normal map, so no separate "gloss" technique is needed. (ps_1_x is rejected
// by the modern D3DX compiler anyway: X3539.)
///////////////////////////////////////////////////////////////////////////////
technique bump
{
    pass p0
    {
        ZEnable          = TRUE;
        ZWriteEnable     = TRUE;
        ZFunc            = LESSEQUAL;
        AlphaBlendEnable = FALSE;
        CullMode         = NONE;
        VertexShader     = compile vs_2_0 bump_vs();
        PixelShader      = compile ps_2_0 bump_ps();
    }
}
