// GX command FIFO: everything the game writes to 0xCC008000 (the write-gather
// pipe) lands here and is parsed into GPU commands.
//
// Parsing must track the CP vertex descriptor/attribute registers so the size
// of every draw command is known and the stream stays in sync. For now the
// only commands with side effects are the pixel-engine draw-done/token BP
// writes, which raise PE interrupts (GXDrawDone / GXSetDrawSync waits on them).
// This is the front end the renderer will hang off.
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "gx.h"
#include "hw.h"

static int comp_size(int fmt) { static const int s[8] = {1, 1, 2, 2, 4, 4, 4, 4}; return s[fmt & 7]; }
static int color_size(int fmt) { static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }

static int attr_size(int mode, int direct_size) {
    switch (mode) { case 1: return direct_size; case 2: return 1; case 3: return 2; default: return 0; }
}

int gx_vertex_size(int vat) {
    uint32_t lo = gx_cp_vcd_lo(), hi = gx_cp_vcd_hi();
    uint32_t a = gx_cp_vat(0, vat), b = gx_cp_vat(1, vat), c = gx_cp_vat(2, vat);
    int size = 0;
    for (int i = 0; i < 9; i++) if (lo & (1u << i)) size++;  // matrix indices
    // position
    size += attr_size((lo >> 9) & 3, ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7));
    // normal (NBT = 9 components; index3 -> three indices)
    int nmode = (lo >> 11) & 3, nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
    if (nmode == 1) size += (nbt ? 9 : 3) * comp_size((a >> 10) & 7);
    else if (nmode >= 2) size += (nbt && idx3 ? 3 : 1) * (nmode == 2 ? 1 : 2);
    // colors
    size += attr_size((lo >> 13) & 3, color_size((a >> 14) & 7));
    size += attr_size((lo >> 15) & 3, color_size((a >> 18) & 7));
    // texture coordinates 0..7
    int tcnt[8], tfmt[8];
    tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7;
    tcnt[1] = b & 1;         tfmt[1] = (b >> 1) & 7;
    tcnt[2] = (b >> 9) & 1;  tfmt[2] = (b >> 10) & 7;
    tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7;
    tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7;
    tcnt[5] = (c >> 5) & 1;  tfmt[5] = (c >> 6) & 7;
    tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7;
    tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7;
    for (int t = 0; t < 8; t++)
        size += attr_size((hi >> (2 * t)) & 3, (tcnt[t] ? 2 : 1) * comp_size(tfmt[t]));
    return size;
}

// ---- command parsing -----------------------------------------------------------
static uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint64_t s_stats_cmds, s_stats_verts;

// Parse one command at p (n bytes available). Returns bytes consumed, or 0 if
// the command is incomplete.
static uint32_t parse_one(const uint8_t *p, uint32_t n, int in_dl);

static void run_display_list(uint32_t addr, uint32_t size) {
    const uint8_t *p = gptr(addr, size, 0);
    uint32_t pos = 0;
    while (pos < size) {
        uint32_t k = parse_one(p + pos, size - pos, 1);
        if (!k) break;
        pos += k;
    }
}

// Recent commands, for diagnosing stream desyncs.
static struct { uint8_t op; uint32_t size, a, b; } s_hist[16];
static int s_hist_pos;
static void hist(uint8_t op, uint32_t size, uint32_t a, uint32_t b) {
    s_hist[s_hist_pos].op = op; s_hist[s_hist_pos].size = size; s_hist[s_hist_pos].a = a; s_hist[s_hist_pos].b = b;
    s_hist_pos = (s_hist_pos + 1) & 15;
}

static uint32_t parse_one_inner(const uint8_t *p, uint32_t n, int in_dl);
static uint32_t parse_one(const uint8_t *p, uint32_t n, int in_dl) {
    uint32_t k = parse_one_inner(p, n, in_dl);
    if (k && p[0]) {
        uint32_t a = n >= 5 ? be32p(p + 1) : 0;
        hist(p[0], k, a, (p[0] & 0x80) ? (uint32_t)gx_vertex_size(p[0] & 7) : in_dl);
    }
    return k;
}

