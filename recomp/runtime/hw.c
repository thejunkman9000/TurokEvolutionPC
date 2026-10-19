// Minimal hardware register behaviour needed to get through SDK init.
// Anything not handled here falls back to the register shadow in memory.c.
#include <stdio.h>
#include <time.h>
#include "ppc.h"
#include "hw.h"
#include "host.h"
#include "gx.h"

// ============================================================================
// EXI (0xCC006800): 3 channels x 5 registers. Transfers complete instantly.
// Channel 0 / device 1 is the boot ROM chip: RTC, SRAM and the debug UART.
// ============================================================================
enum { EXI_CSR = 0, EXI_MAR = 1, EXI_LEN = 2, EXI_CR = 3, EXI_DATA = 4 };
static uint32_t exi[3][5];

typedef struct {
    uint32_t addr;   // command word received after chip select
    int have_addr;
    uint32_t cursor;
} RomDev;
static RomDev s_rom;
static uint8_t s_sram[64];
static char s_uart[1024];
static int s_uart_len;

static void sram_init(void) {
    memset(s_sram, 0, sizeof s_sram);
    s_sram[0x12] = 0;     // language: English
    s_sram[0x13] = 0x2C;  // flags (as Dolphin's default SRAM)
    uint16_t sum = 0, inv = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t w = (uint16_t)(s_sram[0x0C + 2 * i] << 8 | s_sram[0x0D + 2 * i]);
        sum += w; inv += (uint16_t)~w;
    }
    s_sram[0] = sum >> 8; s_sram[1] = (uint8_t)sum;
    s_sram[2] = inv >> 8; s_sram[3] = (uint8_t)inv;
}

static uint32_t rtc_seconds(void) {
    // GameCube RTC counts seconds since 2000-01-01.
    return (uint32_t)(time(NULL) - 946684800);
}

static void uart_putc(char ch) {
    if (ch == '\n' || ch == '\r' || s_uart_len >= (int)sizeof s_uart - 1) {
        if (s_uart_len) { s_uart[s_uart_len] = 0; logf_("[OSReport] %s", s_uart); s_uart_len = 0; }
        return;
    }
    s_uart[s_uart_len++] = ch;
}

// Transfer one byte to/from the ROM device at the current cursor.
static uint8_t rom_byte(int write, uint8_t in) {
    uint32_t a = s_rom.addr & 0x7FFFFF00u;
    uint32_t i = s_rom.cursor++;
    if (a == 0x20000000u) {  // RTC (4 bytes, big endian)
        uint32_t t = rtc_seconds();
        return write ? 0 : (uint8_t)(t >> (24 - 8 * (i & 3)));
    }
    if ((s_rom.addr & 0x7FFFFFFFu) >= 0x20000100u && (s_rom.addr & 0x7FFFFFFFu) < 0x20001100u) {  // SRAM
        uint32_t off = (((s_rom.addr & 0x7FFFFFFFu) - 0x20000100u) >> 6) + i;
        if (off >= 64) return 0;
        if (write) { s_sram[off] = in; return 0; }
        return s_sram[off];
    }
    if (a == 0x20010000u) {  // UART
        if (write && in) uart_putc((char)in);
        return 0;
    }
    return 0;  // IPL ROM font/code reads: not provided
}

