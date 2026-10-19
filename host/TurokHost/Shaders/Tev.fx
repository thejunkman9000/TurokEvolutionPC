// GameCube TEV (texture environment) ubershader.
// Parameters are a decoded CoreMaterial (recomp/runtime/core_api.h); vertices
// arrive in view space with both colour channels lit and texgens applied.
#if OPENGL
#define VS_SHADERMODEL vs_3_0
#define PS_SHADERMODEL ps_3_0
#else
#define VS_SHADERMODEL vs_4_0
#define PS_SHADERMODEL ps_4_0
#endif

float4x4 Proj;
float4 ColorIn[16];   // a, b, c, d (GX_CC_*)
float4 AlphaIn[16];   // a, b, c, d (GX_CA_*)
float4 ColorOp[16];   // bias (3 = compare), scale, subtract, clamp
float4 AlphaOp[16];
float4 Stage[16];     // tex map (-1), ras channel (0, 1, -1), colour dest, alpha dest
float4 Swap[16];      // ras swap table, tex swap table
float4 Konst[16];     // per-stage konst colour / alpha
float4 SwapTable[4];
float4 Regs[4];       // PREV, C0, C1, C2
float4 MapCoord[2];   // texcoord index per tex map
float4 AlphaTest;     // ref0, ref1, comp0, comp1
float4 Misc;          // logic op, stage count, debug view
float4 Fog;           // a, c, b magnitude, b shift
float4 FogMode;       // fsel, ortho, viewport z scale, z offset
float4 FogColor;

Texture2D Tex0; sampler2D Samp0 = sampler_state { Texture = <Tex0>; };
Texture2D Tex1; sampler2D Samp1 = sampler_state { Texture = <Tex1>; };
Texture2D Tex2; sampler2D Samp2 = sampler_state { Texture = <Tex2>; };
Texture2D Tex3; sampler2D Samp3 = sampler_state { Texture = <Tex3>; };
Texture2D Tex4; sampler2D Samp4 = sampler_state { Texture = <Tex4>; };
Texture2D Tex5; sampler2D Samp5 = sampler_state { Texture = <Tex5>; };
Texture2D Tex6; sampler2D Samp6 = sampler_state { Texture = <Tex6>; };
Texture2D Tex7; sampler2D Samp7 = sampler_state { Texture = <Tex7>; };

struct VSIn
{
    float3 Pos : POSITION0;
    float4 C0 : COLOR0;
    float4 C1 : COLOR1;
    float2 T0 : TEXCOORD0; float2 T1 : TEXCOORD1; float2 T2 : TEXCOORD2; float2 T3 : TEXCOORD3;
    float2 T4 : TEXCOORD4; float2 T5 : TEXCOORD5; float2 T6 : TEXCOORD6; float2 T7 : TEXCOORD7;
};

struct VSOut
{
    float4 Pos : SV_POSITION;
    float4 C0 : COLOR0;
    float4 C1 : COLOR1;
    float2 T0 : TEXCOORD0; float2 T1 : TEXCOORD1; float2 T2 : TEXCOORD2; float2 T3 : TEXCOORD3;
    float2 T4 : TEXCOORD4; float2 T5 : TEXCOORD5; float2 T6 : TEXCOORD6; float2 T7 : TEXCOORD7;
};

VSOut MainVS(VSIn i)
{
    VSOut o;
    o.Pos = mul(float4(i.Pos, 1.0), Proj);
    o.C0 = i.C0; o.C1 = i.C1;
    o.T0 = i.T0; o.T1 = i.T1; o.T2 = i.T2; o.T3 = i.T3;
    o.T4 = i.T4; o.T5 = i.T5; o.T6 = i.T6; o.T7 = i.T7;
    return o;
}

float4 Swizzle(float4 v, float4 t)
{
    return float4(v[(int)t.x], v[(int)t.y], v[(int)t.z], v[(int)t.w]);
}

float Bias(float b) { return b == 1.0 ? 0.5 : (b == 2.0 ? -0.5 : 0.0); }
float Scale(float s) { return s == 1.0 ? 2.0 : (s == 2.0 ? 4.0 : (s == 3.0 ? 0.5 : 1.0)); }

