// GX state tracking and the vertex pipeline: turns the parsed command stream
// into CoreFrame batches for the host renderer.
//
// Per draw, on the CPU:
//   * vertices are decoded (direct or indexed attributes) and transformed to
//     view space with the position matrix
//   * both colour channels are lit exactly like the XF unit (gx_light)
//   * all texgens run, including dual-transform post matrices
//   * the pixel pipeline (TEV, alpha test, blend, depth) is captured as a
//     CoreMaterial (gx_material.c) and evaluated by the host's TEV shader
// Not yet: EFB->texture copies, fog, indirect texturing, lines/points.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "ppc.h"
#include "host.h"
#include "gx.h"
#include "core_api.h"

uint32_t gx_material_build(CoreMaterial *m);
void gx_material_bp(uint8_t reg, uint32_t v);
int gx_texture_for_map(int map, CoreTexture *out);

// ---- CP / XF / BP state ---------------------------------------------------------
static uint32_t cp_vcd_lo, cp_vcd_hi, cp_vat_a[8], cp_vat_b[8], cp_vat_c[8];
static uint32_t cp_matidx_a, cp_matidx_b;
static uint32_t cp_array_base[16], cp_array_stride[16];

static float xf_mem[0x700];       // pos/tex mtx 0x000-0x0FF, normal 0x400-0x45F, post 0x500-0x5FF, lights 0x600-0x67F
static uint32_t xf_reg[0x60];     // registers 0x1000..0x105F
static uint32_t bp[0x100];
static uint8_t s_tmem_tlut[0x80000];  // upper TMEM half (TLUTs), 512 KB

#define XF_NUMCHAN   0x09
#define XF_AMBIENT0  0x0A
#define XF_MATERIAL0 0x0C
#define XF_COLOR0CTL 0x0E
#define XF_ALPHA0CTL 0x10
#define XF_DUALTEX   0x12
#define XF_MATIDX_A  0x18
#define XF_MATIDX_B  0x19
#define XF_VIEWPORT  0x1A
#define XF_PROJ      0x20
#define XF_NUMTEXGEN 0x3F
#define XF_TEXGEN0   0x40
#define XF_POSTTEX0  0x50

void gx_cp_write(uint8_t reg, uint32_t v) {
    switch (reg & 0xF0) {
    case 0x30: cp_matidx_a = v; break;
    case 0x40: cp_matidx_b = v; break;
    case 0x50: cp_vcd_lo = v; break;
    case 0x60: cp_vcd_hi = v; break;
    case 0x70: cp_vat_a[reg & 7] = v; break;
    case 0x80: cp_vat_b[reg & 7] = v; break;
    case 0x90: cp_vat_c[reg & 7] = v; break;
    case 0xA0: cp_array_base[reg & 15] = v & 0x03FFFFFF; break;
    case 0xB0: cp_array_stride[reg & 15] = v & 0xFF; break;
    }
}

uint32_t gx_cp_vcd_lo(void) { return cp_vcd_lo; }
uint32_t gx_cp_vcd_hi(void) { return cp_vcd_hi; }
uint32_t gx_cp_vat(int which, int fmt) {
    return which == 0 ? cp_vat_a[fmt] : which == 1 ? cp_vat_b[fmt] : cp_vat_c[fmt];
}

static void xf_store(uint32_t addr, uint32_t v) {
    if (addr < 0x700) { memcpy(&xf_mem[addr], &v, 4); return; }
    if (addr >= 0x1000 && addr < 0x1060) {
        xf_reg[addr - 0x1000] = v;
        if (addr == 0x1018) cp_matidx_a = v;   // XF copies of the matrix indices
        if (addr == 0x1019) cp_matidx_b = v;
    }
}

void gx_xf_write(uint32_t first, uint32_t count, const uint8_t *data) {
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = data + 4 * i;
        xf_store(first + i, (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
    }
}

void gx_load_indx(int array, uint32_t v) {  // LOAD_INDX_A..D: arrays 12..15
    uint32_t index = v >> 16, n = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    uint32_t src = cp_array_base[array] + index * cp_array_stride[array];
    for (uint32_t i = 0; i < n; i++) xf_store(addr + i, rd32(0x80000000u | (src + 4 * i)));
}

static uint32_t xf_u32(uint32_t addr) { uint32_t u; memcpy(&u, &xf_mem[addr], 4); return u; }

// ---- frame building -----------------------------------------------------------------
typedef struct {
    CoreFrame f;
    CoreVertex *vtx; uint32_t nvtx, capvtx;
    CoreBatch *bat; uint32_t nbat, capbat;
    CoreTexture *tex; uint32_t ntex, captex;
    CoreMaterial *mat; uint32_t nmat, capmat;
} FrameSlot;

static FrameSlot s_slots[3];
static FrameSlot *s_build = &s_slots[0], *s_ready, *s_held;
static FrameSlot *s_free_list[3] = {&s_slots[1], &s_slots[2]};
static int s_nfree = 2;
static CRITICAL_SECTION s_frame_lock;
static int s_lock_init;
static uint32_t s_serial;
static uint32_t s_clear_color = 0xFF000000u;

static void frame_lock(void) {
    if (!s_lock_init) { InitializeCriticalSection(&s_frame_lock); s_lock_init = 1; }
    EnterCriticalSection(&s_frame_lock);
}

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return p;
    uint32_t nc = *cap ? *cap : 64;
    while (nc < need) nc *= 2;
    p = realloc(p, nc * elem);
    *cap = nc;
    return p;
}

