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
