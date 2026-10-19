// PAD: controller input from the host replaces the SI serial protocol.
//
// The game reads all four controllers once per frame with PADRead(PADStatus[4]);
// only the SDK itself uses the SI sampling callback, so PADRead is the single
// hook needed. PADStatus (12 bytes): u16 button, s8 stickX, stickY, substickX,
// substickY, u8 triggerL, triggerR, analogA, analogB, s8 err, pad.
#include "ppc.h"
#include "host.h"
#include "../generated/funcs.h"
#include "../generated/symbols.h"

typedef struct { uint16_t buttons; int8_t sx, sy, cx, cy; uint8_t l, r; } PadState;
extern PadState g_pads[4];
extern int g_cy_override;

#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER -1

void SYM_PADRead(PPCContext *c) {
    uint32_t status = c->r[3];
    for (int port = 0; port < 4; port++) {
        uint32_t p = status + 12 * port;
        for (int i = 0; i < 12; i++) wr8(p + i, 0);
        if (port != 0) { wr8(p + 10, (uint8_t)PAD_ERR_NO_CONTROLLER); continue; }
        PadState s = g_pads[port];
        wr16(p + 0, s.buttons);
        wr8(p + 2, (uint8_t)s.sx);
        wr8(p + 3, (uint8_t)s.sy);
        wr8(p + 4, (uint8_t)s.cx);
        wr8(p + 5, (uint8_t)(g_cy_override >= -128 && g_cy_override <= 127 ? g_cy_override : s.cy));
        wr8(p + 6, s.l);
        wr8(p + 7, s.r);
        wr8(p + 8, (s.buttons & 0x0100) ? 0xFF : 0);  // analog A
        wr8(p + 9, (s.buttons & 0x0200) ? 0xFF : 0);  // analog B
        wr8(p + 10, PAD_ERR_NONE);
    }
    c->r[3] = 0;  // no controllers being reset
}
