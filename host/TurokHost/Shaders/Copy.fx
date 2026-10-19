// EFB-to-texture copy: channel conversion applied while copying a region of the
// EFB render target into a copy texture (see CORE_COPY_* in core_api.h).
// Used through SpriteBatch, which supplies the vertex shader.
#if OPENGL
#define PS_SHADERMODEL ps_3_0
#else
#define PS_SHADERMODEL ps_4_0
#endif

Texture2D SpriteTexture;
sampler2D SpriteSampler = sampler_state { Texture = <SpriteTexture>; };
float Mode;

float4 MainPS(float4 pos : SV_POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
    float4 c = tex2D(SpriteSampler, uv);
    // GX intensity conversion (BT.601, limited range)
    float y = saturate(dot(c.rgb, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0);
    int m = (int)Mode;
    if (m == 1) return c.rrrr;
    if (m == 2) return float4(c.rrr, c.a);
    if (m == 3) return c.aaaa;
    if (m == 4) return c.gggg;
    if (m == 5) return c.bbbb;
    if (m == 6) return float4(y, y, y, y);
    if (m == 7) return float4(y, y, y, c.a);
    if (m == 8) return float4(c.rgb, 1);
    if (m == 9) return float4(c.rrr, c.g);
    if (m == 10) return float4(c.ggg, c.b);
    return c;
}

technique Copy
{
    pass P0
    {
        PixelShader = compile PS_SHADERMODEL MainPS();
    }
};
