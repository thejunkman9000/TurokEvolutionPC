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
float4 IndCmd0[16];   // bt, fmt, bias, bump-alpha select
float4 IndCmd1[16];   // matrix index, matrix id, wrap s*8 + wrap t, add previous
float4 IndMtx[6];     // rows of the 3 indirect matrices; w = shift
float4 IndStage[4];   // tex map, texcoord, s shift, t shift
float4 CoordSize[8];  // setup-unit size per texcoord
float4 IndMisc;       // number of indirect stages, indirect path enabled
float4 TexSize[8];    // dimensions of each bound tex map; z = mip level count

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

float4 SampleMapGrad(int map, float2 uv, float2 dx, float2 dy)
{
    if (map == 0) return tex2Dgrad(Samp0, uv, dx, dy);
    if (map == 1) return tex2Dgrad(Samp1, uv, dx, dy);
    if (map == 2) return tex2Dgrad(Samp2, uv, dx, dy);
    if (map == 3) return tex2Dgrad(Samp3, uv, dx, dy);
    if (map == 4) return tex2Dgrad(Samp4, uv, dx, dy);
    if (map == 5) return tex2Dgrad(Samp5, uv, dx, dy);
    if (map == 6) return tex2Dgrad(Samp6, uv, dx, dy);
    return tex2Dgrad(Samp7, uv, dx, dy);
}

// ---- TEV combiner, integer like the hardware (Dolphin's semantics) ----
// Colours are 8-bit (0..255) inputs; registers hold 11-bit signed values (-1024..1023).
int4 Q8(float4 v) { return int4(round(saturate(v) * 255.0)); }
int4 ISwizzle(int4 v, float4 t) { return int4(v[(int)t.x], v[(int)t.y], v[(int)t.z], v[(int)t.w]); }

// Linear interpolate, bias, add/subtract, scale (op = bias (3 = compare), scale, subtract, clamp).
// C is widened to 0..256, and the shift is folded into the lerp with the hardware's rounding.
int3 TevLerp(int3 A, int3 B, int3 C, int3 D, int bias, bool sub, bool alpha, int shift)
{
    C += C >> 7;
    if (bias == 1) D += 128; else if (bias == 2) D -= 128;
    int3 l = (A << 8) + (B - A) * C;
    if (shift != 3) { l = l << shift; D = D << shift; }
    if ((shift == 3) == alpha) l += sub ? 127 : 128;
    int3 r = l >> 8;
    r = sub ? D - r : D + r;
    if (shift == 3) r = r >> 1;
    return r;
}

int3 TevColor(int3 a, int3 b, int3 c, int3 d, float4 op)
{
    int3 r;
    if (op.x == 3.0)
    {
        // compare mode: d + (a CMP b ? c : 0); R8, GR16, BGR24 compare packed values, RGB8 per channel
        int cmp = (int)op.y * 2 + (int)op.z;
        int va, vb;
        if (cmp < 2)      { va = a.r; vb = b.r; }
        else if (cmp < 4) { va = (a.g << 8) | a.r; vb = (b.g << 8) | b.r; }
        else              { va = (a.b << 16) | (a.g << 8) | a.r; vb = (b.b << 16) | (b.g << 8) | b.r; }
        if (cmp == 6)      r = int3(a.r > b.r ? c.r : 0, a.g > b.g ? c.g : 0, a.b > b.b ? c.b : 0);
        else if (cmp == 7) r = int3(a.r == b.r ? c.r : 0, a.g == b.g ? c.g : 0, a.b == b.b ? c.b : 0);
        else if (cmp & 1)  r = (va == vb) ? c : int3(0, 0, 0);
        else               r = (va > vb) ? c : int3(0, 0, 0);
        r = d + r;
    }
    else r = TevLerp(a, b, c, d, (int)op.x, op.z > 0.5, false, (int)op.y);
    return op.w > 0.5 ? clamp(r, 0, 255) : clamp(r, -1024, 1023);
}