float3 TevColor(float3 a, float3 b, float3 c, float3 d, float4 op)
{
    float3 r;
    if (op.x == 3.0)
    {
        // compare mode: d + (a CMP b ? c : 0)
        int cmp = (int)op.y * 2 + (int)op.z;
        float3 qa = round(a * 255.0), qb = round(b * 255.0);
        float va, vb;
        if (cmp < 2)      { va = qa.r; vb = qb.r; }
        else if (cmp < 4) { va = qa.g * 256.0 + qa.r; vb = qb.g * 256.0 + qb.r; }
        else              { va = qa.b * 65536.0 + qa.g * 256.0 + qa.r; vb = qb.b * 65536.0 + qb.g * 256.0 + qb.r; }
        if (cmp == 6)      r = float3(qa.r > qb.r ? c.r : 0, qa.g > qb.g ? c.g : 0, qa.b > qb.b ? c.b : 0);
        else if (cmp == 7) r = float3(qa.r == qb.r ? c.r : 0, qa.g == qb.g ? c.g : 0, qa.b == qb.b ? c.b : 0);
        else if (cmp & 1)  r = (va == vb) ? c : float3(0, 0, 0);
        else               r = (va > vb) ? c : float3(0, 0, 0);
        r = d + r;
    }
    else
    {
        float3 l = lerp(a, b, c);
        r = (op.z > 0.5 ? d - l : d + l) + Bias(op.x);
        r *= Scale(op.y);
    }
    return op.w > 0.5 ? saturate(r) : clamp(r, -4.0, 4.0);
}

float TevAlpha(float a, float b, float c, float d, float4 op)
{
    float r;
    if (op.x == 3.0)
    {
        int cmp = (int)op.y * 2 + (int)op.z;
        float qa = round(a * 255.0), qb = round(b * 255.0);
        bool hit = (cmp & 1) ? (qa == qb) : (qa > qb);
        r = d + (hit ? c : 0.0);
    }
    else
    {
        float l = lerp(a, b, c);
        r = (op.z > 0.5 ? d - l : d + l) + Bias(op.x);
        r *= Scale(op.y);
    }
    return op.w > 0.5 ? saturate(r) : clamp(r, -4.0, 4.0);
}

bool AlphaCompare(float a, float ref, float func)
{
    a = round(a * 255.0); ref = round(ref * 255.0);
    if (func == 0.0) return false;
    if (func == 1.0) return a < ref;
    if (func == 2.0) return a == ref;
    if (func == 3.0) return a <= ref;
    if (func == 4.0) return a > ref;
    if (func == 5.0) return a != ref;
    if (func == 6.0) return a >= ref;
    return true;
}

