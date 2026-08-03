// Skydome.fx — samples an equirectangular environment texture onto a sphere
// rendered from inside.

float4x4 g_WorldViewProj : WORLDVIEWPROJECTION;
texture  g_Skydome;

sampler g_SkydomeSampler = sampler_state
{
    Texture = <g_Skydome>;
    MinFilter = LINEAR;
    MagFilter = LINEAR;
    MipFilter = LINEAR;
    AddressU  = WRAP;   // longitude wraps
    AddressV  = CLAMP;  // latitude clamps (avoids pole bleed)
};

struct VS_INPUT {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

VS_OUTPUT VS(VS_INPUT input)
{
    VS_OUTPUT o;
    o.Position = mul(float4(input.Position, 1.0), g_WorldViewProj);
    // Force the sphere to render at the far plane so depth-test (when on)
    // always passes for ground/particles. Even with ZTEST off this also
    // means the sphere never z-fights with anything.
    o.Position.z = o.Position.w * 0.9999;
    o.TexCoord = input.TexCoord;
    return o;
}

float4 PS(VS_OUTPUT input) : COLOR
{
    return tex2D(g_SkydomeSampler, input.TexCoord);
}

technique Skydome
{
    pass P0
    {
        VertexShader = compile vs_2_0 VS();
        PixelShader  = compile ps_2_0 PS();
    }
}
