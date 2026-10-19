#pragma once
#include <stdint.h>

void gx_fifo_write(uint32_t v, int size);   // CPU write-gather pipe (0xCC008000)
int gx_vertex_size(int vat);

// Command handlers (gx_render.c), called by the FIFO / display-list parser.
void gx_cp_write(uint8_t reg, uint32_t v);
void gx_xf_write(uint32_t first, uint32_t count, const uint8_t *data);
void gx_load_indx(int array, uint32_t v);
void gx_bp_write(uint32_t v);
void gx_draw(uint8_t op, uint32_t count, const uint8_t *data);
uint32_t gx_cp_vcd_lo(void);
uint32_t gx_cp_vcd_hi(void);
uint32_t gx_cp_vat(int which, int fmt);
void gx_texture_end_frame(void);

// Pixel engine events raised by the command stream (hw.c).
void gx_pe_finish(void);
void gx_pe_token(uint16_t token, int interrupt);
void gx_cp_reg_written(uint32_t off);      // after a CP MMIO register write (0xCC0000xx)
void gx_gather_reset(void);                // mtspr WPAR
int gx_fifo_idle(void);                    // GPU thread has parsed everything sent so far
