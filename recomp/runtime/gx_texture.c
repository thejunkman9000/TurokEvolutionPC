// GX texture decoding (all GameCube formats) with a content-hashed cache.
// Textures are decoded to RGBA8 (R in the low byte) for the host.
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "gx.h"
#include "core_api.h"

uint32_t gx_bp(int reg);
uint8_t *gx_tlut_mem(void);

typedef struct TexEntry {
    uint32_t addr, fmt, w, h, tlut, levels;   // key
    uint64_t hash;                    // content (+ palette) hash
    uint32_t id;
    uint32_t *rgba;
    uint32_t last_used;
    struct TexEntry *next;
} TexEntry;

#define TEX_BUCKETS 4096
static TexEntry *s_tex[TEX_BUCKETS];
static uint32_t s_next_id = 1, s_frame_counter;

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t c565(uint16_t c) { return rgba((c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31, 255); }
static uint32_t c5a3(uint16_t c) {
    if (c & 0x8000) return rgba(((c >> 10) & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, (c & 31) * 255 / 31, 255);
    return rgba(((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, ((c >> 12) & 7) * 255 / 7);
}
static uint32_t cia8(uint16_t c) { uint32_t i = c & 0xFF; return rgba(i, i, i, c >> 8); }

// Size in bytes of a w x h texture of format fmt (blocks are 32 bytes).
uint32_t gx_tex_bytes(uint32_t fmt, uint32_t w, uint32_t h);
static uint32_t tex_bytes(uint32_t fmt, uint32_t w, uint32_t h) { return gx_tex_bytes(fmt, w, h); }
uint32_t gx_tex_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t bw, bh;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;      // I4, C4, CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;       // I8, IA4, C8
    default: bw = 4; bh = 4; break;                      // IA8, 565, 5A3, RGBA8, C14X2
    }
    uint32_t blocks = ((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    return blocks * (fmt == 6 ? 64 : 32);
}

static uint32_t pal_lookup(uint32_t idx, uint32_t tlut) {
    uint32_t tmem = (tlut & 0x3FF) << 9, fmt = (tlut >> 10) & 3;
    const uint8_t *p = gx_tlut_mem() + ((tmem + idx * 2) & 0x7FFFF);
    uint16_t c = (uint16_t)(p[0] << 8 | p[1]);
    return fmt == 0 ? cia8(c) : fmt == 1 ? c565(c) : c5a3(c);
}

static void dxt1_block(const uint8_t *s, uint32_t *dst, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    uint16_t c0 = (uint16_t)(s[0] << 8 | s[1]), c1 = (uint16_t)(s[2] << 8 | s[3]);
    uint32_t pal[4];
    uint32_t r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
    uint32_t r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
    pal[0] = rgba(r0, g0, b0, 255); pal[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = 0;
    }
    for (int y = 0; y < 4; y++) {
        uint8_t bits = s[4 + y];
        for (int x = 0; x < 4; x++) {
            uint32_t px = x0 + x, py = y0 + y;
            if (px < w && py < h) dst[py * stride + px] = pal[(bits >> (6 - 2 * x)) & 3];
        }
    }
}

static void decode(const uint8_t *src, uint32_t fmt, uint32_t w, uint32_t h, uint32_t tlut, uint32_t *dst) {
    uint32_t bw = (fmt == 0 || fmt == 8 || fmt == 14) ? 8 : (fmt == 1 || fmt == 2 || fmt == 9) ? 8 : 4;
    uint32_t bh = (fmt == 0 || fmt == 8 || fmt == 14) ? 8 : 4;
    const uint8_t *s = src;
    for (uint32_t by = 0; by < h; by += bh) {
        for (uint32_t bx = 0; bx < w; bx += bw) {
            if (fmt == 14) {  // CMPR: 2x2 DXT1 sub-blocks
                for (int sb = 0; sb < 4; sb++) { dxt1_block(s, dst, w, bx + (sb & 1) * 4, by + (sb >> 1) * 4, w, h); s += 8; }
                continue;
            }
            if (fmt == 6) {  // RGBA8: AR plane then GB plane
                for (uint32_t i = 0; i < 16; i++) {
                    uint32_t x = bx + (i & 3), y = by + (i >> 2);
                    uint32_t a = s[2 * i], r = s[2 * i + 1], g = s[32 + 2 * i], b = s[32 + 2 * i + 1];
                    if (x < w && y < h) dst[y * w + x] = rgba(r, g, b, a);
                }
                s += 64;
                continue;
            }
            for (uint32_t y = 0; y < bh; y++) {
                for (uint32_t x = 0; x < bw; x++) {
                    uint32_t px = bx + x, py = by + y, c;
                    switch (fmt) {
                    case 0: { uint32_t i = (s[x / 2] >> ((x & 1) ? 0 : 4)) & 15; i *= 17; c = rgba(i, i, i, i); break; }
                    case 1: { uint32_t i = s[x]; c = rgba(i, i, i, i); break; }
                    case 2: { uint32_t a = (s[x] >> 4) * 17, i = (s[x] & 15) * 17; c = rgba(i, i, i, a); break; }
                    case 3: c = rgba(s[2 * x + 1], s[2 * x + 1], s[2 * x + 1], s[2 * x]); break;
                    case 4: c = c565((uint16_t)(s[2 * x] << 8 | s[2 * x + 1])); break;
                    case 5: c = c5a3((uint16_t)(s[2 * x] << 8 | s[2 * x + 1])); break;
                    case 8: c = pal_lookup((s[x / 2] >> ((x & 1) ? 0 : 4)) & 15, tlut); break;
                    case 9: c = pal_lookup(s[x], tlut); break;
                    case 10: c = pal_lookup(((s[2 * x] << 8) | s[2 * x + 1]) & 0x3FFF, tlut); break;
                    default: c = rgba(255, 0, 255, 255); break;
                    }
                    if (px < w && py < h) dst[py * w + px] = c;
                }
                s += (fmt == 0 || fmt == 8) ? 4 : (fmt == 1 || fmt == 2 || fmt == 9) ? 8 : 8;
            }
        }
    }
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n, uint64_t h) {
    for (uint32_t i = 0; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    return h;
}

// ---- EFB copies --------------------------------------------------------------------
// Copies stay on the host GPU (as in Dolphin's EFB-copy-to-texture). A texture
// read at a copy's destination returns a reference to it, as long as the CPU
// hasn't since rewritten that memory (detected by hashing it at copy time).
typedef struct { uint32_t addr, id, w, h, bytes; uint64_t hash; } EfbCopy;
static EfbCopy s_copies[64];
static int s_ncopies;

void gx_texture_register_efb_copy(uint32_t addr, uint32_t id, uint32_t w, uint32_t h, uint32_t bytes) {
    EfbCopy *c = NULL;
    for (int i = 0; i < s_ncopies; i++) if (s_copies[i].addr == addr) c = &s_copies[i];
    if (!c) { if (s_ncopies == 64) s_ncopies = 0; c = &s_copies[s_ncopies++]; }
    c->addr = addr; c->id = id; c->w = w; c->h = h; c->bytes = bytes;
    c->hash = addr + bytes <= RAM_SIZE ? hash_bytes(g_ram + addr, bytes, 0) : 0;
}

static int efb_copy_lookup(uint32_t addr, CoreTexture *out) {
    for (int i = 0; i < s_ncopies; i++) {
        EfbCopy *c = &s_copies[i];
        if (c->addr != addr) continue;
        if (c->addr + c->bytes <= RAM_SIZE && hash_bytes(g_ram + c->addr, c->bytes, 0) != c->hash) {
            c->addr = 0xFFFFFFFFu;  // CPU wrote a real texture here
            return 0;
        }
        out->id = c->id; out->width = c->w; out->height = c->h; out->levels = 1; out->rgba = NULL;
        return 1;
    }
    return 0;
}

// Look up / decode the texture bound to map 0..7.
int gx_texture_for_map(int map, CoreTexture *out) {
    static const int img0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    static const int img3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
    static const int tlr[8]  = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};
    static const int mode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    static const int mode1[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
    uint32_t i0 = gx_bp(img0[map]);
    uint32_t w = (i0 & 0x3FF) + 1, h = ((i0 >> 10) & 0x3FF) + 1, fmt = (i0 >> 20) & 15;
    uint32_t addr = ((gx_bp(img3[map]) & 0xFFFFFF) << 5) & 0x01FFFFFF;
    uint32_t tlut = gx_bp(tlr[map]);
    static const int img1[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
    static int s_fail_logs;
    if (!addr || addr >= RAM_SIZE) {
        if (s_fail_logs < 20) {
            s_fail_logs++;
            logf_("[tex] map %d: no RAM image (img0 %08X img1 %08X img3 %08X loadblock %08X %08X %08X %08X)", map, i0,
                  gx_bp(img1[map]), gx_bp(img3[map]), gx_bp(0x60), gx_bp(0x61), gx_bp(0x62), gx_bp(0x63));
        }
        return 0;
    }
    if (efb_copy_lookup(addr, out)) return 1;
    // Mip chain: only when the min filter samples mips (near/lin _mip_ near/lin).
    // Level count from TX_SETMODE1 max LOD (u4.4), stopping at 1x1.
    uint32_t minf = (gx_bp(mode0[map]) >> 5) & 7, levels = 1;
    if (minf == 1 || minf == 2 || minf == 5 || minf == 6) {
        uint32_t max_lod = (gx_bp(mode1[map]) >> 8) & 0xFF;
        levels = max_lod / 16 + 1;
        uint32_t full = 1, m = w > h ? w : h;
        while (m > 1) { m >>= 1; full++; }
        if (levels > full) levels = full;
    }
    uint32_t bytes = 0, pixels = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        bytes += tex_bytes(fmt, lw, lh); pixels += lw * lh;
    }
    if (addr + bytes > RAM_SIZE) return 0;
    const uint8_t *src = g_ram + addr;
    uint64_t hash = hash_bytes(src, bytes, 0xCBF29CE484222325ull ^ fmt ^ ((uint64_t)w << 32 | h) ^ ((uint64_t)levels << 56));
    if (fmt >= 8 && fmt <= 10) {
        uint32_t tmem = (tlut & 0x3FF) << 9, pal_bytes = fmt == 8 ? 32 : fmt == 9 ? 512 : 0x8000;
        if (tmem + pal_bytes <= 0x80000) hash = hash_bytes(gx_tlut_mem() + tmem, pal_bytes, hash ^ tlut);
    }
    uint32_t bucket = (addr >> 5) % TEX_BUCKETS;
    TexEntry *e;
    for (e = s_tex[bucket]; e; e = e->next)
        if (e->addr == addr && e->fmt == fmt && e->w == w && e->h == h && e->levels == levels && e->hash == hash) break;
    if (!e) {
        e = calloc(1, sizeof *e);
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->tlut = tlut; e->hash = hash; e->levels = levels;
        e->id = s_next_id++;
        e->rgba = malloc((size_t)pixels * 4);
        const uint8_t *ls = src;
        uint32_t *ld = e->rgba;
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            decode(ls, fmt, lw, lh, tlut, ld);
            ls += tex_bytes(fmt, lw, lh); ld += lw * lh;
        }
        static int s_log_budget = 400;
        if (s_log_budget > 0) { s_log_budget--; logf_("[tex] id %u fmt %u %ux%u addr %08X tlut %08X", e->id, fmt, w, h, addr, tlut); }
        e->next = s_tex[bucket]; s_tex[bucket] = e;
    }
    e->last_used = s_frame_counter;
    out->id = e->id; out->width = w; out->height = h; out->levels = levels; out->rgba = e->rgba;
    return 1;
}

// Called once per published frame: drop textures unused for ~10 seconds.
// (The host never holds a frame anywhere near that long.)
void gx_texture_end_frame(void) {
    s_frame_counter++;
    if (s_frame_counter % 60) return;
    for (int b = 0; b < TEX_BUCKETS; b++) {
        TexEntry **pp = &s_tex[b];
        while (*pp) {
            TexEntry *e = *pp;
            if (s_frame_counter - e->last_used > 600) { *pp = e->next; free(e->rgba); free(e); }
            else pp = &e->next;
        }
    }
}
