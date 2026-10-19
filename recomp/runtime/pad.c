// Controller input from the host, read by the PADRead HLE (hle_pad.c).
#include <string.h>
#include "core_api.h"

typedef struct { uint16_t buttons; int8_t sx, sy, cx, cy; uint8_t l, r; } PadState;
PadState g_pads[4];
int g_cy_override = 1000;  // raw C-stick Y forced by mouse look (hle_look.c) when in [-128, 127]
int g_pad_cy;              // the host controller's raw C-stick Y

CORE_API void core_set_pad(int port, uint16_t buttons, int8_t sx, int8_t sy, int8_t cx, int8_t cy, uint8_t l, uint8_t r) {
    if (port < 0 || port > 3) return;
    PadState p = {buttons, sx, sy, cx, cy, l, r};
    g_pads[port] = p;
    if (port == 0) g_pad_cy = cy;
}
