// Decode the BP-side pixel pipeline (TEV stages, konst/registers, swap tables,
// alpha test, blend/depth/cull) into a shader-ready CoreMaterial.
//
// Register layouts (BP):
//   0x00       GENMODE: ntexgen 0-3, ncolor 4-5, ntev 10-13 (+1), cull 14-15
//   0x28-0x2F  TREF: per stage pair; even stage map 0-2, coord 3-5, enable 6,
//              channel 7-9; odd stage at +12
//   0xC0+2i    TEV_COLOR_ENV: d 0-3, c 4-7, b 8-11, a 12-15, bias 16-17,
//              sub 18, clamp 19, scale 20-21, dest 22-23
//   0xC1+2i    TEV_ALPHA_ENV: rswap 0-1, tswap 2-3, d 4-6, c 7-9, b 10-12,
//              a 13-15, bias 16-17, sub 18, clamp 19, scale 20-21, dest 22-23
//   0xE0-0xE7  TEV_REGISTER L/H pairs (PREV, C0, C1, C2): L = R 0-10, A 12-22,
//              H = B 0-10, G 12-22, bit 23 = konst register
//   0xF3       ALPHACOMPARE: ref0 0-7, ref1 8-15, comp0 16-18, comp1 19-21, logic 22-23
//   0xF6-0xFD  KSEL: swap 0-3, kcsel0 4-8, kasel0 9-13, kcsel1 14-18, kasel1 19-23
#include <math.h>
#include "ppc.h"
#include "host.h"
#include "gx.h"
#include "core_api.h"

uint32_t gx_bp(int reg);
void gx_viewport_z(float *scale, float *offset);

// TEV colour registers (11-bit signed) and konst registers (8-bit), RGBA.
static int16_t s_tevreg[4][4];
static uint8_t s_konst[4][4];

// BP writes to 0xE0..0xE7 come through here (gx_render.c forwards them).
void gx_material_bp(uint8_t reg, uint32_t v) {
    if (reg < 0xE0 || reg > 0xE7) return;
    int idx = (reg - 0xE0) >> 1, high = reg & 1, konst = (v >> 23) & 1;
    int lo = (int16_t)((v & 0x7FF) << 5) >> 5, hi = (int16_t)(((v >> 12) & 0x7FF) << 5) >> 5;
    if (!high) {  // R, A
        if (konst) { s_konst[idx][0] = (uint8_t)lo; s_konst[idx][3] = (uint8_t)hi; }
        else { s_tevreg[idx][0] = (int16_t)lo; s_tevreg[idx][3] = (int16_t)hi; }
    } else {      // B, G
        if (konst) { s_konst[idx][2] = (uint8_t)lo; s_konst[idx][1] = (uint8_t)hi; }
        else { s_tevreg[idx][2] = (int16_t)lo; s_tevreg[idx][1] = (int16_t)hi; }
    }
}

static const float s_kfrac[8] = {1.0f, 7 / 8.0f, 6 / 8.0f, 5 / 8.0f, 4 / 8.0f, 3 / 8.0f, 2 / 8.0f, 1 / 8.0f};

static void konst_color(int sel, float out[3]) {
    if (sel < 8) { out[0] = out[1] = out[2] = s_kfrac[sel]; return; }
    if (sel >= 12 && sel <= 15) {
        for (int i = 0; i < 3; i++) out[i] = s_konst[sel - 12][i] / 255.0f;
        return;
    }
    if (sel >= 16) {  // 16-19 r, 20-23 g, 24-27 b, 28-31 a of K0..K3
        int k = (sel - 16) & 3, ch = (sel - 16) >> 2;
        out[0] = out[1] = out[2] = s_konst[k][ch] / 255.0f;
        return;
    }
    out[0] = out[1] = out[2] = 0;
}

static float konst_alpha(int sel) {
    if (sel < 8) return s_kfrac[sel];
    if (sel >= 16) { int k = (sel - 16) & 3, ch = (sel - 16) >> 2; return s_konst[k][ch] / 255.0f; }
    return 0;
}