static void slot_reset(FrameSlot *s) { s->nvtx = s->nbat = s->ntex = s->nmat = 0; }

uint8_t *gx_tlut_mem(void) { return s_tmem_tlut; }
// XF viewport Z scale/offset (window Z = ndc_z * scale + offset, 24-bit units)
void gx_viewport_z(float *scale, float *offset) {
    memcpy(scale, &xf_reg[XF_VIEWPORT + 2], 4);
    memcpy(offset, &xf_reg[XF_VIEWPORT + 5], 4);
}
uint32_t gx_bp(int reg) { return bp[reg & 0xFF]; }

static int32_t frame_texture(int map) {
    CoreTexture t;
    if (!gx_texture_for_map(map, &t)) return -1;
    for (uint32_t i = 0; i < s_build->ntex; i++)
        if (s_build->tex[i].id == t.id) return (int32_t)i;
    s_build->tex = grow(s_build->tex, &s_build->captex, s_build->ntex + 1, sizeof(CoreTexture));
    s_build->tex[s_build->ntex] = t;
    return (int32_t)s_build->ntex++;
}

// Materials repeat a lot within a frame; dedupe against the last few.
static uint32_t frame_material(const CoreMaterial *m) {
    uint32_t n = s_build->nmat;
    for (uint32_t k = 0; k < 8 && k < n; k++)
        if (memcmp(&s_build->mat[n - 1 - k], m, sizeof *m) == 0) return n - 1 - k;
    s_build->mat = grow(s_build->mat, &s_build->capmat, n + 1, sizeof(CoreMaterial));
    s_build->mat[n] = *m;
    return s_build->nmat++;
}

void gx_texture_end_frame(void);

static void publish_frame(void) {
    FrameSlot *f = s_build;
    f->f.serial = ++s_serial;
    f->f.efb_width = 640; f->f.efb_height = 480;
    f->f.clear_color = s_clear_color;
    f->f.batch_count = f->nbat; f->f.vertex_count = f->nvtx;
    f->f.texture_count = f->ntex; f->f.material_count = f->nmat;
    f->f.batches = f->bat; f->f.vertices = f->vtx; f->f.textures = f->tex; f->f.materials = f->mat;
    frame_lock();
    if (s_ready) s_free_list[s_nfree++] = s_ready;  // host never took it: drop
    s_ready = f;
    s_build = s_free_list[--s_nfree];
    LeaveCriticalSection(&s_frame_lock);
    slot_reset(s_build);
    gx_texture_end_frame();
}

// ---- EFB -> texture copy ---------------------------------------------------------------
void gx_texture_register_efb_copy(uint32_t addr, uint32_t id, uint32_t w, uint32_t h, uint32_t bytes);
uint32_t gx_tex_bytes(uint32_t fmt, uint32_t w, uint32_t h);

static void efb_copy_to_texture(uint32_t v) {
    uint32_t tl = bp[0x49], wh = bp[0x4A];
    int32_t x = tl & 0x3FF, y = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    uint32_t dst = ((bp[0x4B] & 0xFFFFFF) << 5) & 0x01FFFFFF;
    uint32_t tf = (v >> 3) & 15, fmt = (tf >> 1) | ((tf & 1) << 3);
    int intensity = (v >> 15) & 1, half = (v >> 9) & 1, clear = (v >> 11) & 1;
    uint32_t mode;
    switch (fmt) {
    case 0: case 1: case 8: mode = intensity ? CORE_COPY_YYYY : CORE_COPY_RRRR; break;  // R4 / R8
    case 2: case 3: mode = intensity ? CORE_COPY_YYYA : CORE_COPY_RRRA; break;          // RA4 / RA8
    case 4: mode = CORE_COPY_RGB1; break;                                               // RGB565
    case 7: mode = CORE_COPY_AAAA; break;                                               // A8
    case 9: mode = CORE_COPY_GGGG; break;
    case 10: mode = CORE_COPY_BBBB; break;
    case 11: mode = CORE_COPY_RRRG; break;
    case 12: mode = CORE_COPY_GGGB; break;
    default: mode = CORE_COPY_RGBA; break;                                              // RGB5A3 / RGBA8
    }
    uint32_t id = 0x80000000u | (dst >> 5);
    uint32_t tw = half ? (uint32_t)w / 2 : (uint32_t)w, th = half ? (uint32_t)h / 2 : (uint32_t)h;
    // texture format a later read is likely to use (only sizes the hashed region)
    static const uint8_t tex_fmt_for_copy[16] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3, 0, 0, 0};
    gx_texture_register_efb_copy(dst, id, tw, th, gx_tex_bytes(tex_fmt_for_copy[fmt], tw, th));

    CoreBatch b;
    memset(&b, 0, sizeof b);
    b.kind = 1;
    b.copy_id = id;
    b.copy_mode = mode | (half ? 0x100u : 0) | (clear ? 0x200u : 0);
    b.copy_src[0] = x; b.copy_src[1] = y; b.copy_src[2] = w; b.copy_src[3] = h;
    b.clear_color = s_clear_color;
    b.clear_depth = (bp[0x51] & 0xFFFFFF) / 16777215.0f;
    b.first_vertex = s_build->nvtx;
    for (int i = 0; i < 8; i++) b.textures[i] = -1;
    s_build->bat = grow(s_build->bat, &s_build->capbat, s_build->nbat + 1, sizeof(CoreBatch));
    s_build->bat[s_build->nbat++] = b;
}

