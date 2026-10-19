// turok_core.dll: loads main.dol from the user's disc image into guest RAM,
// sets up the state the GameCube boot ROM (IPL) normally leaves behind, and
// runs the recompiled __start on a dedicated guest thread. The host (MonoGame
// or the console runner) talks to it through the API in core_api.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "ppc.h"
#include "host.h"
#include "func_table.h"
#include "hw.h"
#include "core_api.h"
#include "../generated/funcs.h"

static PPCContext g_ctx;
static FILE *g_log;
static LARGE_INTEGER s_qpf, s_qpc0;
static CRITICAL_SECTION s_log_lock;
static volatile LONG s_state = CORE_NOT_STARTED;
static char s_fatal_msg[512];
static uint32_t s_entry;
FILE *g_iso;

void logf_(const char *fmt, ...) {
    va_list ap;
    EnterCriticalSection(&s_log_lock);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fputc('\n', g_log); fflush(g_log); }
    LeaveCriticalSection(&s_log_lock);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vsnprintf(s_fatal_msg, sizeof s_fatal_msg, fmt, ap); va_end(ap);
    logf_("FATAL: %s", s_fatal_msg);
    debug_dump_here("fatal");
    const FuncEntry *f = func_containing(g_ctx.lr);
    logf_("  lr=%08X (%s+%X) ctr=%08X", g_ctx.lr, f && f->name[0] ? f->name : "?", f ? g_ctx.lr - f->addr : 0, g_ctx.ctr);
    for (int i = 0; i < 32; i += 8)
        logf_("  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X", i, g_ctx.r[i], g_ctx.r[i + 1], g_ctx.r[i + 2],
              g_ctx.r[i + 3], g_ctx.r[i + 4], g_ctx.r[i + 5], g_ctx.r[i + 6], g_ctx.r[i + 7]);
    InterlockedExchange(&s_state, CORE_FAILED);
    ExitThread(1);  // stop the guest; the host stays up and reports the error
}

uint64_t host_time_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t dt = (uint64_t)(now.QuadPart - s_qpc0.QuadPart);
    return (dt / s_qpf.QuadPart) * TB_FREQ + (dt % s_qpf.QuadPart) * TB_FREQ / s_qpf.QuadPart;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint32_t a, uint32_t v) { wr32(a, v); }

void iso_read(uint64_t off, void *dst, uint32_t len) {
    _fseeki64(g_iso, (long long)off, SEEK_SET);
    if (fread(dst, 1, len, g_iso) != len) fatal("short read from disc image at %llx", (unsigned long long)off);
}
static uint8_t *iso_alloc_read(uint64_t off, uint32_t len) {
    uint8_t *buf = malloc(len);
    iso_read(off, buf, len);
    return buf;
}

static int load_boot(void) {
    // Disc header: game ID etc. goes to 0x80000000, DOL/FST offsets at 0x420.
    uint8_t *hdr = iso_alloc_read(0, 0x440);
    if (memcmp(hdr, "GTKE51", 6) != 0) {
        snprintf(s_fatal_msg, sizeof s_fatal_msg, "not Turok: Evolution (NTSC-U, GTKE51)");
        free(hdr);
        return 0;
    }
    uint32_t dol_off = be32(hdr + 0x420), fst_off = be32(hdr + 0x424), fst_size = be32(hdr + 0x428);

    uint8_t *dh = iso_alloc_read(dol_off, 0x100);
    // The DOL "bss" range spans .sdata too, so clear it *before* loading sections.
    uint32_t bss = be32(dh + 0xD8), bss_size = be32(dh + 0xDC);
    s_entry = be32(dh + 0xE0);
    memset(g_ram + (bss & 0x01FFFFFF), 0, bss_size);
    for (int i = 0; i < 18; i++) {
        uint32_t o = be32(dh + 4 * i), a = be32(dh + 0x48 + 4 * i), s = be32(dh + 0x90 + 4 * i);
        if (s) iso_read(dol_off + o, g_ram + (a & 0x01FFFFFF), s);
    }

    // Boot ROM state (see Dolphin's BS2 HLE).
    memcpy(g_ram, hdr, 0x20);                 // disc ID
    put32(0x80000020, 0x0D15EA5E);            // boot magic
    put32(0x80000024, 1);                     // version
    put32(0x80000028, RAM_SIZE);              // physical memory size
    put32(0x8000002C, 1);                     // console type: retail
    put32(0x80000030, 0);                     // arena low (0 = use linker default)
    uint32_t fst_addr = (0x81800000u - fst_size) & ~31u;
    iso_read(fst_off, g_ram + (fst_addr & 0x01FFFFFF), fst_size);
    put32(0x80000034, fst_addr);              // arena high = below FST
    put32(0x80000038, fst_addr);              // FST location
    put32(0x8000003C, fst_size);              // FST max length
    put32(0x800000CC, 0);                     // video mode NTSC
    put32(0x800000F0, RAM_SIZE);              // simulated memory size
    put32(0x800000F8, 162000000);             // bus clock
    put32(0x800000FC, 486000000);             // CPU clock
    put32(0x800030D8, 0);                     // system time
    free(hdr); free(dh);
    return 1;
}

static DWORD WINAPI guest_thread(LPVOID arg) {
    (void)arg;
    debug_init();  // watchdog / crash dumps target this thread
    debug_arm_watch();
    g_ctx.msr = 0x00002032;
    logf_("loaded main.dol, entry %08X; running", s_entry);
    const FuncEntry *e = func_lookup(s_entry);
    if (!e) fatal("no recompiled function for entry %08X", s_entry);
    InterlockedExchange(&s_state, CORE_RUNNING);
    e->fn(&g_ctx);
    logf_("__start returned (r3=%08X)", g_ctx.r[3]);
    InterlockedExchange(&s_state, CORE_EXITED);
    return 0;
}

CORE_API int core_start(const char *iso_path, const char *log_path) {
    if (s_state != CORE_NOT_STARTED) return 0;
    InitializeCriticalSection(&s_log_lock);
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_qpc0);
    g_log = fopen(log_path && *log_path ? log_path : "turok_recomp.log", "w");

    g_ram = VirtualAlloc(NULL, RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_iso = fopen(iso_path, "rb");
    if (!g_iso) {
        snprintf(s_fatal_msg, sizeof s_fatal_msg, "cannot open disc image '%s'", iso_path);
        s_state = CORE_FAILED;
        return 0;
    }
    if (!load_boot()) { s_state = CORE_FAILED; return 0; }
    irq_init();
    s_state = CORE_STARTING;
    HANDLE t = CreateThread(NULL, 64u << 20, guest_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { s_state = CORE_FAILED; return 0; }
    CloseHandle(t);
    return 1;
}

CORE_API const uint8_t *core_ram(uint32_t *size) {
    if (size) *size = RAM_SIZE;
    return g_ram;
}

CORE_API int core_state(char *msg, int msg_len) {
    if (msg && msg_len > 0) snprintf(msg, msg_len, "%s", s_fatal_msg);
    return s_state;
}
