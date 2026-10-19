// DVD interface (0xCC006000), backed by the user's disc image.
// Commands complete instantly and raise the transfer-complete interrupt; the
// SDK's DVD library (queues, callbacks, timeouts) runs unmodified on top.
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"

void iso_read(uint64_t off, void *dst, uint32_t len);

enum { DISR = 0, DICVR, DICMDBUF0, DICMDBUF1, DICMDBUF2, DIMAR, DILENGTH, DICR, DIIMMBUF, DICFG };
static uint32_t di[10];

#define DISR_INTS 0x54u  // DEINT | TCINT | BRKINT (write 1 to clear)

static void di_dma_to_ram(uint64_t disc_off, uint32_t len) {
    uint32_t mar = di[DIMAR] & 0x01FFFFE0u;
    if (mar + len > RAM_SIZE) fatal("DVD DMA out of range: %08X+%X", di[DIMAR], len);
    iso_read(disc_off, g_ram + mar, len);
    di[DIMAR] += len;
    di[DILENGTH] = 0;
}

static void di_execute(void) {
    uint32_t cmd = di[DICMDBUF0];
    switch (cmd >> 24) {
    case 0xA8:  // read (sub 0x40 = read disc ID, otherwise normal read)
        if ((cmd & 0xFF) == 0x40) di_dma_to_ram(0, di[DILENGTH] ? di[DILENGTH] : 0x20);
        else di_dma_to_ram((uint64_t)di[DICMDBUF1] << 2, di[DILENGTH]);
        break;
    case 0x12: {  // inquiry: drive revision / date
        static const uint8_t info[0x20] = {0x00, 0x00, 0x00, 0x02, 0x20, 0x02, 0x04, 0x02, 0x61};
        uint32_t mar = di[DIMAR] & 0x01FFFFE0u;
        memcpy(g_ram + mar, info, sizeof info);
        di[DIMAR] += sizeof info; di[DILENGTH] = 0;
        break;
    }
    case 0xE0: di[DIIMMBUF] = 0; break;  // request error: none
    case 0xE2: di[DIIMMBUF] = 0; break;  // audio stream status
    case 0xAB: case 0xE1: case 0xE3: case 0xE4: break;  // seek / audio / stop motor / audio buffer config
    default:
        logf_("[dvd] unhandled command %08X %08X %08X", cmd, di[DICMDBUF1], di[DICMDBUF2]);
        di[DIIMMBUF] = 0;
        break;
    }
    di[DICR] &= ~1u;     // TSTART cleared: done
    di[DISR] |= 0x10;    // TCINT
    g_irq_pending = 1;
}

int di_read(uint32_t off, uint32_t *out) {
    int r = (off - 0x6000) / 4;
    if (r > DICFG) return 0;
    *out = di[r];
    return 1;
}

int di_write(uint32_t off, uint32_t v) {
    int r = (off - 0x6000) / 4;
    if (r > DICFG) return 0;
    switch (r) {
    case DISR:
        di[DISR] = (v & ~DISR_INTS) | (di[DISR] & DISR_INTS & ~v);
        break;
    case DICVR:
        di[DICVR] = (v & 2) | (di[DICVR] & 4 & ~v);  // cover closed; CVRINT w1c
        break;
    case DICR:
        di[DICR] = v;
        if (v & 1) di_execute();
        break;
    case DICFG:
        break;
    default:
        di[r] = v;
    }
    return 1;
}

int di_irq_pending(void) {
    uint32_t s = di[DISR], c = di[DICVR];
    return ((s & 0x04) && (s & 0x02)) || ((s & 0x10) && (s & 0x08)) || ((s & 0x40) && (s & 0x20)) ||
           ((c & 0x04) && (c & 0x02));
}