static void exi_transfer(int ch) {
    uint32_t cr = exi[ch][EXI_CR];
    int dma = (cr >> 1) & 1, rw = (cr >> 2) & 3, len = ((cr >> 4) & 3) + 1;
    int dev = (exi[ch][EXI_CSR] >> 7) & 7;  // one-hot CS0..CS2
    int is_rom = ch == 0 && dev == 2;
    if (dma) {
        uint32_t mar = exi[ch][EXI_MAR] & 0x03FFFFE0u, n = exi[ch][EXI_LEN] & 0x03FFFFE0u;
        for (uint32_t k = 0; k < n; k++) {
            if (rw == 0) g_ram[(mar + k) % RAM_SIZE] = is_rom ? rom_byte(0, 0) : 0xFF;
            else if (is_rom) rom_byte(1, g_ram[(mar + k) % RAM_SIZE]);
        }
    } else {
        uint32_t data = exi[ch][EXI_DATA], out = 0;
        if (is_rom && !s_rom.have_addr && rw != 0) {
            s_rom.addr = data; s_rom.have_addr = 1; s_rom.cursor = 0;
        } else {
            for (int k = 0; k < len; k++) {
                uint8_t in = (uint8_t)(data >> (24 - 8 * k));
                uint8_t b = is_rom ? rom_byte(rw != 0, in) : (rw == 0 ? 0xFF : 0);
                out |= (uint32_t)b << (24 - 8 * k);
            }
            if (rw != 1) exi[ch][EXI_DATA] = out;
        }
    }
    exi[ch][EXI_CR] &= ~1u;          // TSTART clear: transfer complete
    exi[ch][EXI_CSR] |= 1u << 3;     // TCINT
}

static int exi_read(uint32_t off, uint32_t *out) {
    int ch = (off - 0x6800) / 0x14, r = ((off - 0x6800) % 0x14) / 4;
    if (ch > 2) return 0;
    uint32_t v = exi[ch][r];
    if (r == EXI_CSR) {
        // EXT (device present): memory card slots empty for now; ROM always there.
        v &= ~(1u << 12);
        if (ch == 0 || ch == 2) v |= 0;  // no card inserted
    }
    *out = v;
    return 1;
}

static int exi_write(uint32_t off, uint32_t v) {
    int ch = (off - 0x6800) / 0x14, r = ((off - 0x6800) % 0x14) / 4;
    if (ch > 2) return 0;
    if (r == EXI_CSR) {
        uint32_t old_cs = (exi[ch][EXI_CSR] >> 7) & 7;
        // Interrupt status bits (EXIINT bit1, TCINT bit3, EXTINT bit11) are write-1-to-clear.
        uint32_t ack = v & ((1u << 1) | (1u << 3) | (1u << 11));
        exi[ch][EXI_CSR] = (v & ~ack & ~((1u << 1) | (1u << 3) | (1u << 11))) |
                           (exi[ch][EXI_CSR] & ((1u << 1) | (1u << 3) | (1u << 11)) & ~ack);
        uint32_t new_cs = (exi[ch][EXI_CSR] >> 7) & 7;
        if (ch == 0 && old_cs != new_cs) { s_rom.have_addr = 0; s_rom.cursor = 0; }
        return 1;
    }
    exi[ch][r] = v;
    if (r == EXI_CR && (v & 1)) exi_transfer(ch);
    return 1;
}

// ============================================================================
// DSP interface (0xCC005000): mailboxes, control/status, ARAM and AI DMA.
// The DSP itself is not run; ARAM DMA completes instantly.
// A fake DSP answers the boot ROM handshake and the SDK task protocol
// (DSP_INIT after the microcode upload); later mails are logged for now.
// ============================================================================
#define ARAM_SIZE 0x01000000u
static uint8_t *s_aram;
static uint16_t dsp[0x40 / 2];

// DSP -> CPU mail queue. irq: raise DSPINT when this mail becomes current.
static struct { uint32_t mail; int irq; } s_dmail[64];
static int s_dmail_head, s_dmail_count;
static uint32_t s_cmail_hi;       // CPU -> DSP high half (bit 15 = pending)
static int s_boot_step = -1;      // -1: ROM waiting; >=0: counting boot mails
static uint32_t s_ai_cr;  // AI control (AI section below)

void dsp_push_mail(uint32_t mail, int irq) {
    if (s_dmail_count == 64) { logf_("[dsp] mail queue full"); return; }
    s_dmail[(s_dmail_head + s_dmail_count++) % 64].mail = mail;
    s_dmail[(s_dmail_head + s_dmail_count - 1) % 64].irq = irq;
    if (s_dmail_count == 1 && irq) { dsp[0x0A / 2] |= 1u << 7; g_irq_pending = 1; }
}