// ---- BP ---------------------------------------------------------------------------------
void gx_bp_write(uint32_t v) {
    uint8_t reg = v >> 24;
    uint32_t mask = bp[0xFE] ? bp[0xFE] : 0xFFFFFF;
    if (reg != 0xFE) bp[0xFE] = 0;
    bp[reg] = (bp[reg] & ~mask) | (v & mask);
    v = bp[reg];
    switch (reg) {
    case 0x45: gx_pe_finish(); break;
    case 0x47: gx_pe_token(v & 0xFFFF, 1); break;
    case 0x48: gx_pe_token(v & 0xFFFF, 0); break;
    case 0x4F: case 0x50: {
        // AR: red bits 0-7, alpha 8-15; GB: blue bits 0-7, green 8-15
        uint32_t ar = bp[0x4F], gb = bp[0x50];
        s_clear_color = (ar & 0xFF) | ((gb >> 8) & 0xFF) << 8 | (gb & 0xFF) << 16 | ((ar >> 8) & 0xFF) << 24;
        break;
    }
    case 0x52:  // EFB copy: bit 14 set = copy to XFB (display), else to a texture
        if (v & (1u << 14)) publish_frame();
        else efb_copy_to_texture(v);
        break;
    case 0x65: {  // LOADTLUT1: copy palette from RAM into TMEM
        uint32_t src = ((bp[0x64] & 0xFFFFFF) << 5) & 0x01FFFFFF;
        uint32_t tmem = (v & 0x3FF) << 9, bytes = ((v >> 10) & 0x7FF) * 32;
        if (tmem + bytes <= sizeof s_tmem_tlut)
            memcpy(s_tmem_tlut + tmem, gptr(0x80000000u | src, bytes, 0), bytes);
        break;
    }
    default:
        if (reg >= 0xE0 && reg <= 0xE7) gx_material_bp(reg, v);
        break;
    }
}

// ---- vertex decoding ------------------------------------------------------------------
typedef struct {
    float pos[3], nrm[3];
    float st[8][2];
    uint32_t color[2];
    int has_nrm, has_color[2];
    int pnmtx, tmtx[8];
} RawVtx;

static float read_comp(const uint8_t *p, int fmt, int shift, int *adv) {
    float scale = 1.0f / (float)(1u << (shift & 31));
    switch (fmt) {
    case 0: *adv = 1; return p[0] * scale;
    case 1: *adv = 1; return (int8_t)p[0] * scale;
    case 2: *adv = 2; return (uint16_t)(p[0] << 8 | p[1]) * scale;
    case 3: *adv = 2; return (int16_t)(p[0] << 8 | p[1]) * scale;
    default: { *adv = 4; uint32_t u = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; return u2f(u); }
    }
}

static uint32_t read_color(const uint8_t *p, int fmt, int *adv) {
    uint32_t r, g, b, a = 255;
    switch (fmt) {
    case 0: { uint16_t c = p[0] << 8 | p[1]; r = (c >> 11) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; *adv = 2; break; }
    case 1: r = p[0]; g = p[1]; b = p[2]; *adv = 3; break;
    case 2: r = p[0]; g = p[1]; b = p[2]; *adv = 4; break;
    case 3: { uint16_t c = p[0] << 8 | p[1]; r = (c >> 12) * 17; g = ((c >> 8) & 15) * 17; b = ((c >> 4) & 15) * 17; a = (c & 15) * 17; *adv = 2; break; }
    case 4: { uint32_t c = p[0] << 16 | p[1] << 8 | p[2];
              r = ((c >> 18) & 63) * 255 / 63; g = ((c >> 12) & 63) * 255 / 63; b = ((c >> 6) & 63) * 255 / 63; a = (c & 63) * 255 / 63; *adv = 3; break; }
    default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; *adv = 4; break;
    }
    return r | g << 8 | b << 16 | a << 24;
}

