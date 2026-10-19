// Interrupt delivery for recompiled code.
//
// Real hardware interrupts at any instruction. Here, generated code checks
// g_irq_pending at safe points (backward branches and mtmsr) and calls
// irq_check(), which delivers a pending external interrupt or decrementer
// exception by calling the SDK's own handler from the exception table at
// 0x80003000, exactly as the exception vectors would.
//
// The game creates no threads of its own (OSCreateThread is dead-stripped),
// so OSLoadContext is only ever used to (a) return from an interrupt handler
// to the interrupted code, or (b) "switch" back to the one running thread.
// (a) is a longjmp to the delivery point, (b) is a plain return.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#pragma comment(lib, "winmm.lib")
#include "ppc.h"
#include "host.h"
#include "hw.h"
#include "../generated/funcs.h"
#include "../generated/symbols.h"

volatile int g_irq_pending;

#define EXC_EXTERNAL 4
#define EXC_DECREMENTER 8
#define OS_CURRENT_CONTEXT 0x800000D4u
#define OS_EXCEPTION_TABLE 0x80003000u
#define MSR_EE 0x8000u

// ---- decrementer --------------------------------------------------------------
static uint64_t s_dec_deadline;
static int s_dec_armed;

void irq_dec_written(uint32_t value) {
    // DEC raises an exception when it goes from 0 to -1.
    s_dec_deadline = host_time_ticks() + (uint64_t)(int64_t)(int32_t)value;
    s_dec_armed = (int32_t)value >= 0;
    g_irq_pending = 1;
}

// ---- timer thread: just makes sure safe points look at time-based sources ----
static DWORD WINAPI irq_timer(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    for (;;) { Sleep(1); g_irq_pending = 1; }
}

void irq_init(void) {
    CreateThread(NULL, 0, irq_timer, NULL, 0, NULL);
}

// ---- delivery --------------------------------------------------------------------
static int s_in_irq;
static uint32_t s_irq_ctx;
static jmp_buf s_irq_jb;

static void save_context(PPCContext *c, uint32_t ctx) {
    for (int i = 0; i < 32; i++) wr32(ctx + 4 * i, c->r[i]);
    wr32(ctx + 0x80, c->cr);
    wr32(ctx + 0x84, c->lr);
    wr32(ctx + 0x88, c->ctr);
    wr32(ctx + 0x8C, (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29));
    wr32(ctx + 0x198, c->lr);     // SRR0: exact interrupted PC is not tracked
    wr32(ctx + 0x19C, c->msr);    // SRR1
    wr16(ctx + 0x1A2, rd16(ctx + 0x1A2) | 2);  // OS_CONTEXT_STATE_EXC
}

static void deliver(PPCContext *c, int exc) {
    uint32_t handler = rd32(OS_EXCEPTION_TABLE + 4 * exc);
    uint32_t ctx = rd32(OS_CURRENT_CONTEXT);
    if (!handler || !ctx) return;
    save_context(c, ctx);

    // Handlers run on the interrupted stack with interrupts disabled. Use a
    // scratch register file so the interrupted code's registers are untouched.
    static PPCContext t;
    t = *c;
    t.msr &= ~MSR_EE;
    t.r[1] = (c->r[1] - 0x400) & ~0xFu;
    t.r[3] = exc;
    t.r[4] = ctx;

    s_in_irq = 1;
    s_irq_ctx = ctx;
    if (!setjmp(s_irq_jb)) call_indirect(&t, handler);
    s_in_irq = 0;
    wr16(ctx + 0x1A2, rd16(ctx + 0x1A2) & ~2);
}

// ---- deferred callbacks ---------------------------------------------------------------
// HLE'd async APIs (CARD*Async etc.) complete immediately but must not call
// back before returning: the real callbacks arrive later from an interrupt.
// They are queued here and run at the next interrupt safe point.
static struct { uint32_t fn, r3, r4; } s_cbq[64];
static int s_cbq_head, s_cbq_count;

void irq_queue_callback(uint32_t fn, uint32_t r3, uint32_t r4) {
    if (s_cbq_count == 64) fatal("callback queue overflow");
    int i = (s_cbq_head + s_cbq_count++) % 64;
    s_cbq[i].fn = fn; s_cbq[i].r3 = r3; s_cbq[i].r4 = r4;
    g_irq_pending = 1;
}

static void run_callbacks(PPCContext *c) {
    while (s_cbq_count) {
        int i = s_cbq_head;
        s_cbq_head = (s_cbq_head + 1) % 64; s_cbq_count--;
        static PPCContext t;
        t = *c;
        t.msr &= ~MSR_EE;
        t.r[1] = (c->r[1] - 0x400) & ~0xFu;
        t.r[3] = s_cbq[i].r3; t.r[4] = s_cbq[i].r4;
        s_in_irq = 1;
        call_indirect(&t, s_cbq[i].fn);
        s_in_irq = 0;
    }
}

void irq_check(PPCContext *c) {
    g_irq_pending = 0;
    if (s_in_irq) return;
    hw_update();  // advance time-based hardware (VI retrace etc.)
    if (!(c->msr & MSR_EE)) { g_irq_pending = 1; return; }  // look again once enabled
    if (s_cbq_count) run_callbacks(c);
    if (s_dec_armed && host_time_ticks() >= s_dec_deadline) {
        s_dec_armed = 0;
        deliver(c, EXC_DECREMENTER);
    }
    if (hw_pi_pending()) deliver(c, EXC_EXTERNAL);
}

// ---- HLE: context switching -------------------------------------------------------
void SYM_OSLoadContext(PPCContext *c) {
    if (s_in_irq && c->r[3] == s_irq_ctx) longjmp(s_irq_jb, 1);  // return from interrupt
    // Otherwise: scheduler resuming the only thread there is. Nothing to do.
}