int TevAlpha(int a, int b, int c, int d, float4 op)
{
    int r;
    if (op.x == 3.0)
    {
        int cmp = (int)op.y * 2 + (int)op.z;
        bool hit = (cmp & 1) ? (a == b) : (a > b);
        r = d + (hit ? c : 0);
    }
    else r = TevLerp(int3(a, a, a), int3(b, b, b), int3(c, c, c), int3(d, d, d), (int)op.x, op.z > 0.5, true, (int)op.y).x;
    return op.w > 0.5 ? clamp(r, 0, 255) : clamp(r, -1024, 1023);
}

bool AlphaCompare(int a, int ref, float func)
{
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
    if (Misc.z == 5.0 && IndMisc.y != 0.0) return float4(1, 0, 1, 1);  // debug view 5: indirect draws

    // ---- indirect texturing (GX fixed point: texel * 128) ----
    bool indPath = IndMisc.y != 0.0;
    int numInd = (int)IndMisc.x;
    float2 duvdx[8], duvdy[8];
    int2 fixuv[8];
    int3 iindtex[4] = { int3(0, 0, 0), int3(0, 0, 0), int3(0, 0, 0), int3(0, 0, 0) };
    [unroll] for (int q = 0; q < 8; q++)
    {
        duvdx[q] = ddx(uv[q]); duvdy[q] = ddy(uv[q]);
        fixuv[q] = int2(uv[q] * CoordSize[q].xy * 128.0);
    }
    if (indPath)
    {
        for (int k = 0; k < numInd; k++)
        {
            int imap = (int)IndStage[k].x, icoord = (int)IndStage[k].y;
            int2 tc = fixuv[icoord] >> int2(IndStage[k].zw);
            float sc = 1.0 / exp2(IndStage[k].z);
            float4 c = SampleMapGrad(imap, float2(tc) / (128.0 * TexSize[imap].xy),
                                     duvdx[icoord] * sc, duvdy[icoord] * sc);
            iindtex[k] = int3(round(c.abg * 255.0));
        }
    }
    int2 tevcoord = int2(0, 0);
    int alphabump = 0;

    int4 reg[4] = { int4(round(Regs[0] * 255.0)), int4(round(Regs[1] * 255.0)),
                    int4(round(Regs[2] * 255.0)), int4(round(Regs[3] * 255.0)) };
    int4 result = reg[0];
    int n = (int)Misc.y;

    [loop]
    for (int s = 0; s < n; s++)
    {
        float4 st = Stage[s];
        float4 sw = Swap[s];
        float4 tex = st.x >= 0.0 ? texs[(int)st.x] : float4(0, 0, 0, 0);
        if (indPath)
        {
            float4 c0i = IndCmd0[s], c1i = IndCmd1[s];
            int coord = (int)sw.z;
            int bt = (int)c0i.x, fmt = (int)c0i.y, bias = (int)c0i.z, bs = (int)c0i.w;
            int mi = (int)c1i.x, mid = (int)c1i.y;
            int wrapS = (int)c1i.z / 8, wrapT = (int)c1i.z % 8;
            bool hasInd = bt < numInd;
            if (hasInd && bs != 0)
            {
                int ashift = fmt == 0 ? 0 : (fmt == 1 ? 5 : (fmt == 2 ? 4 : 3));
                alphabump = (iindtex[bt][bs - 1] << ashift) & 248;
            }
            int2 trans = int2(0, 0);
            if (hasInd && mi != 0)
            {
                int fshift = fmt == 0 ? 0 : (fmt == 1 ? 3 : (fmt == 2 ? 4 : 5));
                int3 crd = iindtex[bt] >> fshift;
                int badd = fmt == 0 ? -128 : 1;
                if (bias & 1) crd.x += badd;
                if (bias & 2) crd.y += badd;
                if (bias & 4) crd.z += badd;
                int r = 2 * (mi - 1);
                if (mid == 0) trans = int2(dot(int3(IndMtx[r].xyz), crd), dot(int3(IndMtx[r + 1].xyz), crd)) >> 3;
                else if (mid == 1) trans = (fixuv[coord] * crd.xx) >> 8;
                else trans = (fixuv[coord] * crd.yy) >> 8;
                int w = (int)IndMtx[r].w;
                trans = w >= 0 ? (trans >> w) : (trans << (-w));
            }
            int2 wrapped;
            wrapped.x = wrapS == 0 ? fixuv[coord].x : (wrapS >= 6 ? 0 : (fixuv[coord].x & (((256 >> (wrapS - 1)) << 7) - 1)));
            wrapped.y = wrapT == 0 ? fixuv[coord].y : (wrapT >= 6 ? 0 : (fixuv[coord].y & (((256 >> (wrapT - 1)) << 7) - 1)));
            tevcoord = c1i.w != 0.0 ? tevcoord + wrapped + trans : wrapped + trans;
            tevcoord = (tevcoord << 8) >> 8;  // s24 wrap
            if (st.x >= 0.0)
            {
                int map = (int)st.x;
                tex = SampleMapGrad(map, float2(tevcoord) / (128.0 * TexSize[map].xy), duvdx[coord], duvdy[coord]);
            }
        }
        int4 itex = ISwizzle(Q8(tex), SwapTable[(int)sw.y]);
        int abn = alphabump | (alphabump >> 5);
        int4 ras = st.y == 0.0 ? Q8(i.C0) : (st.y == 1.0 ? Q8(i.C1) : (st.y == 2.0 ? int4(alphabump, alphabump, alphabump, alphabump) :
                   (st.y == 3.0 ? int4(abn, abn, abn, abn) : int4(0, 0, 0, 0))));
        ras = ISwizzle(ras, SwapTable[(int)sw.x]);
        int4 k = Q8(Konst[s]);

        int3 cin[16];
        cin[0] = reg[0].rgb; cin[1] = reg[0].aaa; cin[2] = reg[1].rgb; cin[3] = reg[1].aaa;
        cin[4] = reg[2].rgb; cin[5] = reg[2].aaa; cin[6] = reg[3].rgb; cin[7] = reg[3].aaa;
        cin[8] = itex.rgb; cin[9] = itex.aaa; cin[10] = ras.rgb; cin[11] = ras.aaa;
        cin[12] = int3(255, 255, 255); cin[13] = int3(128, 128, 128); cin[14] = k.rgb; cin[15] = int3(0, 0, 0);
        int ain[8];
        ain[0] = reg[0].a; ain[1] = reg[1].a; ain[2] = reg[2].a; ain[3] = reg[3].a;
        ain[4] = itex.a; ain[5] = ras.a; ain[6] = k.a; ain[7] = 0;

        float4 ci = ColorIn[s], ai = AlphaIn[s];
        // a, b, c use the low 8 bits of their input; d keeps the full signed 11-bit range
        int3 col = TevColor(cin[(int)ci.x] & 255, cin[(int)ci.y] & 255, cin[(int)ci.z] & 255,
                            cin[(int)ci.w], ColorOp[s]);
        int alp = TevAlpha(ain[(int)ai.x] & 255, ain[(int)ai.y] & 255, ain[(int)ai.z] & 255,
                           ain[(int)ai.w], AlphaOp[s]);
        reg[(int)st.z].rgb = col;
        reg[(int)st.w].a = alp;
        result = int4(col, alp);
    }
    // the last stage's output is what leaves the TEV, wrapped to 8 bits
    result &= 255;

    // (Cut-out foliage keeps its coverage at a distance through its mip levels:
    // TurokGame.PreserveCoverage.)
    int testA = result.a;
    bool p0 = AlphaCompare(testA, (int)round(AlphaTest.x * 255.0), AlphaTest.z);
    bool p1 = AlphaCompare(testA, (int)round(AlphaTest.y * 255.0), AlphaTest.w);
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
        int ifog = (int)round(f * 256.0);
        result.rgb = (result.rgb * (256 - ifog) + Q8(FogColor).rgb * ifog) >> 8;
    }
    else if (Misc.z == 4.0) return float4(0, 0, 0.25, 1);  // no fog on this draw

    return float4(result) / 255.0;
}

technique Tev
{
    pass P0
    {
        VertexShader = compile VS_SHADERMODEL MainVS();
        PixelShader = compile PS_SHADERMODEL MainPS();
    }
};
