// Debug helpers: dump the guest call stack (derived from the native stack of
// recompiled fn_XXXXXXXX frames) on hangs and crashes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "func_table.h"

#pragma comment(lib, "dbghelp.lib")

static HANDLE s_main_thread;
static volatile LONG s_dumping;

static void dump_stack(CONTEXT *live, HANDLE thread, const char *why) {
    if (InterlockedExchange(&s_dumping, 1)) return;
    CONTEXT copy = *live;  // StackWalk64 unwinds the context it is given
    CONTEXT *ctx = &copy;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    STACKFRAME64 sf = {0};
    sf.AddrPC.Offset = ctx->Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx->Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx->Rsp; sf.AddrStack.Mode = AddrModeFlat;
    logf_("---- guest call stack (%s), innermost first ----", why);
    char buf[sizeof(SYMBOL_INFO) + 256];
    for (int depth = 0; depth < 64; depth++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (!SymFromAddr(proc, sf.AddrPC.Offset, &disp, si)) continue;
        unsigned addr;
        if (sscanf(si->Name, "fn_%8X", &addr) == 1 || sscanf(si->Name, "orig_%8X", &addr) == 1) {
            const FuncEntry *e = func_lookup(addr);
            logf_("  %08X %s", addr, e && e->name[0] ? e->name : "");
        } else {
            logf_("  [host] %s", si->Name);
        }
    }
    InterlockedExchange(&s_dumping, 0);
}

static DWORD WINAPI watchdog(LPVOID arg) {
    DWORD secs = (DWORD)(uintptr_t)arg;
    Sleep(secs * 1000);
    SuspendThread(s_main_thread);
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(s_main_thread, &ctx);
    dump_stack(&ctx, s_main_thread, "watchdog");
    ResumeThread(s_main_thread);
    return 0;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    logf_("host exception %08lX at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    dump_stack(ep->ContextRecord, GetCurrentThread(), "crash");
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- guest write watchpoint (TUROK_WATCH=<hex guest addr>) ------------------
// Write-protects the host page backing the address; on a fault that hits the
// watched word, dumps the guest stack. The page is unprotected for one
// single-stepped instruction and then protected again.
static uint8_t *s_watch_host;
static uint8_t *s_watch_page;
static uint32_t s_watch_guest;

static void watch_protect(int on) {
    DWORD old;
    VirtualProtect(s_watch_page, 4096, on ? PAGE_READONLY : PAGE_READWRITE, &old);
}

static LONG WINAPI watch_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uint8_t *a = (uint8_t *)ep->ExceptionRecord->ExceptionInformation[1];
        if (a >= s_watch_page && a < s_watch_page + 4096) {
            if (a + 8 > s_watch_host && a < s_watch_host + 4) {
                logf_("[watch] write to %08X (word was %08X)", s_watch_guest, BSWAP32(*(uint32_t *)s_watch_host));
                dump_stack(ep->ContextRecord, GetCurrentThread(), "watchpoint");
            }
            watch_protect(0);
            ep->ContextRecord->EFlags |= 0x100;  // single step, then re-protect
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && s_watch_page) {
        watch_protect(1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Call once guest RAM is allocated.
void debug_arm_watch(void) {
    const char *w = getenv("TUROK_WATCH");
    if (!w) return;
    s_watch_guest = (uint32_t)strtoul(w, NULL, 16);
    s_watch_host = g_ram + (s_watch_guest & 0x01FFFFFF);
    s_watch_page = (uint8_t *)((uintptr_t)s_watch_host & ~(uintptr_t)4095);
    AddVectoredExceptionHandler(1, watch_handler);
    watch_protect(1);
    logf_("[watch] armed on %08X", s_watch_guest);
}

void debug_init(void) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main_thread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    SetUnhandledExceptionFilter(on_crash);
    const char *w = getenv("TUROK_WATCHDOG");
    DWORD secs = w ? (DWORD)atoi(w) : 0;
    if (secs) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)secs, 0, NULL);
}

void debug_dump_here(const char *why) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    dump_stack(&ctx, GetCurrentThread(), why);
}