// The tex maps referenced by enabled TEV stages (bit n = map n), and the
// texcoord each one is sampled with.
uint32_t gx_material_build(CoreMaterial *m) {
    memset(m, 0, sizeof *m);
    uint32_t gen = gx_bp(0x00);
    int nstages = ((gen >> 10) & 15) + 1;
    uint32_t used_maps = 0;
    int map_coord[8] = {0};
    int map_seen[8] = {0};

    for (int s = 0; s < 16; s++) {
        uint32_t tref = gx_bp(0x28 + s / 2) >> ((s & 1) * 12);
        int map = tref & 7, coord = (tref >> 3) & 7, en = (tref >> 6) & 1, chan = (tref >> 7) & 7;
        uint32_t ce = gx_bp(0xC0 + 2 * s), ae = gx_bp(0xC1 + 2 * s);
        uint32_t ks = gx_bp(0xF6 + s / 2) >> ((s & 1) * 10);
        int kc = (ks >> 4) & 31, ka = (ks >> 9) & 31;

        m->color_in[s][0] = (float)((ce >> 12) & 15); m->color_in[s][1] = (float)((ce >> 8) & 15);
        m->color_in[s][2] = (float)((ce >> 4) & 15);  m->color_in[s][3] = (float)(ce & 15);
        m->alpha_in[s][0] = (float)((ae >> 13) & 7);  m->alpha_in[s][1] = (float)((ae >> 10) & 7);
        m->alpha_in[s][2] = (float)((ae >> 7) & 7);   m->alpha_in[s][3] = (float)((ae >> 4) & 7);
        m->color_op[s][0] = (float)((ce >> 16) & 3);  m->color_op[s][1] = (float)((ce >> 20) & 3);
        m->color_op[s][2] = (float)((ce >> 18) & 1);  m->color_op[s][3] = (float)((ce >> 19) & 1);
        m->alpha_op[s][0] = (float)((ae >> 16) & 3);  m->alpha_op[s][1] = (float)((ae >> 20) & 3);
        m->alpha_op[s][2] = (float)((ae >> 18) & 1);  m->alpha_op[s][3] = (float)((ae >> 19) & 1);

        int use_map = en && s < nstages;
        m->stage[s][0] = use_map ? (float)map : -1.0f;
        m->stage[s][1] = chan == 0 ? 0.0f : chan == 1 ? 1.0f : chan == 5 ? 2.0f : chan == 6 ? 3.0f : -1.0f;
        m->stage[s][2] = (float)((ce >> 22) & 3);
        m->stage[s][3] = (float)((ae >> 22) & 3);
        m->swap[s][0] = (float)(ae & 3);
        m->swap[s][1] = (float)((ae >> 2) & 3);
        m->swap[s][2] = (float)coord;  // stage texcoord (used by indirect ops)

        float kcol[3];
        konst_color(kc, kcol);
        m->konst[s][0] = kcol[0]; m->konst[s][1] = kcol[1]; m->konst[s][2] = kcol[2];
        m->konst[s][3] = konst_alpha(ka);

        if (use_map) {
            used_maps |= 1u << map;
            if (!map_seen[map]) { map_seen[map] = 1; map_coord[map] = coord; }
        }
    }
    for (int i = 0; i < 8; i++) m->map_coord[i / 4][i % 4] = (float)map_coord[i];

    // swap tables: table t = r,g from KSEL[2t] bits 0-3, b,a from KSEL[2t+1]
    for (int t = 0; t < 4; t++) {
        uint32_t k0 = gx_bp(0xF6 + 2 * t), k1 = gx_bp(0xF7 + 2 * t);
        m->swap_table[t][0] = (float)(k0 & 3); m->swap_table[t][1] = (float)((k0 >> 2) & 3);
        m->swap_table[t][2] = (float)(k1 & 3); m->swap_table[t][3] = (float)((k1 >> 2) & 3);
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m->regs[r][c] = s_tevreg[r][c] / 255.0f;

    uint32_t ac = gx_bp(0xF3);
    m->alpha_test[0] = (ac & 0xFF) / 255.0f; m->alpha_test[1] = ((ac >> 8) & 0xFF) / 255.0f;
    m->alpha_test[2] = (float)((ac >> 16) & 7); m->alpha_test[3] = (float)((ac >> 19) & 7);
    m->misc[0] = (float)((ac >> 22) & 3);
    m->misc[1] = (float)nstages;

    // Fog. A and C are 20-bit floats: mantissa 0-10, exponent 11-18, sign 19.
    {
        uint32_t f0 = gx_bp(0xEE), f3 = gx_bp(0xF1), col = gx_bp(0xF2);
        uint32_t ia = ((f0 >> 19) & 1) << 31 | ((f0 >> 11) & 0xFF) << 23 | (f0 & 0x7FF) << 12;
        uint32_t ic = ((f3 >> 19) & 1) << 31 | ((f3 >> 11) & 0xFF) << 23 | (f3 & 0x7FF) << 12;
        float a, c;
        memcpy(&a, &ia, 4); memcpy(&c, &ic, 4);
        m->fog[0] = a; m->fog[1] = c;
        m->fog[2] = (float)(gx_bp(0xEF) & 0xFFFFFF);
        m->fog[3] = (float)(gx_bp(0xF0) & 0x1F);
        m->fog_mode[0] = (float)((f3 >> 21) & 7);
        static uint32_t s_last_fog;
        if (((f3 >> 21) & 7) && f3 != s_last_fog) {
            static int budget = 10;
            s_last_fog = f3;
            if (budget-- > 0) logf_("[fog] fsel %u ortho %u a %g c %g b %u>>%u color %06X", (f3 >> 21) & 7, (f3 >> 20) & 1,
                                    a, c, gx_bp(0xEF) & 0xFFFFFF, gx_bp(0xF0) & 0x1F, col & 0xFFFFFF);
        }
        m->fog_mode[1] = (float)((f3 >> 20) & 1);
        gx_viewport_z(&m->fog_mode[2], &m->fog_mode[3]);
        m->fog_color[0] = ((col >> 16) & 0xFF) / 255.0f;
        m->fog_color[1] = ((col >> 8) & 0xFF) / 255.0f;
        m->fog_color[2] = (col & 0xFF) / 255.0f;
    }
    // ---- indirect texturing ----
    {
        uint32_t nind = (gen >> 16) & 7;
        int any = 0;
        for (int s = 0; s < 16; s++) {
            uint32_t ic = gx_bp(0x10 + s);
            m->ind_cmd0[s][0] = (float)(ic & 3);          // bt
            m->ind_cmd0[s][1] = (float)((ic >> 2) & 3);   // fmt
            m->ind_cmd0[s][2] = (float)((ic >> 4) & 7);   // bias
            m->ind_cmd0[s][3] = (float)((ic >> 7) & 3);   // bump alpha
            m->ind_cmd1[s][0] = (float)((ic >> 9) & 3);   // matrix index
            m->ind_cmd1[s][1] = (float)((ic >> 11) & 3);  // matrix id
            m->ind_cmd1[s][2] = (float)(((ic >> 13) & 7) * 8 + ((ic >> 16) & 7));  // wrap s, t
            m->ind_cmd1[s][3] = (float)((ic >> 20) & 1);  // add previous
            if (s < nstages && (ic & 0x1FFF80)) any = 1;  // matrix, wrap, addprev or bump alpha in use
        }
        for (int mi = 0; mi < 3; mi++) {
            uint32_t a = gx_bp(0x06 + 3 * mi), b = gx_bp(0x07 + 3 * mi), c = gx_bp(0x08 + 3 * mi);
            #define S11(v) ((float)((int32_t)((v) << 21) >> 21))
            int scale = (int)(((a >> 22) & 3) | ((b >> 22) & 3) << 2 | ((c >> 22) & 3) << 4);
            m->ind_mtx[2 * mi][0] = S11(a & 0x7FF);         m->ind_mtx[2 * mi][1] = S11(b & 0x7FF);
            m->ind_mtx[2 * mi][2] = S11(c & 0x7FF);         m->ind_mtx[2 * mi][3] = (float)(17 - scale);
            m->ind_mtx[2 * mi + 1][0] = S11((a >> 11) & 0x7FF); m->ind_mtx[2 * mi + 1][1] = S11((b >> 11) & 0x7FF);
            m->ind_mtx[2 * mi + 1][2] = S11((c >> 11) & 0x7FF); m->ind_mtx[2 * mi + 1][3] = (float)(17 - scale);
            #undef S11
        }
        uint32_t iref = gx_bp(0x27), ss0 = gx_bp(0x25), ss1 = gx_bp(0x26);
        for (int i = 0; i < 4; i++) {
            uint32_t ss = i < 2 ? ss0 : ss1, sh = (i & 1) * 8;
            m->ind_stage[i][0] = (float)((iref >> (6 * i)) & 7);
            m->ind_stage[i][1] = (float)((iref >> (6 * i + 3)) & 7);
            m->ind_stage[i][2] = (float)((ss >> sh) & 15);
            m->ind_stage[i][3] = (float)((ss >> (sh + 4)) & 15);
        }
        for (int t = 0; t < 8; t++) {
            m->coord_size[t][0] = (float)((gx_bp(0x30 + 2 * t) & 0xFFFF) + 1);
            m->coord_size[t][1] = (float)((gx_bp(0x31 + 2 * t) & 0xFFFF) + 1);
        }
        m->ind_misc[0] = (float)nind;
        m->ind_misc[1] = (float)(any || nind);
        // indirect stage texture maps are sampled too
        for (uint32_t i = 0; i < nind; i++) {
            uint32_t map = (iref >> (6 * i)) & 7;
            if (!(used_maps & (1u << map))) { used_maps |= 1u << map; map_coord[map] = (iref >> (6 * i + 3)) & 7; }
        }
        for (int i = 0; i < 8; i++) m->map_coord[i / 4][i % 4] = (float)map_coord[i];
    }
    m->blend = gx_bp(0x41);
    m->zmode = gx_bp(0x40);
    m->cull = (gen >> 14) & 3;
    return used_maps;
}