static void dsp_reset(void) {
    s_dmail_head = s_dmail_count = 0;
    s_boot_step = 0;
    dsp_push_mail(0x8071FEED, 0);  // boot ROM ready
}

static uint32_t s_boot_mail[10];
void ax_mail(uint32_t mail);
uint8_t *hw_aram(void) { return s_aram; }

static void dsp_receive(uint32_t mail) {
    if (s_boot_step >= 0 && s_boot_step < 10) {
        s_boot_mail[s_boot_step] = mail;
        if (s_boot_step == 9)
            logf_("[dsp] boot: iram mmem %08X iram addr %04X len %X dram mmem %08X entry %04X",
                  s_boot_mail[1], s_boot_mail[3] & 0xFFFF, s_boot_mail[5], s_boot_mail[7], s_boot_mail[9] & 0xFFFF);
        // __DSP_boot_task: 5 command/value pairs ending with 0x80F3D001 <entry>.
        if (++s_boot_step == 10) {
            logf_("[dsp] microcode booted");
            dsp_push_mail(0xDCD10000, 1);  // DSP_INIT
        }
        return;
    }
    ax_mail(mail);  // microcode running: AX HLE (dsp_ax.c)
}

static void aram_dma(void) {
    uint32_t mm = ((uint32_t)dsp[0x20 / 2] << 16 | dsp[0x22 / 2]) & 0x03FFFFE0u;
    uint32_t ar = ((uint32_t)dsp[0x24 / 2] << 16 | dsp[0x26 / 2]) & 0x03FFFFE0u;
    uint32_t cnt = (uint32_t)dsp[0x28 / 2] << 16 | dsp[0x2A / 2];
    uint32_t n = cnt & 0x03FFFFE0u;
    int to_ram = cnt >> 31;  // 1: ARAM -> main RAM
    for (uint32_t k = 0; k < n; k++) {
        uint8_t *m = &g_ram[(mm + k) % RAM_SIZE], *a = &s_aram[(ar + k) % ARAM_SIZE];
        if (to_ram) *m = *a; else *a = *m;
    }
    dsp[0x28 / 2] = 0; dsp[0x2A / 2] = 0;
    dsp[0x0A / 2] |= 1u << 5;  // ARINT
}

// ---- AI DMA: main-RAM PCM to the audio DAC -----------------------------------------
// 0x5030/0x5032 start address, 0x5036 bit 15 = play, bits 0-14 = length in
// 32-byte blocks. On start (and each time a buffer finishes) the registers are
// latched and AIDINT is raised so the game can queue the next buffer; the
// latched buffer is handed to the host once its play time has elapsed.
static int s_aid_playing;
static uint32_t s_aid_addr, s_aid_len;
static uint64_t s_aid_deadline;
uint32_t ai_dsp_sample_rate(void);
void audio_push_be_stereo(const uint8_t *pcm, uint32_t bytes, uint32_t rate);

static void aid_latch(void) {
    s_aid_addr = ((uint32_t)dsp[0x30 / 2] << 16 | dsp[0x32 / 2]) & 0x03FFFFE0u;
    s_aid_len = (dsp[0x36 / 2] & 0x7FFF) * 32u;
    uint64_t frames = s_aid_len / 4;
    uint64_t dur = frames ? frames * TB_FREQ / ai_dsp_sample_rate() : TB_FREQ / 200;
    // Schedule from the previous deadline so short stalls are caught up instead of
    // permanently drifting behind real time; resync after a long stall (>100 ms).
    uint64_t now = host_time_ticks();
    if (!s_aid_deadline || now > s_aid_deadline + TB_FREQ / 10) s_aid_deadline = now;
    s_aid_deadline += dur;
    dsp[0x0A / 2] |= 1u << 3;  // AIDINT
    g_irq_pending = 1;
}

static void aid_update(void) {
    if (!s_aid_playing) return;
    if (host_time_ticks() < s_aid_deadline) return;
    if (s_aid_len && s_aid_addr + s_aid_len <= RAM_SIZE)
        audio_push_be_stereo(g_ram + s_aid_addr, s_aid_len, ai_dsp_sample_rate());
    aid_latch();
}

