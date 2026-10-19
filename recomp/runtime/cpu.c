// Runtime helpers called from recompiled code: dispatch, SPRs, paired-single
// quantisation, cache ops and traps.
#include <stdio.h>
#include <float.h>
#include "ppc.h"
#include "func_table.h"
#include "host.h"
#include "gx.h"

const FuncEntry *func_lookup(uint32_t addr) {
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_func_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < g_func_count && g_func_table[lo].addr == addr) ? &g_func_table[lo] : NULL;
}

const FuncEntry *func_containing(uint32_t addr) {
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_func_table[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    return lo ? &g_func_table[lo - 1] : NULL;
}

void call_indirect(PPCContext *c, uint32_t addr) {
    const FuncEntry *e = func_lookup(addr);
    if (!e) fatal("call_indirect: no function at %08X (lr=%08X)", addr, c->lr);
    e->fn(c);
}

void ppc_sc(PPCContext *c, uint32_t pc) { (void)c; (void)pc; }

// rfi = "msr = SRR1; jump to SRR0". The SDK uses it outside of exceptions to
// enter/leave real mode (OSMemory.c RealMode / Config24MB): either to tail-jump
// into a routine, or to return to LR. Generated code returns right after this.
void ppc_rfi(PPCContext *c, uint32_t pc) {
    uint32_t target = c->spr[26];
    if (target < RAM_SIZE) target |= 0x80000000u;  // real-mode (physical) address
    c->msr = c->spr[27];
    if (target == c->lr) return;                    // "return via rfi"
    if (!func_lookup(target)) fatal("rfi at %08X to %08X: no function there", pc, target);
    call_indirect(c, target);
}

void ppc_trap(PPCContext *c, uint32_t pc) { (void)c; fatal("trap at %08X", pc); }

void ppc_unimpl(PPCContext *c, uint32_t pc, uint32_t insn) {
    (void)c; fatal("unimplemented instruction %08X at %08X", insn, pc);
}

void ppc_bad_jump(PPCContext *c, uint32_t pc, uint32_t target) {
    (void)c; fatal("control fell off the end of text at %08X -> %08X", pc, target);
}

// Time base runs at bus clock / 4 = 40.5 MHz.
uint32_t ppc_mftb(PPCContext *c, int upper) {
    (void)c;
    uint64_t tb = host_time_ticks();
    return upper ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

uint32_t ppc_mfspr(PPCContext *c, int spr) {
    switch (spr) {
    case 22: {  // DEC counts down at the time base rate
        uint64_t now = host_time_ticks();
        return c->spr[22] - (uint32_t)(now - ((uint64_t)c->spr[1023] << 32 | c->spr[1022]));
    }
    case 921: return c->spr[921] & ~1u;  // WPAR: BNE (buffer not empty) never set
    case 268: case 284: return ppc_mftb(c, 0);
    case 269: case 285: return ppc_mftb(c, 1);
    default: return c->spr[spr & 1023];
    }
}

void ppc_mtspr(PPCContext *c, int spr, uint32_t v) {
    c->spr[spr & 1023] = v;
    if (spr == 921) gx_gather_reset();
    if (spr == 923 && (v & 2)) {
        // Locked-cache DMA (used by the THP video decoder). DMA_U: length high
        // bits 0-4, memory address bits 5-31; DMA_L: trigger bit 1, length low
        // bits 2-3, direction bit 4 (1 = memory -> cache), cache address 5-31.
        // Length is in 32-byte lines (0 = 128). Completes instantly.
        uint32_t u = c->spr[922];
        uint32_t lines = ((u & 0x1F) << 2) | ((v >> 2) & 3);
        if (!lines) lines = 128;
        uint32_t mem = 0x80000000u | (u & 0x01FFFFE0u), lc = v & ~0x1Fu;  // memory side is physical
        for (uint32_t i = 0; i < lines * 32; i += 4) {
            if (v & 0x10) wr32(lc + i, rd32(mem + i));
            else wr32(mem + i, rd32(lc + i));
        }
        c->spr[923] = v & ~2u;  // trigger clears when done
    }
    if (spr == 22) {  // remember when DEC was written
        uint64_t now = host_time_ticks();
        c->spr[1022] = (uint32_t)now; c->spr[1023] = (uint32_t)(now >> 32);
        irq_dec_written(v);
    }
}

// ---- paired-single quantised load/store -------------------------------------
static double ldexp_scale(int scale) {  // 6-bit signed scale
    if (scale & 0x20) scale -= 64;
    return ldexp(1.0, -scale);
}
static double dequant(uint32_t ea, int type, double scale, int *size) {
    switch (type) {
    case 4: *size = 1; return rd8(ea) * scale;
    case 5: *size = 2; return rd16(ea) * scale;
    case 6: *size = 1; return (int8_t)rd8(ea) * scale;
    case 7: *size = 2; return (int16_t)rd16(ea) * scale;
    default: *size = 4; return u2f(rd32(ea));
    }
}
void psq_load(PPCContext *c, int frd, uint32_t ea, int w, int i) {
    uint32_t gqr = c->spr[SPR_GQR0 + i];
    int type = (gqr >> 16) & 7;
    double scale = ldexp_scale((gqr >> 24) & 0x3F);
    int sz;
    double a = dequant(ea, type, scale, &sz);
    double b = w ? 1.0 : dequant(ea + sz, type, scale, &sz);
    c->f[frd] = a; c->ps1[frd] = b;
}
static void quant(uint32_t ea, int type, double v, double scale, int *size) {
    double x = v * scale;
    switch (type) {
    case 4: *size = 1; x = x < 0 ? 0 : x > 255 ? 255 : x; wr8(ea, (uint8_t)x); return;
    case 5: *size = 2; x = x < 0 ? 0 : x > 65535 ? 65535 : x; wr16(ea, (uint16_t)x); return;
    case 6: *size = 1; x = x < -128 ? -128 : x > 127 ? 127 : x; wr8(ea, (uint8_t)(int8_t)x); return;
    case 7: *size = 2; x = x < -32768 ? -32768 : x > 32767 ? 32767 : x; wr16(ea, (uint16_t)(int16_t)x); return;
    default: *size = 4; wr32(ea, f2u((float)v)); return;
    }
}
void psq_store(PPCContext *c, int frs, uint32_t ea, int w, int i) {
    uint32_t gqr = c->spr[SPR_GQR0 + i];
    int type = gqr & 7;
    int sc = (gqr >> 8) & 0x3F;
    if (sc & 0x20) sc -= 64;
    double scale = ldexp(1.0, sc);
    int sz;
    quant(ea, type, c->f[frs], scale, &sz);
    if (!w) quant(ea + sz, type, c->ps1[frs], scale, &sz);
}

void ppc_dcbz(uint32_t ea) { memset(gptr(ea & ~31u, 32, 1), 0, 32); }
void ppc_dcbz_l(uint32_t ea) { memset(gptr(ea & ~31u, 32, 1), 0, 32); }

double ppc_fctiw(PPCContext *c, double v, int trunc) {
    int32_t r;
    if (v != v) r = INT32_MIN;
    else if (v >= 2147483648.0) r = INT32_MAX;
    else if (v < -2147483648.0) r = INT32_MIN;
    else if (trunc) r = (int32_t)v;
    else {
        switch (c->fpscr & 3) {
        case 0: r = (int32_t)nearbyint(v); break;  // host default is round-to-nearest-even
        case 1: r = (int32_t)v; break;
        case 2: r = (int32_t)ceil(v); break;
        default: r = (int32_t)floor(v); break;
        }
    }
    return u2d(0xFFF8000000000000ull | (uint32_t)r);
}

void ppc_lswi(PPCContext *c, int rd, uint32_t ea, int nb) {
    int r = rd - 1, i = 0;
    while (nb > 0) {
        if (i == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= (uint32_t)rd8(ea) << (24 - i);
        i = (i + 8) & 31; ea++; nb--;
    }
}
void ppc_stswi(PPCContext *c, int rs, uint32_t ea, int nb) {
    int r = rs - 1, i = 0;
    while (nb > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea, (uint8_t)(c->r[r] >> (24 - i)));
        i = (i + 8) & 31; ea++; nb--;
    }
}
