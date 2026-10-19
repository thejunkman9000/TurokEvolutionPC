#pragma once
#include <stdint.h>

#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#else
#define NORETURN __attribute__((noreturn))
#endif

NORETURN void fatal(const char *fmt, ...);
void logf_(const char *fmt, ...);
uint64_t host_time_ticks(void);  // guest time base ticks (40.5 MHz) since boot

#define TB_FREQ 40500000ull

void debug_init(void);
void debug_dump_here(const char *why);
void debug_arm_watch(void);