static int dsp_read(uint32_t off, int size, uint32_t *out) {
    uint32_t i = (off - 0x5000) / 2;
    if (size == 4) {
        uint32_t hi, lo;
        dsp_read(off, 2, &hi); dsp_read(off + 2, 2, &lo);
        *out = hi << 16 | lo; return 1;
    }
    uint16_t v = dsp[i];
    switch (off) {
    case 0x5000: v = (uint16_t)s_cmail_hi; break;  // DSP consumes mail instantly
    case 0x5004:
        v = s_dmail_count ? (uint16_t)(0x8000 | (s_dmail[s_dmail_head].mail >> 16)) : 0;
        break;
    case 0x5006:
        if (!s_dmail_count) { v = 0; break; }
        v = (uint16_t)s_dmail[s_dmail_head].mail;
        s_dmail_head = (s_dmail_head + 1) % 64; s_dmail_count--;
        if (s_dmail_count && s_dmail[s_dmail_head].irq) { dsp[0x0A / 2] |= 1u << 7; g_irq_pending = 1; }
        break;
    case 0x500A: v &= ~((1u << 0) | (1u << 9) | (1u << 10) | (1u << 11)); break;  // not resetting, no DMA busy
    case 0x5016: v |= 1; break;  // ARAM ready
    case 0x503A: v = 0; break;   // AI DMA bytes left
    }
    *out = v; return 1;
}

static int dsp_write(uint32_t off, uint32_t v, int size) {
    if (size == 4) { dsp_write(off, v >> 16, 2); dsp_write(off + 2, v & 0xFFFF, 2); return 1; }
    uint32_t i = (off - 0x5000) / 2;
    if (off == 0x500A) {
        // Interrupt flags AIDINT(3), ARINT(5), DSPINT(7) are write-1-to-clear.
        uint16_t ack = (uint16_t)(v & ((1u << 3) | (1u << 5) | (1u << 7)));
        uint16_t keep = dsp[i] & ((1u << 3) | (1u << 5) | (1u << 7)) & ~ack;
        dsp[i] = (uint16_t)((v & ~((1u << 3) | (1u << 5) | (1u << 7))) | keep);
        if (v & 1) dsp_reset();  // RES
        return 1;
    }
    if (off == 0x5000) { s_cmail_hi = v & 0x7FFF; return 1; }
    if (off == 0x5002) { dsp_receive((s_cmail_hi | 0x8000) << 16 | (v & 0xFFFF)); return 1; }
    dsp[i] = (uint16_t)v;
    if (off == 0x502A) aram_dma();
    if (off == 0x5036) {
        int play = (v >> 15) & 1;
        if (play && !s_aid_playing) {
            s_aid_playing = 1;
            s_aid_deadline = 0;
            aid_latch();
            static int logged;
            if (!logged) { logged = 1; logf_("[ai] DMA start: AICR %08X -> %u Hz, %u bytes per buffer", s_ai_cr, ai_dsp_sample_rate(), s_aid_len); }
        }
        if (!play) s_aid_playing = 0;
    }
    return 1;
}

// ============================================================================
// VI (0xCC002000): only the display interrupts DI0..DI3 (0x2030..0x203C).
// Each is 32 bits: bit 31 = status, bit 28 = enable. The SDK accesses VI
// registers as 16-bit halves, so status/enable live in the high half.
// Retrace fires at ~59.94 Hz of host time.
// ============================================================================
#define VI_FRAME_TICKS (TB_FREQ * 1001 / 60000)
static uint16_t s_vi_status;  // bit n = DIn status
static uint64_t s_vi_frame;

static int vi_read(uint32_t off, int size, uint32_t *out) {
    if (off < 0x2030 || off >= 0x2040) return 0;
    uint32_t v = shadow_read(off, size);
    int n = (off - 0x2030) / 4;
    if (size == 4 || (off & 3) == 0) {
        uint32_t st = (s_vi_status >> n) & 1;
        if (size == 4) v = (v & ~0x80000000u) | (st << 31);
        else v = (v & ~0x8000u) | (st << 15);
    }
    *out = v;
    return 1;
}

