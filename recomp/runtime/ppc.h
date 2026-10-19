// Guest CPU state and memory access for recompiled Gekko (PowerPC 750CXe) code.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define BSWAP16(x) _byteswap_ushort(x)
#define BSWAP32(x) _byteswap_ulong(x)
#define BSWAP64(x) _byteswap_uint64(x)
#define FORCEINLINE __forceinline
#else
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
#define FORCEINLINE inline __attribute__((always_inline))
#endif

typedef struct PPCContext {
    uint32_t r[32];
    double f[32];      // FPR / paired-single slot 0
    double ps1[32];    // paired-single slot 1
    uint32_t cr;       // CR0 in the top nibble, as on hardware
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t spr[1024];  // everything else (GQRs at 912..919, HID*, SRR*, SPRG*)
    uint32_t pc;         // last known guest PC (set at calls for diagnostics)
} PPCContext;

#define SPR_GQR0 912

// ---- Memory ---------------------------------------------------------------
// 24 MB main RAM, mirrored at 0x80000000 (cached) and 0xC0000000 (uncached).
#define RAM_SIZE 0x01800000u
extern uint8_t *g_ram;
extern uint8_t g_lockedcache[0x4000];  // L2-locked cache at 0xE0000000

uint32_t mmio_read(uint32_t addr, int size);
void mmio_write(uint32_t addr, uint32_t val, int size);
uint8_t *slow_ptr(uint32_t addr, int size, int write);

static FORCEINLINE uint8_t *gptr(uint32_t a, int size, int write) {
    uint32_t p = a & 0x3FFFFFFFu;
    if (p + size <= RAM_SIZE && (a >> 30) == 2 /* 0x8xxxxxxx */) return g_ram + p;
    if ((a >> 28) == 0xC && p + size <= RAM_SIZE) return g_ram + p;
    return slow_ptr(a, size, write);
}
static FORCEINLINE int is_mmio(uint32_t a) { return (a & 0xFF000000u) == 0xCC000000u; }

static FORCEINLINE uint8_t rd8(uint32_t a) {
    if (is_mmio(a)) return (uint8_t)mmio_read(a, 1);
    return *gptr(a, 1, 0);
}
static FORCEINLINE uint16_t rd16(uint32_t a) {
    if (is_mmio(a)) return (uint16_t)mmio_read(a, 2);
    uint16_t v; memcpy(&v, gptr(a, 2, 0), 2); return BSWAP16(v);
}
static FORCEINLINE uint32_t rd32(uint32_t a) {
    if (is_mmio(a)) return mmio_read(a, 4);
    uint32_t v; memcpy(&v, gptr(a, 4, 0), 4); return BSWAP32(v);
}
static FORCEINLINE uint64_t rd64(uint32_t a) {
    if (is_mmio(a)) return (uint64_t)mmio_read(a, 4) << 32 | mmio_read(a + 4, 4);
    uint64_t v; memcpy(&v, gptr(a, 8, 0), 8); return BSWAP64(v);
}
static FORCEINLINE void wr8(uint32_t a, uint8_t v) {
    if (is_mmio(a)) { mmio_write(a, v, 1); return; }
    *gptr(a, 1, 1) = v;
}
static FORCEINLINE void wr16(uint32_t a, uint16_t v) {
    if (is_mmio(a)) { mmio_write(a, v, 2); return; }
    v = BSWAP16(v); memcpy(gptr(a, 2, 1), &v, 2);
}
static FORCEINLINE void wr32(uint32_t a, uint32_t v) {
    if (is_mmio(a)) { mmio_write(a, v, 4); return; }
    v = BSWAP32(v); memcpy(gptr(a, 4, 1), &v, 4);
}
static FORCEINLINE void wr64(uint32_t a, uint64_t v) {
    if (is_mmio(a)) { mmio_write(a, (uint32_t)(v >> 32), 4); mmio_write(a + 4, (uint32_t)v, 4); return; }
    v = BSWAP64(v); memcpy(gptr(a, 8, 1), &v, 8);
}

// ---- Float bit helpers ------------------------------------------------------
static FORCEINLINE uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static FORCEINLINE double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static FORCEINLINE uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static FORCEINLINE float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static FORCEINLINE double rnd_s(double d) { return (double)(float)d; }

// ---- Condition register ------------------------------------------------------
static FORCEINLINE void cr_set(PPCContext *c, int field, uint32_t v4) {
    int sh = (7 - field) * 4;
    c->cr = (c->cr & ~(0xFu << sh)) | (v4 << sh);
}
static FORCEINLINE uint32_t cr_bit(PPCContext *c, int bit) { return (c->cr >> (31 - bit)) & 1; }
static FORCEINLINE void cr_setbit(PPCContext *c, int bit, uint32_t v) {
    c->cr = (c->cr & ~(1u << (31 - bit))) | ((v & 1) << (31 - bit));
}
static FORCEINLINE void cmp_s(PPCContext *c, int f, int32_t a, int32_t b) {
    cr_set(c, f, (a < b ? 8 : a > b ? 4 : 2) | (c->xer_so & 1));
}
static FORCEINLINE void cmp_u(PPCContext *c, int f, uint32_t a, uint32_t b) {
    cr_set(c, f, (a < b ? 8 : a > b ? 4 : 2) | (c->xer_so & 1));
}
static FORCEINLINE void cmp_f(PPCContext *c, int f, double a, double b) {
    uint32_t v = (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1;
    cr_set(c, f, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}
#define REC(c, v) cmp_s((c), 0, (int32_t)(v), 0)

static FORCEINLINE uint32_t rotl32(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static FORCEINLINE uint32_t cntlz32(uint32_t v) {
#if defined(_MSC_VER)
    unsigned long i; return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
    return v ? __builtin_clz(v) : 32;
#endif
}

// ---- Runtime services used by generated code ---------------------------------
typedef void (*GuestFn)(PPCContext *);
void call_indirect(PPCContext *c, uint32_t addr);
void ppc_sc(PPCContext *c, uint32_t pc);
void ppc_rfi(PPCContext *c, uint32_t pc);
void ppc_trap(PPCContext *c, uint32_t pc);
void ppc_unimpl(PPCContext *c, uint32_t pc, uint32_t insn);
void ppc_bad_jump(PPCContext *c, uint32_t pc, uint32_t target);
uint32_t ppc_mftb(PPCContext *c, int upper);
void ppc_mtspr(PPCContext *c, int spr, uint32_t v);
uint32_t ppc_mfspr(PPCContext *c, int spr);
void psq_load(PPCContext *c, int frd, uint32_t ea, int w, int i);
void psq_store(PPCContext *c, int frs, uint32_t ea, int w, int i);
void ppc_dcbz(uint32_t ea);
void ppc_dcbz_l(uint32_t ea);
double ppc_fctiw(PPCContext *c, double v, int trunc);
void ppc_lswi(PPCContext *c, int rd, uint32_t ea, int nb);
void ppc_stswi(PPCContext *c, int rs, uint32_t ea, int nb);

// ---- interrupts (irq.c) ---------------------------------------------------------
extern volatile int g_irq_pending;
void irq_check(PPCContext *c);
void irq_dec_written(uint32_t value);