// Pointer to attribute data: direct (from the stream) or indexed (from RAM).
static const uint8_t *attr_ptr(int mode, int array, const uint8_t **pp, int direct_size) {
    const uint8_t *p = *pp;
    if (mode == 1) { *pp += direct_size; return p; }
    uint32_t idx;
    if (mode == 2) { idx = p[0]; *pp += 1; }
    else { idx = p[0] << 8 | p[1]; *pp += 2; }
    uint32_t a = cp_array_base[array] + idx * cp_array_stride[array];
    return gptr(0x80000000u | a, 36, 0);
}

static int comp_size(int fmt) { static const int s[8] = {1, 1, 2, 2, 4, 4, 4, 4}; return s[fmt & 7]; }
static int color_size(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }

static void decode_vertex(const uint8_t **pp, int vat, RawVtx *out) {
    uint32_t lo = cp_vcd_lo, hi = cp_vcd_hi, a = cp_vat_a[vat], b = cp_vat_b[vat], c = cp_vat_c[vat];
    const uint8_t *p = *pp;
    int adv;
    out->pnmtx = cp_matidx_a & 63;
    for (int t = 0; t < 8; t++)
        out->tmtx[t] = t < 4 ? (cp_matidx_a >> (6 + 6 * t)) & 63 : (cp_matidx_b >> (6 * (t - 4))) & 63;
    if (lo & 1) out->pnmtx = *p++;
    for (int t = 0; t < 8; t++) if (lo & (2u << t)) out->tmtx[t] = *p++;

    // position
    int pcnt = (a & 1) ? 3 : 2, pfmt = (a >> 1) & 7, pshift = (a >> 4) & 31;
    const uint8_t *q = attr_ptr((lo >> 9) & 3, 0, &p, pcnt * comp_size(pfmt));
    out->pos[2] = 0;
    for (int i = 0; i < pcnt; i++) { out->pos[i] = read_comp(q, pfmt, pshift, &adv); q += adv; }

    // normal (fixed fraction bits: s8 -> 6, s16 -> 14). NBT: keep N only.
    int nmode = (lo >> 11) & 3;
    out->has_nrm = nmode != 0;
    if (nmode) {
        int nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1, nfmt = (a >> 10) & 7;
        int nshift = nfmt == 1 ? 6 : nfmt == 3 ? 14 : 0;
        if (nmode == 1) {
            for (int i = 0; i < 3; i++) { out->nrm[i] = read_comp(p, nfmt, nshift, &adv); p += adv; }
            if (nbt) p += 6 * comp_size(nfmt);
        } else {
            q = attr_ptr(nmode, 1, &p, 0);
            for (int i = 0; i < 3; i++) { out->nrm[i] = read_comp(q, nfmt, nshift, &adv); q += adv; }
            if (nbt && idx3) p += 2 * (nmode == 2 ? 1 : 2);
        }
    }

    // colours
    for (int ci = 0; ci < 2; ci++) {
        int mode = (lo >> (13 + 2 * ci)) & 3;
        out->has_color[ci] = mode != 0;
        if (!mode) continue;
        int fmt = (a >> (14 + 4 * ci)) & 7;
        q = attr_ptr(mode, 2 + ci, &p, color_size(fmt));
        out->color[ci] = read_color(q, fmt, &adv);
    }

    // texture coordinates
    int tcnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    int tfmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    int tsh[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (int t = 0; t < 8; t++) {
        out->st[t][0] = out->st[t][1] = 0;
        int mode = (hi >> (2 * t)) & 3;
        if (!mode) continue;
        int n = tcnt[t] ? 2 : 1;
        q = attr_ptr(mode, 4 + t, &p, n * comp_size(tfmt[t]));
        for (int i = 0; i < n; i++) { out->st[t][i] = read_comp(q, tfmt[t], tsh[t], &adv); q += adv; }
    }
    *pp = p;
}

// ---- lighting (XF colour channels) -------------------------------------------------------
static void unpack_rgba(uint32_t c, float o[4]) {  // XF registers: R in the high byte
    o[0] = (c >> 24) / 255.0f; o[1] = ((c >> 16) & 0xFF) / 255.0f; o[2] = ((c >> 8) & 0xFF) / 255.0f; o[3] = (c & 0xFF) / 255.0f;
}
static void unpack_vtx(uint32_t c, float o[4]) {   // decoded vertex colour: R in the low byte
    o[0] = (c & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f; o[2] = ((c >> 16) & 0xFF) / 255.0f; o[3] = (c >> 24) / 255.0f;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Accumulate the lights selected by a channel control word into lacc[comp0..comp1).
static void accumulate_lights(uint32_t ctl, const float pos[3], const float n[3], float lacc[4], int c0, int c1) {
    uint32_t mask = ((ctl >> 2) & 15) | ((ctl >> 11) & 15) << 4;
    int diffuse = (ctl >> 7) & 3, attnf = (ctl >> 9) & 3;
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        uint32_t base = 0x600 + i * 0x10;
        float col[4]; unpack_rgba(xf_u32(base + 3), col);
        const float *cosatt = &xf_mem[base + 4], *distatt = &xf_mem[base + 7];
        const float *lpos = &xf_mem[base + 10], *ldirv = &xf_mem[base + 13];
        float ldir[3] = {lpos[0] - pos[0], lpos[1] - pos[1], lpos[2] - pos[2]};
        float attn = 1.0f;
        if (attnf == 3) {  // spot
            float d2 = dot3(ldir, ldir), d = sqrtf(d2);
            if (d > 0) for (int k = 0; k < 3; k++) ldir[k] /= d;
            float cs = fmaxf(0, dot3(ldir, ldirv));
            float num = fmaxf(0, cosatt[0] + cosatt[1] * cs + cosatt[2] * cs * cs);
            float den = distatt[0] + distatt[1] * d + distatt[2] * d2;
            attn = den != 0 ? num / den : 0;
        } else if (attnf == 1) {  // specular
            float len = sqrtf(dot3(ldir, ldir));
            if (len > 0) for (int k = 0; k < 3; k++) ldir[k] /= len;
            float s = dot3(n, ldir) >= 0 ? fmaxf(0, dot3(n, ldirv)) : 0;
            float num = fmaxf(0, cosatt[0] + cosatt[1] * s + cosatt[2] * s * s);
            float den = distatt[0] + distatt[1] * s + distatt[2] * s * s;
            attn = den != 0 ? num / den : 0;
        } else {
            float len = sqrtf(dot3(ldir, ldir));
            if (len > 0) for (int k = 0; k < 3; k++) ldir[k] /= len;
        }
        float nl = dot3(ldir, n);
        float f = diffuse == 0 ? 1.0f : diffuse == 1 ? nl : fmaxf(0, nl);
        if (attnf == 1) f = diffuse == 0 ? 1.0f : fmaxf(0, nl);
        for (int k = c0; k < c1; k++) lacc[k] += attn * f * col[k];
    }
}

static uint32_t light_channel(int chan, const RawVtx *r, const float pos[3], const float n[3]) {
    uint32_t cctl = xf_reg[XF_COLOR0CTL + chan], actl = xf_reg[XF_ALPHA0CTL + chan];
    float vtx[4] = {1, 1, 1, 1};
    if (r->has_color[chan]) unpack_vtx(r->color[chan], vtx);
    else if (chan == 1 && r->has_color[0]) unpack_vtx(r->color[0], vtx);
    float matreg[4], ambreg[4];
    unpack_rgba(xf_reg[XF_MATERIAL0 + chan], matreg);
    unpack_rgba(xf_reg[XF_AMBIENT0 + chan], ambreg);
    float out[4];
    for (int pass = 0; pass < 2; pass++) {  // pass 0: rgb (colour ctl), pass 1: alpha (alpha ctl)
        uint32_t ctl = pass ? actl : cctl;
        int c0 = pass ? 3 : 0, c1 = pass ? 4 : 3;
        const float *mat = (ctl & 1) ? vtx : matreg;
        if (!(ctl & 2)) { for (int k = c0; k < c1; k++) out[k] = mat[k]; continue; }
        const float *amb = (ctl & 0x40) ? vtx : ambreg;
        float lacc[4] = {amb[0], amb[1], amb[2], amb[3]};
        accumulate_lights(ctl, pos, n, lacc, c0, c1);
        for (int k = c0; k < c1; k++) out[k] = mat[k] * clamp01(lacc[k]);
    }
    uint32_t res = 0;
    for (int k = 0; k < 4; k++) res |= (uint32_t)(clamp01(out[k]) * 255.0f + 0.5f) << (8 * k);
    return res;
}

// ---- texgen ---------------------------------------------------------------------------------
static void texgen(const RawVtx *r, const float nmodel[3], uint32_t c0, CoreVertex *o) {
    int ngen = xf_reg[XF_NUMTEXGEN] & 15;
    int dual = xf_reg[XF_DUALTEX] & 1;
    float outs[8][3];
    for (int g = 0; g < 8; g++) {
        o->uv[g][0] = o->uv[g][1] = 0;
        if (g >= ngen) continue;
        uint32_t info = xf_reg[XF_TEXGEN0 + g];
        int proj = (info >> 1) & 1, abc1 = (info >> 2) & 1, type = (info >> 4) & 7, src = (info >> 7) & 31;
        float in[4] = {0, 0, 1, 1};
        switch (src) {
        case 0: in[0] = r->pos[0]; in[1] = r->pos[1]; in[2] = r->pos[2]; break;   // position
        case 1: in[0] = nmodel[0]; in[1] = nmodel[1]; in[2] = nmodel[2]; break;   // normal
        case 2: in[0] = (c0 & 0xFF) / 255.0f; in[1] = ((c0 >> 8) & 0xFF) / 255.0f; break;  // colours
        default:
            if (src >= 5 && src <= 12) { in[0] = r->st[src - 5][0]; in[1] = r->st[src - 5][1]; }
            break;
        }
        if (!abc1 && src >= 5) in[2] = 1.0f;
        float s, t, q = 1.0f;
        if (type == 0) {  // regular: texture matrix
            const float *m = &xf_mem[(r->tmtx[g] & 63) * 4];
            s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
            t = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
            if (proj) q = m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3];
        } else if (type == 1) {  // emboss: reuse source texgen (light offset ignored)
            int from = (info >> 12) & 7;
            s = outs[from][0]; t = outs[from][1]; q = outs[from][2];
        } else {  // colour-to-texcoord
            s = in[0]; t = in[1];
        }
        if (dual && type == 0) {
            uint32_t pinfo = xf_reg[XF_POSTTEX0 + g];
            float v[3] = {s, t, q};
            if ((pinfo >> 8) & 1) {
                float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len > 0) { v[0] /= len; v[1] /= len; v[2] /= len; }
            }
            const float *pm = &xf_mem[0x500 + (pinfo & 63) * 4];
            s = pm[0] * v[0] + pm[1] * v[1] + pm[2] * v[2] + pm[3];
            t = pm[4] * v[0] + pm[5] * v[1] + pm[6] * v[2] + pm[7];
            q = pm[8] * v[0] + pm[9] * v[1] + pm[10] * v[2] + pm[11];
            proj = 1;
        }
        outs[g][0] = s; outs[g][1] = t; outs[g][2] = q;
        if (proj && q != 0) { s /= q; t /= q; }
        o->uv[g][0] = s; o->uv[g][1] = t;
    }
}