static uint32_t parse_one_inner(const uint8_t *p, uint32_t n, int in_dl) {
    uint8_t op = p[0];
    switch (op) {
    case 0x00: return 1;                                   // NOP
    case 0x08: if (n < 6) return 0; gx_cp_write(p[1], be32p(p + 2)); return 6;
    case 0x10: {
        if (n < 5) return 0;
        uint32_t hdr = be32p(p + 1), cnt = ((hdr >> 16) & 0xF) + 1;
        if (n < 5 + 4 * cnt) return 0;
        gx_xf_write(hdr & 0xFFFF, cnt, p + 5);
        return 5 + 4 * cnt;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:
        if (n < 5) return 0;
        gx_load_indx(12 + ((op >> 3) & 3), be32p(p + 1));
        return 5;
    case 0x40:
        if (n < 9) return 0;
        if (!in_dl) run_display_list(be32p(p + 1), be32p(p + 5));
        return 9;
    case 0x44: case 0x48: return 1;
    case 0x61: if (n < 5) return 0; gx_bp_write(be32p(p + 1)); return 5;
    default:
        if (op & 0x80) {
            if (n < 3) return 0;
            uint32_t count = (uint32_t)p[1] << 8 | p[2];
            uint32_t total = 3 + count * gx_vertex_size(op & 7);
            if (n < total) return 0;
            s_stats_verts += count;
            gx_draw(op, count, p + 3);
            return total;
        }
        logf_("[gx] unknown %s opcode %02X; dropping stream. Recent commands (oldest first):", in_dl ? "display-list" : "FIFO", op);
        for (int i = 0; i < 16; i++) {
            int j = (s_hist_pos + i) & 15;
            if (s_hist[j].op) logf_("    op %02X size %u data %08X vtxsize/dl %u", s_hist[j].op, s_hist[j].size, s_hist[j].a, s_hist[j].b);
        }
        logf_("    next bytes: %02X %02X %02X %02X %02X %02X %02X %02X", p[0], n > 1 ? p[1] : 0, n > 2 ? p[2] : 0,
              n > 3 ? p[3] : 0, n > 4 ? p[4] : 0, n > 5 ? p[5] : 0, n > 6 ? p[6] : 0, n > 7 ? p[7] : 0);
        return n;  // resync by discarding what we have
    }
}

// ---- command processor input ----------------------------------------------------------
// Bytes the CP actually reads are parsed here; an incomplete trailing command
// is kept until the rest arrives.
static uint8_t s_fifo[1 << 20];
static uint32_t s_len;

static void cp_feed(const uint8_t *data, uint32_t n) {
    if (s_len + n > sizeof s_fifo) fatal("GX FIFO overflow (stuck command?)");
    memcpy(s_fifo + s_len, data, n);
    s_len += n;
    uint32_t pos = 0;
    while (pos < s_len) {
        uint32_t k = parse_one(s_fifo + pos, s_len - pos, 0);
        if (!k) break;
        pos += k; s_stats_cmds++;
    }
    if (pos) { memmove(s_fifo, s_fifo + pos, s_len - pos); s_len -= pos; }
}

// ---- write-gather pipe -> PI FIFO in RAM -> CP ---------------------------------------
// CPU writes to 0xCC008000 are gathered into 32-byte bursts and written to RAM
// at the PI write pointer (0x3014, wrapping between 0x300C and 0x3010). If the
// CPU FIFO is linked to the GPU FIFO (CP control bit 4) the CP write pointer
// follows, and with reads enabled (bit 0) the CP consumes up to it. While a
// display list is being built the FIFO is unlinked, so commands only land in
// RAM, exactly as GXBeginDisplayList / GXEndDisplayList expect.
#define CP_CTRL 0x02
#define CP_BASE 0x20
#define CP_END  0x24
#define CP_DIST 0x30
#define CP_WPTR 0x34
#define CP_RPTR 0x38
#define PI_BASE 0x300C
#define PI_END  0x3010
#define PI_WPTR 0x3014

static uint32_t cp32(uint32_t off) { return shadow_read(off + 2, 2) << 16 | shadow_read(off, 2); }  // lo, hi halves
static void cp32_set(uint32_t off, uint32_t v) { shadow_write(off, v & 0xFFFF, 2); shadow_write(off + 2, v >> 16, 2); }

static void cp_run(void) {
    if (!(shadow_read(CP_CTRL, 2) & 1)) return;  // GP reads disabled
    uint32_t base = cp32(CP_BASE) & 0x03FFFFE0u, end = cp32(CP_END) & 0x03FFFFE0u;
    uint32_t w = cp32(CP_WPTR) & 0x03FFFFE0u, r = cp32(CP_RPTR) & 0x03FFFFE0u;
    if (end <= base || r < base || r >= end + 32) { cp32_set(CP_RPTR, w); return; }
    for (uint32_t guard = 0; r != w && guard < (1u << 20); guard++) {
        cp_feed(g_ram + (r & 0x01FFFFFF), 32);
        r += 32;
        if (r >= end) r = base;
    }
    cp32_set(CP_RPTR, r);
    cp32_set(CP_DIST, 0);
}

static uint8_t s_gather[32];
static int s_gather_len;

static void gather_flush(void) {
    uint32_t base = shadow_read(PI_BASE, 4) & 0x03FFFFE0u, end = shadow_read(PI_END, 4) & 0x03FFFFE0u;
    uint32_t wraw = shadow_read(PI_WPTR, 4), w = wraw & 0x03FFFFE0u;
    if (w + 32 <= RAM_SIZE) memcpy(g_ram + w, s_gather, 32);
    if (!(shadow_read(CP_CTRL, 2) & 0x10) && getenv("TUROK_LOG_REDIRECT")) {
        static int budget = 40;
        if (budget > 0) {
            budget--;
            logf_("[wgp] redirected burst -> %08X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X", w,
                  s_gather[0], s_gather[1], s_gather[2], s_gather[3], s_gather[4], s_gather[5], s_gather[6], s_gather[7],
                  s_gather[8], s_gather[9], s_gather[10], s_gather[11]);
        }
    }
    w += 32;
    uint32_t wrap = wraw & 0x20000000u;
    if (end > base && w >= end) { w = base; wrap = 0x20000000u; }
    shadow_write(PI_WPTR, w | wrap, 4);
    s_gather_len = 0;
    if (shadow_read(CP_CTRL, 2) & 0x10) {  // linked: GPU FIFO receives it too
        cp32_set(CP_WPTR, w);
        cp_run();
    }
}

// Writing WPAR (SPR 921, PPCMtwpar) resets the gather pipe: pending bytes
// are discarded. GXRedirectWriteGatherPipe relies on this so the redirected
// stream starts aligned.
void gx_gather_reset(void) { s_gather_len = 0; }

void gx_fifo_write(uint32_t v, int size) {
    for (int i = size - 1; i >= 0; i--) {
        s_gather[s_gather_len++] = (uint8_t)(v >> (8 * i));
        if (s_gather_len == 32) gather_flush();
    }
}

// CP register writes (0xCC000000..): enabling reads may release queued commands.
void gx_cp_reg_written(uint32_t off) {
    if (off == CP_CTRL || off == CP_WPTR || off == CP_WPTR + 2) cp_run();
}