static int vi_write(uint32_t off, uint32_t v, int size) {
    if (off < 0x2030 || off >= 0x2040) return 0;
    shadow_write(off, v, size);
    int n = (off - 0x2030) / 4;
    if ((off & 3) == 0) {
        uint32_t st = size == 4 ? v >> 31 : (v >> 15) & 1;
        if (!st) s_vi_status &= ~(1u << n);  // writing 0 to status acknowledges
    }
    return 1;
}

static void vi_update(void) {
    uint64_t frame = host_time_ticks() / VI_FRAME_TICKS;
    if (frame == s_vi_frame) return;
    s_vi_frame = frame;
    for (int n = 0; n < 4; n++)
        if (shadow_read(0x2030 + 4 * n, 2) & 0x1000) s_vi_status |= 1u << n;
}

// ============================================================================
// PE (0xCC001000): draw-done / token interrupts raised by the GX FIFO parser.
// 0x100A: bit0 token enable, bit1 finish enable, bit2 token status,
//         bit3 finish status (status bits are write-1-to-clear). 0x100E: token.
// ============================================================================
static uint16_t s_pe_ctrl, s_pe_token;

void gx_pe_finish(void) { s_pe_ctrl |= 8; g_irq_pending = 1; }
void gx_pe_token(uint16_t token, int interrupt) {
    s_pe_token = token;
    if (interrupt) { s_pe_ctrl |= 4; g_irq_pending = 1; }
}

static int pe_read(uint32_t off, int size, uint32_t *out) {
    if (off == 0x100A && size == 2) { *out = s_pe_ctrl; return 1; }
    if (off == 0x100E && size == 2) { *out = s_pe_token; return 1; }
    return 0;
}
static int pe_write(uint32_t off, uint32_t v, int size) {
    if (off == 0x100A && size == 2) {
        s_pe_ctrl = (uint16_t)((s_pe_ctrl & 0xC & ~(v & 0xC)) | (v & 3));
        return 1;
    }
    return 0;
}

// ============================================================================
// AI (0xCC006C00): streaming audio control. AICR bit0 PSTAT (playing),
// bit1 AFR (48 kHz), bit2 AIINTMSK, bit3 AIINT (w1c), bit5 SCRESET.
// AISCNT counts samples while playing; AIINT fires when it reaches AIIT.
// ============================================================================
static uint32_t s_ai_vr, s_ai_it, s_ai_base_count;
static uint64_t s_ai_start;

static uint32_t ai_count(void) {
    if (!(s_ai_cr & 1)) return s_ai_base_count;
    uint64_t rate = (s_ai_cr & 2) ? 48000 : 32000;
    return s_ai_base_count + (uint32_t)((host_time_ticks() - s_ai_start) * rate / TB_FREQ);
}

static int ai_read(uint32_t off, uint32_t *out) {
    switch (off) {
    case 0x6C00: {
        uint32_t cnt = ai_count();
        if ((s_ai_cr & 1) && s_ai_it && cnt >= s_ai_it) s_ai_cr |= 8;
        *out = s_ai_cr; return 1;
    }
    case 0x6C04: *out = s_ai_vr; return 1;
    case 0x6C08: *out = ai_count(); return 1;
    case 0x6C0C: *out = s_ai_it; return 1;
    }
    return 0;
}

static int ai_write(uint32_t off, uint32_t v) {
    switch (off) {
    case 0x6C00: {
        uint32_t cnt = ai_count();
        if (v & 0x20) cnt = 0;                       // SCRESET
        s_ai_base_count = cnt;
        s_ai_start = host_time_ticks();
        uint32_t ack = v & 8;
        s_ai_cr = (v & ~0x28u) | (s_ai_cr & 8 & ~ack);
        return 1;
    }
    case 0x6C04: s_ai_vr = v; return 1;
    case 0x6C08: return 1;
    case 0x6C0C: s_ai_it = v; return 1;
    }
    return 0;
}

