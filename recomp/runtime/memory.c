// Guest memory: main RAM, locked cache and the hardware register (MMIO) space.
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"

uint8_t *g_ram;
uint8_t g_lockedcache[0x4000];

uint8_t *slow_ptr(uint32_t a, int size, int write) {
    if ((a & 0xFFFFC000u) == 0xE0000000u && (a & 0x3FFF) + size <= 0x4000)
        return g_lockedcache + (a & 0x3FFF);
    // Untranslated (physical) addresses fault on real hardware: these are
    // almost always NULL-pointer dereferences, so report them.
    fatal("bad guest %s of %d bytes at %08X", write ? "write" : "read", size, a);
}

// ---- MMIO -----------------------------------------------------------------
// Register shadow for everything not explicitly emulated: reads return the
// last value written. Every distinct register is logged the first time it is
// touched so we can see what the game depends on.
static uint8_t s_regs[0x10000];
static uint8_t s_seen_r[0x10000], s_seen_w[0x10000];

static const char *block_name(uint32_t off) {
    switch (off & 0xFC00) {
    case 0x0000: return "CP";
    case 0x1000: return "PE";
    case 0x2000: return "VI";
    case 0x3000: return "PI";
    case 0x4000: return "MI";
    case 0x5000: return "DSP";
    case 0x6000: return "DI";
    case 0x6400: return "SI";
    case 0x6800: return "EXI";
    case 0x6C00: return "AI";
    case 0x8000: return "GXFIFO";
    default:     return "?";
    }
}

uint32_t shadow_read(uint32_t off, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | s_regs[(off + i) & 0xFFFF];
    return v;
}
void shadow_write(uint32_t off, uint32_t v, int size) {
    for (int i = size - 1; i >= 0; i--) { s_regs[(off + i) & 0xFFFF] = (uint8_t)v; v >>= 8; }
}

uint32_t mmio_read(uint32_t addr, int size) {
    uint32_t off = addr & 0xFFFF;
    uint32_t v;
    if (hw_read(off, size, &v)) return v;
    v = shadow_read(off, size);
    if (!s_seen_r[off]) {
        s_seen_r[off] = 1;
        logf_("[mmio] first read%d  %s %08X -> %08X", size * 8, block_name(off), addr, v);
    }
    return v;
}

void mmio_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = addr & 0xFFFF;
    if (hw_write(off, v, size)) return;
    shadow_write(off, v, size);
    if (!s_seen_w[off]) {
        s_seen_w[off] = 1;
        logf_("[mmio] first write%d %s %08X <- %08X", size * 8, block_name(off), addr, v);
    }
}
