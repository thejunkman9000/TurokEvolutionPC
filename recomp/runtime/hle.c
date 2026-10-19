// High-level replacements for SDK functions (listed in recomp/hle_overrides.txt).
// Each is called exactly like the recompiled function would be: arguments in
// r3..r10 / f1..f8, result in r3 / f1, and returning == guest `blr`.
#include "ppc.h"
#include "host.h"
#include "../generated/funcs.h"
#include "../generated/symbols.h"

// Boots the DSP with a tiny init program and waits on its mailbox replies.
// Audio is high-level emulated, so there is nothing to do.
void SYM___OSInitAudioSystem(PPCContext *c) { (void)c; }
void SYM___OSStopAudioSystem(PPCContext *c) { (void)c; }

// VIWaitForRetrace: timed, so [stats] can report how long each frame keeps the
// guest busy (everything except this wait).
volatile uint64_t g_perf_idle_tsc, g_perf_max_busy_tsc;  // max: longest stretch between waits
void SYM_VIWaitForRetrace(PPCContext *c) {
    static uint64_t s_last_end;
    uint64_t t = __rdtsc();
    if (s_last_end && t - s_last_end > g_perf_max_busy_tsc) g_perf_max_busy_tsc = t - s_last_end;
    orig_80143344(c);
    s_last_end = __rdtsc();
    g_perf_idle_tsc += s_last_end - t;
}