// ============================================================================
// PI (0xCC003000): interrupt cause (0x3000) and mask (0x3004).
// Causes are level-triggered from each device's own status registers.
// ============================================================================
static uint32_t pi_cause(void) {
    uint32_t cause = 0;
    uint16_t csr = dsp[0x0A / 2];
    if (((csr & 0x08) && (csr & 0x10)) || ((csr & 0x20) && (csr & 0x40)) || ((csr & 0x80) && (csr & 0x100)))
        cause |= 0x40;  // DSP
    for (int ch = 0; ch < 3; ch++) {
        uint32_t c = exi[ch][EXI_CSR];
        if (((c & 2) && (c & 1)) || ((c & 8) && (c & 4)) || ((c & 0x800) && (c & 0x400))) cause |= 0x10;  // EXI
    }
    if (s_vi_status) cause |= 0x100;  // VI
    if (di_irq_pending()) cause |= 0x04;  // DI
    if ((s_ai_cr & 8) && (s_ai_cr & 4)) cause |= 0x20;  // AI
    if ((s_pe_ctrl & 4) && (s_pe_ctrl & 1)) cause |= 0x200;  // PE token
    if ((s_pe_ctrl & 8) && (s_pe_ctrl & 2)) cause |= 0x400;  // PE finish
    return cause;
}

int hw_pi_pending(void) { return (pi_cause() & shadow_read(0x3004, 4)) != 0; }

void hw_update(void) { vi_update(); aid_update(); }

// AICR bit 6 (AIDFR) selects the DMA sample rate: 0 = 48 kHz, 1 = 32 kHz.
uint32_t ai_dsp_sample_rate(void) { return (s_ai_cr & 0x40) ? 32000 : 48000; }

static int pi_read(uint32_t off, int size, uint32_t *out) {
    if (off == 0x3000 && size == 4) { *out = pi_cause(); return 1; }
    return 0;
}

// ============================================================================
static int s_init;
static void hw_init(void) {
    sram_init();
    dsp_reset();
    s_aram = calloc(1, ARAM_SIZE);
    s_init = 1;
}

int hw_read(uint32_t off, int size, uint32_t *out) {
    if (!s_init) hw_init();
    if (off >= 0x6800 && off < 0x6800 + 0x3C && size == 4) return exi_read(off, out);
    if (off >= 0x5000 && off < 0x5040) return dsp_read(off, size, out);
    if (off >= 0x2000 && off < 0x2100) return vi_read(off, size, out);
    if (off >= 0x3000 && off < 0x3100) return pi_read(off, size, out);
    if (off >= 0x1000 && off < 0x1100) return pe_read(off, size, out);
    if (off == 0x0000 && size == 2) { *out = (shadow_read(0, 2) & ~0x1Fu) | 0x0C; return 1; }  // CP status: GP idle
    if (off >= 0x6000 && off < 0x6028 && size == 4) return di_read(off, out);
    if (off >= 0x6C00 && off < 0x6C10 && size == 4) return ai_read(off, out);
    return 0;
}

int hw_write(uint32_t off, uint32_t v, int size) {
    if (!s_init) hw_init();
    if (off >= 0x6800 && off < 0x6800 + 0x3C && size == 4) return exi_write(off, v);
    if (off >= 0x5000 && off < 0x5040) return dsp_write(off, v, size);
    if (off >= 0x2000 && off < 0x2100) return vi_write(off, v, size);
    if (off >= 0x1000 && off < 0x1100) return pe_write(off, v, size);
    if (off >= 0x6000 && off < 0x6028 && size == 4) return di_write(off, v);
    if (off >= 0x6C00 && off < 0x6C10 && size == 4) return ai_write(off, v);
    if (off >= 0x8000 && off < 0x9000) { gx_fifo_write(v, size); return 1; }
    if (off < 0x80) { shadow_write(off, v, size); gx_cp_reg_written(off & ~1u); return 1; }
    return 0;
}