float4 MainPS(VSOut i) : COLOR0
{
    float2 uv[8] = { i.T0, i.T1, i.T2, i.T3, i.T4, i.T5, i.T6, i.T7 };
    float4 texs[8];
    texs[0] = tex2D(Samp0, uv[(int)MapCoord[0].x]);
    texs[1] = tex2D(Samp1, uv[(int)MapCoord[0].y]);
    texs[2] = tex2D(Samp2, uv[(int)MapCoord[0].z]);
    texs[3] = tex2D(Samp3, uv[(int)MapCoord[0].w]);
    texs[4] = tex2D(Samp4, uv[(int)MapCoord[1].x]);
    texs[5] = tex2D(Samp5, uv[(int)MapCoord[1].y]);
    texs[6] = tex2D(Samp6, uv[(int)MapCoord[1].z]);
    texs[7] = tex2D(Samp7, uv[(int)MapCoord[1].w]);

    // Debug views (TUROK_DEBUG_VIEW): 1 stage-0 texture, 2 texcoord 0, 3 colour channel 0
    if (Misc.z == 1.0) return float4((Stage[0].x >= 0.0 ? texs[(int)Stage[0].x] : float4(1, 0, 1, 1)).rgb, 1);
    if (Misc.z == 2.0) return float4(frac(i.T0), 0, 1);
    if (Misc.z == 3.0) return float4(i.C0.rgb, 1);

    float4 reg[4] = { Regs[0], Regs[1], Regs[2], Regs[3] };
    float4 result = reg[0];
    int n = (int)Misc.y;

    [loop]
    for (int s = 0; s < n; s++)
    {
        float4 st = Stage[s];
        float4 sw = Swap[s];
        float4 tex = st.x >= 0.0 ? texs[(int)st.x] : float4(0, 0, 0, 0);
        tex = Swizzle(tex, SwapTable[(int)sw.y]);
        float4 ras = st.y == 0.0 ? i.C0 : (st.y == 1.0 ? i.C1 : float4(0, 0, 0, 0));
        ras = Swizzle(ras, SwapTable[(int)sw.x]);
        float4 k = Konst[s];

        float3 cin[16];
        cin[0] = reg[0].rgb; cin[1] = reg[0].aaa; cin[2] = reg[1].rgb; cin[3] = reg[1].aaa;
        cin[4] = reg[2].rgb; cin[5] = reg[2].aaa; cin[6] = reg[3].rgb; cin[7] = reg[3].aaa;
        cin[8] = tex.rgb; cin[9] = tex.aaa; cin[10] = ras.rgb; cin[11] = ras.aaa;
        cin[12] = float3(1, 1, 1); cin[13] = float3(0.5, 0.5, 0.5); cin[14] = k.rgb; cin[15] = float3(0, 0, 0);
        float ain[8];
        ain[0] = reg[0].a; ain[1] = reg[1].a; ain[2] = reg[2].a; ain[3] = reg[3].a;
        ain[4] = tex.a; ain[5] = ras.a; ain[6] = k.a; ain[7] = 0.0;

        float4 ci = ColorIn[s], ai = AlphaIn[s];
        // a, b, c are 8-bit unsigned inputs; d keeps the full signed range
        float3 col = TevColor(saturate(cin[(int)ci.x]), saturate(cin[(int)ci.y]), saturate(cin[(int)ci.z]),
                              cin[(int)ci.w], ColorOp[s]);
        float alp = TevAlpha(saturate(ain[(int)ai.x]), saturate(ain[(int)ai.y]), saturate(ain[(int)ai.z]),
                             ain[(int)ai.w], AlphaOp[s]);
        reg[(int)st.z].rgb = col;
        reg[(int)st.w].a = alp;
        result = float4(col, alp);
    }

    bool p0 = AlphaCompare(result.a, AlphaTest.x, AlphaTest.z);
    bool p1 = AlphaCompare(result.a, AlphaTest.y, AlphaTest.w);
    bool ok;
    if (Misc.x == 0.0) ok = p0 && p1;
    else if (Misc.x == 1.0) ok = p0 || p1;
    else if (Misc.x == 2.0) ok = p0 != p1;
    else ok = p0 == p1;
    if (!ok) clip(-1);

    // Fog: rebuild 24-bit window Z (our clip z was remapped from GX [-w,0] to [0,w])
    float fsel = FogMode.x;
    if (fsel != 0.0)
    {
        float zs = clamp((i.Pos.z - 1.0) * FogMode.z + FogMode.w, 0.0, 16777215.0);
        float ze;
        if (FogMode.y == 0.0)  // perspective: A / (B_mag - (Zs >> B_shift))
            ze = Fog.x * 16777216.0 / (Fog.z - floor(zs / exp2(Fog.w)));
        else                   // orthographic: A * Zs
            ze = Fog.x * zs / 16777216.0;
        float f = saturate(ze - Fog.y);
        if (fsel == 4.0) f = 1.0 - exp2(-8.0 * f);
        else if (fsel == 5.0) f = 1.0 - exp2(-8.0 * f * f);
        else if (fsel == 6.0) f = exp2(-8.0 * (1.0 - f));
        else if (fsel == 7.0) { f = 1.0 - f; f = exp2(-8.0 * f * f); }
        if (Misc.z == 4.0) return float4(f, f, f, 1);  // debug view 4: fog amount
        result.rgb = lerp(saturate(result.rgb), FogColor.rgb, f);
    }
    else if (Misc.z == 4.0) return float4(0, 0, 0.25, 1);  // no fog on this draw

    return saturate(result);
}

technique Tev
{
    pass P0
    {
        VertexShader = compile VS_SHADERMODEL MainVS();
        PixelShader = compile PS_SHADERMODEL MainPS();
    }
};