// ---- vertex transform -------------------------------------------------------------------------
static CoreVertex transform(const RawVtx *r) {
    CoreVertex o;
    const float *m = &xf_mem[(r->pnmtx & 63) * 4];
    float x = r->pos[0], y = r->pos[1], z = r->pos[2];
    float pos[3];
    pos[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    pos[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    pos[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    o.x = pos[0]; o.y = pos[1]; o.z = pos[2];

    float nmodel[3] = {0, 0, 1}, n[3] = {0, 0, 1};
    if (r->has_nrm) {
        nmodel[0] = r->nrm[0]; nmodel[1] = r->nrm[1]; nmodel[2] = r->nrm[2];
        const float *nm = &xf_mem[0x400 + (r->pnmtx & 31) * 3];
        n[0] = nm[0] * nmodel[0] + nm[1] * nmodel[1] + nm[2] * nmodel[2];
        n[1] = nm[3] * nmodel[0] + nm[4] * nmodel[1] + nm[5] * nmodel[2];
        n[2] = nm[6] * nmodel[0] + nm[7] * nmodel[1] + nm[8] * nmodel[2];
        float len = sqrtf(dot3(n, n));
        if (len > 0) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
    int nchan = xf_reg[XF_NUMCHAN] & 3;
    o.color0 = nchan >= 1 ? light_channel(0, r, pos, n) : 0;
    o.color1 = nchan >= 2 ? light_channel(1, r, pos, n) : 0;
    texgen(r, nmodel, o.color0, &o);
    return o;
}

// ---- draw state --------------------------------------------------------------------------------
static void current_state(CoreBatch *b) {
    // projection: row-major 4x4, z remapped from GX [-w, 0] to D3D [0, w]
    float pr[6]; memcpy(pr, &xf_reg[XF_PROJ], sizeof pr);
    int ortho = xf_reg[XF_PROJ + 6] != 0;
    float m[16] = {0};
    if (!ortho) {
        m[0] = pr[0]; m[2] = pr[1]; m[5] = pr[2]; m[6] = pr[3]; m[10] = pr[4]; m[11] = pr[5]; m[14] = -1;
    } else {
        m[0] = pr[0]; m[3] = pr[1]; m[5] = pr[2]; m[7] = pr[3]; m[10] = pr[4]; m[11] = pr[5]; m[15] = 1;
    }
    for (int i = 0; i < 4; i++) m[8 + i] += m[12 + i];
    memcpy(b->proj, m, sizeof m);

    // viewport (EFB pixels)
    float vp[6]; memcpy(vp, &xf_reg[XF_VIEWPORT], sizeof vp);
    b->viewport[0] = (int32_t)(vp[3] - 342 - vp[0]);
    b->viewport[1] = (int32_t)(vp[4] - 342 + vp[1]);
    b->viewport[2] = (int32_t)(2 * vp[0]);
    b->viewport[3] = (int32_t)(-2 * vp[1]);
    // SCISSORTL/BR: y in bits 0-10, x in bits 12-22, both offset by 342
    uint32_t tl = bp[0x20], br = bp[0x21];
    b->scissor[0] = (int32_t)((tl >> 12) & 0x7FF) - 342; b->scissor[1] = (int32_t)(tl & 0x7FF) - 342;
    b->scissor[2] = (int32_t)((br >> 12) & 0x7FF) - 342 + 1; b->scissor[3] = (int32_t)(br & 0x7FF) - 342 + 1;

    CoreMaterial mat;
    uint32_t maps = gx_material_build(&mat);
    b->material = frame_material(&mat);
    static const int mode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    for (int i = 0; i < 8; i++) {
        b->textures[i] = (maps & (1u << i)) ? frame_texture(i) : -1;
        b->sampler[i] = bp[mode0[i]];
    }
}

static int same_state(const CoreBatch *a, const CoreBatch *b) {
    return memcmp(a->proj, b->proj, sizeof a->proj) == 0 && a->material == b->material &&
           memcmp(a->textures, b->textures, sizeof a->textures) == 0 &&
           memcmp(a->sampler, b->sampler, sizeof a->sampler) == 0 &&
           memcmp(a->viewport, b->viewport, sizeof a->viewport) == 0 &&
           memcmp(a->scissor, b->scissor, sizeof a->scissor) == 0;
}

// ---- draw ------------------------------------------------------------------------------------
// Draw dump (format + first vertices of every draw), requested by the host for
// the next frame the core builds.
static volatile LONG s_dump_request;
static uint32_t s_dump_serial = 0xFFFFFFFFu;
CORE_API void core_request_gx_dump(void) { InterlockedExchange(&s_dump_request, 1); }
static int dumping(void) {
    if (s_dump_request && InterlockedExchange(&s_dump_request, 0)) s_dump_serial = s_serial + 1;
    return s_serial + 1 == s_dump_serial;
}

void gx_draw(uint8_t op, uint32_t count, const uint8_t *data) {
    int prim = (op >> 3) & 7, vat = op & 7;
    if (count == 0 || count > 4096) return;
    RawVtx *raw = _alloca(sizeof(RawVtx) * count);
    const uint8_t *start = data;
    for (uint32_t i = 0; i < count; i++) decode_vertex(&data, vat, &raw[i]);
    if (dumping()) {
        logf_("[draw] op %02X n %u vcd %08X/%08X vat %08X %08X %08X matA %08X stride %u tev %u map0 %d",
              op, count, cp_vcd_lo, cp_vcd_hi, cp_vat_a[vat], cp_vat_b[vat], cp_vat_c[vat], cp_matidx_a,
              (uint32_t)(data - start) / count, ((bp[0] >> 10) & 15) + 1, (bp[0x28] & 0x40) ? (int)(bp[0x28] & 7) : -1);
        // log each distinct pixel-pipeline setup once per dump
        static uint32_t s_seen[256]; static int s_nseen; static uint32_t s_seen_serial;
        if (s_seen_serial != s_serial) { s_seen_serial = s_serial; s_nseen = 0; }
        uint32_t key = bp[0] * 31u + bp[0xF3] * 17u + bp[0x41] * 7u;
        for (int st = 0; st < 16; st++) key = key * 131u + bp[0xC0 + 2 * st] * 3u + bp[0xC1 + 2 * st] + bp[0x28 + st / 2];
        int seen = 0;
        for (int k = 0; k < s_nseen; k++) if (s_seen[k] == key) seen = 1;
        if (!seen && s_nseen < 256) {
            s_seen[s_nseen++] = key;
            logf_("    genmode %08X xf ntexgen %u nchan %u col0ctl %08X alp0ctl %08X mat0 %08X amb0 %08X dualtex %u",
                  bp[0], xf_reg[XF_NUMTEXGEN], xf_reg[XF_NUMCHAN], xf_reg[XF_COLOR0CTL], xf_reg[XF_ALPHA0CTL],
                  xf_reg[XF_MATERIAL0], xf_reg[XF_AMBIENT0], xf_reg[XF_DUALTEX]);
            for (int g = 0; g < (int)(xf_reg[XF_NUMTEXGEN] & 15); g++) {
                const float *tm = &xf_mem[(raw[0].tmtx[g] & 63) * 4];
                logf_("    texgen%d %08X post %08X mtx %d [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]", g, xf_reg[XF_TEXGEN0 + g],
                      xf_reg[XF_POSTTEX0 + g], raw[0].tmtx[g], tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], tm[6], tm[7]);
            }
            for (int st = 0; st <= (int)((bp[0] >> 10) & 15); st++)
                logf_("    tev%d cenv %08X aenv %08X tref %03X ksel %08X", st, bp[0xC0 + 2 * st], bp[0xC1 + 2 * st],
                      (bp[0x28 + st / 2] >> ((st & 1) * 12)) & 0xFFF, bp[0xF6 + st / 2]);
            logf_("    alphacmp %08X cmode0 %08X zmode %08X", bp[0xF3], bp[0x41], bp[0x40]);
            const float *pm = &xf_mem[0x500 + (xf_reg[XF_POSTTEX0] & 63) * 4];
            logf_("    post0 [%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]", pm[0], pm[1], pm[2], pm[3],
                  pm[4], pm[5], pm[6], pm[7], pm[8], pm[9], pm[10], pm[11]);
            CoreVertex tv0 = transform(&raw[0]);
            logf_("    out uv0 %.3f %.3f uv1 %.3f %.3f uv2 %.3f %.3f col0 %08X col1 %08X", tv0.uv[0][0], tv0.uv[0][1],
                  tv0.uv[1][0], tv0.uv[1][1], tv0.uv[2][0], tv0.uv[2][1], tv0.color0, tv0.color1);
        }
        if (((cp_vcd_lo >> 9) & 3) >= 2) {
            const uint8_t *pa = gptr(0x80000000u | cp_array_base[0], 16, 0);
            logf_("    pos array %08X stride %u: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X; first idx %02X%02X",
                  cp_array_base[0], cp_array_stride[0], pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pa[6], pa[7], pa[8], pa[9],
                  pa[10], pa[11], start[0], start[1]);
        }
        for (uint32_t i = 0; i < count && i < 3; i++) {
            const float *m = &xf_mem[(raw[i].pnmtx & 63) * 4];
            logf_("    v%u pos %.3f %.3f %.3f pnmtx %d tmtx0 %d st0 %.3f %.3f  mtx row0 %.3f %.3f %.3f %.3f", i,
                  raw[i].pos[0], raw[i].pos[1], raw[i].pos[2], raw[i].pnmtx, raw[i].tmtx[0], raw[i].st[0][0], raw[i].st[0][1],
                  m[0], m[1], m[2], m[3]);
        }
    }
    if (prim >= 5) return;  // lines / points: not drawn yet
    static int s_only_vat = -2;
    if (s_only_vat == -2) { const char *e = getenv("TUROK_ONLY_VAT"); s_only_vat = e ? atoi(e) : -1; }
    if (s_only_vat >= 0 && vat != s_only_vat) return;

    CoreBatch st;
    memset(&st, 0, sizeof st);  // kind 0 = draw; copy fields unused
    current_state(&st);
    CoreBatch *last = s_build->nbat ? &s_build->bat[s_build->nbat - 1] : NULL;
    if (!last || last->kind != 0 || !same_state(last, &st) || last->first_vertex + last->vertex_count != s_build->nvtx) {
        s_build->bat = grow(s_build->bat, &s_build->capbat, s_build->nbat + 1, sizeof(CoreBatch));
        st.first_vertex = s_build->nvtx; st.vertex_count = 0;
        s_build->bat[s_build->nbat++] = st;
        last = &s_build->bat[s_build->nbat - 1];
    }
    uint32_t tris = 0;
    switch (prim) {
    case 0: case 1: tris = (count / 4) * 2; break;  // quads
    case 2: tris = count / 3; break;               // triangles
    case 3: case 4: tris = count >= 3 ? count - 2 : 0; break;  // strip / fan
    }
    // transform each vertex once, then index into the triangle list
    CoreVertex *tv = _alloca(sizeof(CoreVertex) * count);
    for (uint32_t i = 0; i < count; i++) tv[i] = transform(&raw[i]);
    s_build->vtx = grow(s_build->vtx, &s_build->capvtx, s_build->nvtx + tris * 3, sizeof(CoreVertex));
    CoreVertex *o = s_build->vtx + s_build->nvtx;
    #define EMIT(i) (*o++ = tv[i])
    switch (prim) {
    case 0: case 1:
        for (uint32_t i = 0; i + 3 < count; i += 4) { EMIT(i); EMIT(i + 1); EMIT(i + 2); EMIT(i); EMIT(i + 2); EMIT(i + 3); }
        break;
    case 2: for (uint32_t i = 0; i + 2 < count; i += 3) { EMIT(i); EMIT(i + 1); EMIT(i + 2); } break;
    case 3:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) { EMIT(i + 1); EMIT(i); EMIT(i + 2); } else { EMIT(i); EMIT(i + 1); EMIT(i + 2); }
        }
        break;
    case 4: for (uint32_t i = 1; i + 1 < count; i++) { EMIT(0); EMIT(i); EMIT(i + 1); } break;
    }
    #undef EMIT
    s_build->nvtx += tris * 3;
    last->vertex_count += tris * 3;
}

// ---- host API ---------------------------------------------------------------------------------
CORE_API const CoreFrame *core_acquire_frame(void) {
    frame_lock();
    const CoreFrame *f = NULL;
    if (s_ready && !s_held) { s_held = s_ready; s_ready = NULL; f = &s_held->f; }
    LeaveCriticalSection(&s_frame_lock);
    return f;
}

CORE_API void core_release_frame(void) {
    frame_lock();
    if (s_held) { s_free_list[s_nfree++] = s_held; s_held = NULL; }
    LeaveCriticalSection(&s_frame_lock);
}
