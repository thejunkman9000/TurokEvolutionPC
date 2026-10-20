// Final scale of the EFB into the window, with picture adjustments:
// gamma (mid-tones), contrast (around mid-grey) and brightness (gain).
// Used through SpriteBatch, which supplies the vertex shader.
#if OPENGL
#define PS_SHADERMODEL ps_3_0
#else
#define PS_SHADERMODEL ps_4_0
#endif

Texture2D SpriteTexture;
sampler2D SpriteSampler = sampler_state { Texture = <SpriteTexture>; };
float Brightness;  // 1 = unchanged
float Contrast;    // 1 = unchanged
float Gamma;       // 1 = unchanged; > 1 brightens mid-tones

float4 MainPS(float4 pos : SV_POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) : COLOR
{
    float3 c = tex2D(SpriteSampler, uv).rgb;
    c = pow(saturate(c), 1.0 / Gamma);
    c = (c - 0.5) * Contrast + 0.5;
    c *= Brightness;
    return float4(saturate(c), 1.0);
}

technique Present
{
    pass P0
    {
        PixelShader = compile PS_SHADERMODEL MainPS();
    }
};
