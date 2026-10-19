#pragma once
#include <stdint.h>

// Emulated hardware registers. Return 1 if the access was handled.
int hw_read(uint32_t off, int size, uint32_t *out);
int hw_write(uint32_t off, uint32_t v, int size);

uint32_t shadow_read(uint32_t off, int size);
void shadow_write(uint32_t off, uint32_t v, int size);

void hw_update(void);       // advance time-based hardware state
int hw_pi_pending(void);     // PI interrupt cause & mask != 0
void irq_init(void);

int di_read(uint32_t off, uint32_t *out);
int di_write(uint32_t off, uint32_t v);
int di_irq_pending(void);
void irq_queue_callback(uint32_t fn, uint32_t r3, uint32_t r4);  // run guest fn(r3, r4) at the next safe point
