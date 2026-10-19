// High-level emulation of the AX audio microcode (DSP).
//
// Turok: Evolution uploads AX microcode 0x07F88145 (HashEctor of IRAM, the
// same build as F-Zero GX / Ikaruga). Protocol and voice processing follow the
// documented behaviour of that microcode (see Dolphin's DSPHLE AX for the
// reference this was written against):
//
//   CPU -> DSP: 0xBABE<size> then <command list address>      (size in u16s)
//   DSP:        runs the command list (mix 5 ms = 160 samples @ 32 kHz)
//   DSP -> CPU: DSP_YIELD (0xDCD10002) + interrupt
//   CPU -> DSP: 0xCDD1000x task mail (RESUME / NEW_UCODE / RESET / CONTINUE)
//
// Samples are read from ARAM through a simulated "accelerator" (ADPCM, PCM8,
// PCM16 with hardware looping) and resampled linearly (the polyphase filter
// needs coefficients from the DSP ROM, which we don't ship).
#include <stdio.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"

uint8_t *hw_aram(void);
void dsp_push_mail(uint32_t mail, int irq);

#define DSP_RESUME 0xDCD10001u
#define DSP_YIELD  0xDCD10002u
#define MAIL_CMDLIST 0xBABE0000u
#define MAIL_RESUME   0xCDD10000u
#define MAIL_NEW_UCODE 0xCDD10001u
#define MAIL_RESET    0xCDD10002u
#define MAIL_CONTINUE 0xCDD10003u

#define SAMPLES_PER_MS 32
#define FRAME_SAMPLES (5 * SAMPLES_PER_MS)

// ---- mixing buses ------------------------------------------------------------------
enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, NBUS };
static int s_bus[NBUS][FRAME_SAMPLES];

static uint32_t ram_addr(uint32_t a) { return a & 0x01FFFFFFu; }
static uint16_t ram16(uint32_t a) { const uint8_t *p = g_ram + ram_addr(a); return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t ram32(uint32_t a) { const uint8_t *p = g_ram + ram_addr(a); return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_ram16(uint32_t a, uint16_t v) { uint8_t *p = g_ram + ram_addr(a); p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put_ram32(uint32_t a, uint32_t v) { uint8_t *p = g_ram + ram_addr(a); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static int16_t clamp16(int64_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

// ---- parameter block (AXPB as an array of u16, GC layout with low-pass filter) -----
enum {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SELECT = 5, PB_MIXER_CONTROL = 6,
    PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIXER = 9,          // 9 x {volume, delta}: mainL, mainR, auxAL, auxAR, auxBL, auxBR, auxBS, mainS, auxAS
    PB_ITD_ON = 27,
    PB_UPD_NUM = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP = 41,          // mainL auxAL auxBL mainR auxAR auxBR mainS auxAS auxBS
    PB_VOL = 50, PB_VOL_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58, PB_END_HI = 59, PB_END_LO = 60,
    PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PRED = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    PB_LPF_ON = 93, PB_LPF_YN1 = 94, PB_LPF_A0 = 95, PB_LPF_B0 = 96,
    PB_LOOP_COUNTER = 97,
    PB_WORDS = 98
};

// mixer slot order in the PB -> bus, and the matching dpop slot
static const int MIXER_BUS[9] = {MAIN_L, MAIN_R, AUXA_L, AUXA_R, AUXB_L, AUXB_R, AUXB_S, MAIN_S, AUXA_S};
static const int DPOP_SLOT[NBUS] = {0, 3, 6, 1, 4, 7, 2, 5, 8};  // bus -> dpop index

// mixer_control (newer GC microcodes) -> per-bus enable / ramp
static void mixer_flags(uint16_t mc, int on[NBUS], int ramp[NBUS]) {
    memset(on, 0, sizeof(int) * NBUS); memset(ramp, 0, sizeof(int) * NBUS);
    on[MAIN_L] = mc & 0x0001; on[MAIN_R] = (mc & 0x0002) != 0; on[MAIN_S] = (mc & 0x0004) != 0;
    if (mc & 0x0008) ramp[MAIN_L] = ramp[MAIN_R] = ramp[MAIN_S] = 1;
    on[AUXA_L] = (mc & 0x0010) != 0; on[AUXA_R] = (mc & 0x0020) != 0;
    if (mc & 0x0040) ramp[AUXA_L] = ramp[AUXA_R] = 1;
    on[AUXA_S] = (mc & 0x0080) != 0; ramp[AUXA_S] = (mc & 0x0100) != 0;
    on[AUXB_L] = (mc & 0x0200) != 0; on[AUXB_R] = (mc & 0x0400) != 0;
    if (mc & 0x0800) ramp[AUXB_L] = ramp[AUXB_R] = 1;
    on[AUXB_S] = (mc & 0x1000) != 0; ramp[AUXB_S] = (mc & 0x2000) != 0;
}

// ---- accelerator --------------------------------------------------------------------
typedef struct {
    uint32_t start, end, cur;
    uint16_t format;
    int16_t yn1, yn2;
    uint16_t pred_scale;
    int16_t gain;
    int stopped;
    uint16_t *pb;
} Acc;

static uint8_t aram_byte(uint32_t a) { return hw_aram()[a & 0x00FFFFFFu]; }

static uint16_t acc_raw(Acc *a) {
    switch (a->format & 3) {
    case 0: { uint8_t b = aram_byte(a->cur >> 1); return (a->cur & 1) ? (b & 0xF) : (b >> 4); }
    case 1: return aram_byte(a->cur);
    case 2: return (uint16_t)(aram_byte(a->cur * 2) << 8 | aram_byte(a->cur * 2 + 1));
    default: return 0;
    }
}

static void acc_end_exception(Acc *a) {
    uint16_t *pb = a->pb;
    if (pb[PB_LOOPING]) {
        a->pred_scale = pb[PB_LOOP_PRED] & 0x7F;
        if (pb[PB_IS_STREAM] != 1) {
            a->yn1 = (int16_t)pb[PB_LOOP_YN1];
            a->yn2 = (int16_t)pb[PB_LOOP_YN2];
        } else {
            pb[PB_LOOP_COUNTER]++;
        }
        a->stopped = 0;
    } else {
        pb[PB_RUNNING] = 0;
    }
}

static int16_t acc_sample(Acc *a) {
    if (a->stopped) return 0;
    const int16_t *coefs = (const int16_t *)&a->pb[PB_COEFS];
    int ci = (a->pred_scale >> 4) & 7;
    int32_t c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
    int decode = (a->format >> 2) & 3;
    int16_t raw = (int16_t)acc_raw(a);
    int16_t val = 0;
    int step = 0;
    if (decode == 0) {  // ADPCM
        int s = raw & 0xF;
        if (s >= 8) s -= 16;
        int32_t v = (1 << (a->pred_scale & 0xF)) * s + ((0x400 + c1 * a->yn1 + c2 * a->yn2) >> 11);
        val = clamp16(v);
        step = 2;
        a->yn2 = a->yn1; a->yn1 = val;
        a->cur++;
        if ((a->end & 0xF) == 0 && a->cur == a->end) a->cur = a->start + 1;
        else if ((a->end & 0xF) == 1 && a->cur == a->end - 1) a->cur = a->start;
        else if ((a->cur & 15) == 0) {
            a->pred_scale = aram_byte((a->cur & ~15u) >> 1) & 0x7F;
            a->cur += 2;
            step += 2;
        }
    } else {  // PCM
        static const int shifts[4] = {11, 0, 16, 0};
        int sh = shifts[(a->format >> 4) & 3];
        int32_t v = ((int32_t)a->gain * raw >> sh) + ((c1 * a->yn1) >> sh) + ((c2 * a->yn2) >> sh);
        val = (int16_t)v;
        a->yn2 = a->yn1; a->yn1 = val;
        step = 2;
        if (decode != 1) a->cur++;
    }
    if (a->cur == a->end + step - 1) {
        a->cur = a->start;
        a->stopped = 1;
        acc_end_exception(a);
    }
    a->cur &= 0xBFFFFFFFu;
    return val;
}

// ---- one voice, one millisecond -----------------------------------------------------------
static void process_voice_ms(uint16_t *pb, int offset) {
    if (pb[PB_RUNNING] != 1) return;

    Acc a;
    a.start = ((uint32_t)pb[PB_LOOP_HI] << 16 | pb[PB_LOOP_LO]) & 0x3FFFFFFFu;
    a.end = ((uint32_t)pb[PB_END_HI] << 16 | pb[PB_END_LO]) & 0x3FFFFFFFu;
    a.cur = ((uint32_t)pb[PB_CUR_HI] << 16 | pb[PB_CUR_LO]) & 0xBFFFFFFFu;
    a.format = pb[PB_FORMAT];
    a.yn1 = (int16_t)pb[PB_YN1]; a.yn2 = (int16_t)pb[PB_YN2];
    a.gain = (int16_t)pb[PB_GAIN];
    a.pred_scale = pb[PB_PRED_SCALE] & 0x7F;
    a.stopped = 0;
    a.pb = pb;

    // linear sample-rate conversion (polyphase falls back to linear, as without DSP ROM coefficients)
    int16_t out[SAMPLES_PER_MS];
    int16_t *last = (int16_t *)&pb[PB_LAST];
    uint32_t ratio = (uint32_t)pb[PB_RATIO_HI] << 16 | pb[PB_RATIO_LO];
    uint32_t pos = pb[PB_FRAC];
    if (pb[PB_SRC_TYPE] == 2) {  // nearest: no conversion
        for (int i = 0; i < SAMPLES_PER_MS; i++) out[i] = acc_sample(&a);
        memcpy(last, out + SAMPLES_PER_MS - 4, 8);
    } else {
        int16_t t[4] = {last[0], last[1], last[2], last[3]};
        uint32_t idx = 4;
        for (int i = 0; i < SAMPLES_PER_MS; i++) {
            pos += ratio;
            while (pos >= 0x10000) { t[idx++ & 3] = acc_sample(&a); pos -= 0x10000; }
            uint16_t frac = (uint16_t)pos, inv = (uint16_t)-frac;
            if (frac) {
                int32_t s0 = t[idx & 3], s1 = t[(idx + 1) & 3];
                out[i] = (int16_t)((s0 * inv + s1 * frac) >> 16);
            } else {
                out[i] = t[idx & 3];
            }
        }
        for (int k = 0; k < 4; k++) last[k] = t[(idx + k) & 3];
    }
    pb[PB_FRAC] = (uint16_t)pos;
    pb[PB_CUR_HI] = (uint16_t)(a.cur >> 16); pb[PB_CUR_LO] = (uint16_t)a.cur;
    pb[PB_YN1] = (uint16_t)a.yn1; pb[PB_YN2] = (uint16_t)a.yn2;
    pb[PB_PRED_SCALE] = a.pred_scale;

    // volume envelope (signed on GameCube)
    for (int i = 0; i < SAMPLES_PER_MS; i++) {
        int32_t vol = (int16_t)pb[PB_VOL];
        out[i] = clamp16(((int32_t)out[i] * vol) >> 15);
        pb[PB_VOL] = (uint16_t)(pb[PB_VOL] + pb[PB_VOL_DELTA]);
    }
    // low-pass filter
    if (pb[PB_LPF_ON]) {
        int16_t yn1 = (int16_t)pb[PB_LPF_YN1];
        int32_t a0 = pb[PB_LPF_A0], b0 = (int16_t)pb[PB_LPF_B0];
        for (int i = 0; i < SAMPLES_PER_MS; i++) yn1 = out[i] = clamp16((a0 * out[i] + b0 * yn1) >> 15);
        pb[PB_LPF_YN1] = (uint16_t)yn1;
    }
    // mix
    int on[NBUS], ramp[NBUS];
    mixer_flags(pb[PB_MIXER_CONTROL], on, ramp);
    for (int m = 0; m < 9; m++) {
        int bus = MIXER_BUS[m];
        if (!on[bus]) continue;
        uint16_t vol = pb[PB_MIXER + 2 * m], delta = ramp[bus] ? pb[PB_MIXER + 2 * m + 1] : 0;
        int16_t s16 = 0;
        for (int i = 0; i < SAMPLES_PER_MS; i++) {
            s16 = clamp16(((int64_t)out[i] * vol) >> 15);
            s_bus[bus][offset + i] += s16;
            vol = (uint16_t)(vol + delta);
        }
        pb[PB_MIXER + 2 * m] = vol;
        pb[PB_DPOP + DPOP_SLOT[bus]] = (uint16_t)s16;
    }
}

static void process_pb_list(uint32_t pb_addr) {
    uint16_t pb[PB_WORDS];
    for (int guard = 0; pb_addr && guard < 512; guard++) {
        for (int i = 0; i < PB_WORDS; i++) pb[i] = ram16(pb_addr + 2 * i);
        // per-millisecond parameter updates
        uint32_t upd = (uint32_t)pb[PB_UPD_HI] << 16 | pb[PB_UPD_LO];
        uint16_t nupd[5];
        memcpy(nupd, &pb[PB_UPD_NUM], sizeof nupd);
        uint32_t start = 0;
        for (int ms = 0; ms < 5; ms++) {
            uint32_t n = nupd[ms];
            if (upd && start + n <= 32) {
                for (uint32_t k = start; k < start + n; k++) {
                    uint16_t off = ram16(upd + 4 * k), val = ram16(upd + 4 * k + 2);
                    if (off < PB_WORDS) pb[off] = val;
                }
            }
            start += n;
            process_voice_ms(pb, ms * SAMPLES_PER_MS);
        }
        for (int i = 0; i < PB_WORDS; i++) put_ram16(pb_addr + 2 * i, pb[i]);
        pb_addr = (uint32_t)pb[PB_NEXT_HI] << 16 | pb[PB_NEXT_LO];
    }
}

// ---- command list -------------------------------------------------------------------------
static void setup_processing(uint32_t a) {
    for (int b = 0; b < NBUS; b++) {
        int32_t value = (int32_t)((uint32_t)ram16(a + 6 * b) << 16 | ram16(a + 6 * b + 2));
        int16_t delta = (int16_t)ram16(a + 6 * b + 4);
        for (int j = 0; j < FRAME_SAMPLES; j++) s_bus[b][j] = value ? value + j * delta : 0;
    }
}

static void upload(uint32_t a, int bus) {
    for (int j = 0; j < FRAME_SAMPLES; j++) put_ram32(a + 4 * j, (uint32_t)s_bus[bus][j]);
}
static void add_from(uint32_t a, int bus) {
    for (int j = 0; j < FRAME_SAMPLES; j++) s_bus[bus][j] += (int32_t)ram32(a + 4 * j);
}

static void mix_aux(int aux, uint32_t write_addr, uint32_t read_addr) {
    int base = aux ? AUXB_L : AUXA_L;
    if (write_addr)
        for (int k = 0; k < 3; k++) upload(write_addr + k * FRAME_SAMPLES * 4, base + k);
    add_from(read_addr, MAIN_L);
    add_from(read_addr + FRAME_SAMPLES * 4, MAIN_R);
    add_from(read_addr + 2 * FRAME_SAMPLES * 4, MAIN_S);
}

static void output_samples(uint32_t lr_addr, uint32_t surround_addr) {
    upload(surround_addr, MAIN_S);
    for (int j = 0; j < FRAME_SAMPLES; j++) {  // interleaved R, L
        put_ram16(lr_addr + 4 * j, (uint16_t)clamp16(s_bus[MAIN_R][j]));
        put_ram16(lr_addr + 4 * j + 2, (uint16_t)clamp16(s_bus[MAIN_L][j]));
    }
}

static uint16_t s_cmd[0x1000];
static int s_compressor_pos;

static void copy_cmdlist(uint32_t addr, uint32_t words) {
    if (words > 0x1000) { logf_("[ax] command list too large (%u)", words); words = 0x1000; }
    for (uint32_t i = 0; i < words; i++) s_cmd[i] = ram16(addr + 2 * i);
}

#define A32(i) ((uint32_t)s_cmd[i] << 16 | s_cmd[(i) + 1])

static void run_cmdlist(void) {
    uint32_t pb_addr = 0, i = 0;
    for (int guard = 0; guard < 4096; guard++) {
        uint16_t cmd = s_cmd[i++];
        switch (cmd) {
        case 0x00: setup_processing(A32(i)); i += 2; break;
        case 0x01: {  // download + mix with volume
            uint32_t a = A32(i);
            uint16_t vol[3] = {s_cmd[i + 2], s_cmd[i + 3], s_cmd[i + 4]};
            i += 5;
            for (int g = 0; g < 3; g++)
                for (int k = 0; k < 3; k++) {
                    int bus = g * 3 + k;
                    for (int j = 0; j < FRAME_SAMPLES; j++, a += 4)
                        s_bus[bus][j] += (int32_t)(((int64_t)(int32_t)ram32(a) * vol[g]) >> 15);
                }
            break;
        }
        case 0x02: pb_addr = A32(i); i += 2; break;
        case 0x03: process_pb_list(pb_addr); break;
        case 0x04: case 0x05: mix_aux(cmd - 4, A32(i), A32(i + 2)); i += 4; break;
        case 0x06: {  // upload L, R, S
            uint32_t a = A32(i); i += 2;
            for (int k = 0; k < 3; k++) upload(a + k * FRAME_SAMPLES * 4, MAIN_L + k);
            break;
        }
        case 0x07: {  // set main L/R from one buffer
            uint32_t a = A32(i); i += 2;
            for (int j = 0; j < FRAME_SAMPLES; j++) {
                int32_t s = (int32_t)ram32(a + 4 * j);
                s_bus[MAIN_L][j] = s; s_bus[MAIN_R][j] = s; s_bus[MAIN_S][j] = 0;
            }
            break;
        }
        case 0x08: i += 10; break;
        case 0x09: mix_aux(1, 0, A32(i)); i += 2; break;
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: {  // more: continue with another list
            uint32_t a = A32(i); uint16_t n = s_cmd[i + 2];
            copy_cmdlist(a, n); i = 0;
            break;
        }
        case 0x0E: output_samples(A32(i + 2), A32(i)); i += 4; break;
        case 0x0F: return;
        case 0x10: {  // upload AUXB L/R, then replace them and mix into main
            uint32_t ul = A32(i), dl = A32(i + 2); i += 4;
            upload(ul, AUXB_L); upload(ul + FRAME_SAMPLES * 4, AUXB_R);
            for (int j = 0; j < FRAME_SAMPLES; j++) {
                int32_t l = (int32_t)ram32(dl + 4 * j), r = (int32_t)ram32(dl + FRAME_SAMPLES * 4 + 4 * j);
                s_bus[AUXB_L][j] = l; s_bus[MAIN_L][j] += l;
                s_bus[AUXB_R][j] = r; s_bus[MAIN_R][j] += r;
            }
            break;
        }
        case 0x11: {  // opposite L/R
            uint32_t a = A32(i); i += 2;
            for (int j = 0; j < FRAME_SAMPLES; j++) {
                int32_t s = (int32_t)ram32(a + 4 * j);
                s_bus[MAIN_L][j] = -s; s_bus[MAIN_R][j] = s; s_bus[MAIN_S][j] = 0;
            }
            break;
        }
        case 0x12: {  // compressor
            uint16_t threshold = s_cmd[i], frames = s_cmd[i + 1];
            uint32_t table = A32(i + 2);
            i += 4;
            int hit = 0;
            for (int j = 0; j < FRAME_SAMPLES && !hit; j++)
                hit = abs(s_bus[MAIN_L][j]) > (int)threshold || abs(s_bus[MAIN_R][j]) > (int)threshold;
            uint32_t frame_bytes = FRAME_SAMPLES * 2, off;
            if (hit) { off = (uint32_t)s_compressor_pos * frame_bytes; s_compressor_pos = frames; }
            else if (s_compressor_pos) { s_compressor_pos--; off = (11 + (uint32_t)s_compressor_pos) * frame_bytes; }
            else break;
            for (int j = 0; j < FRAME_SAMPLES; j++) {
                uint16_t coef = ram16(table + off + 2 * j);
                s_bus[MAIN_L][j] = (int32_t)(((int64_t)s_bus[MAIN_L][j] * coef) >> 15);
                s_bus[MAIN_R][j] = (int32_t)(((int64_t)s_bus[MAIN_R][j] * coef) >> 15);
            }
            break;
        }
        case 0x13: {  // send AUX and mix
            uint32_t auxa_up = A32(i), auxb_s_up = A32(i + 2);
            uint32_t dl[4] = {A32(i + 4), A32(i + 6), A32(i + 8), A32(i + 10)};
            i += 12;
            for (int k = 0; k < 3; k++) upload(auxa_up + k * FRAME_SAMPLES * 4, AUXA_L + k);
            upload(auxb_s_up, AUXB_S);
            add_from(dl[0], MAIN_L); add_from(dl[1], MAIN_R); add_from(dl[2], AUXB_L); add_from(dl[3], AUXB_R);
            break;
        }
        default:
            logf_("[ax] unknown command %04X; ending list", cmd);
            return;
        }
    }
}

// ---- mail handling -----------------------------------------------------------------------
enum { WAIT_SIZE, WAIT_ADDR, WAIT_TASK };
static int s_state = WAIT_SIZE;
static uint16_t s_cmd_words;

void ax_mail(uint32_t mail) {
    switch (s_state) {
    case WAIT_SIZE:
        if ((mail & 0xFFFF0000u) == MAIL_CMDLIST) { s_cmd_words = (uint16_t)mail; s_state = WAIT_ADDR; }
        else logf_("[ax] unexpected mail %08X (expected command list)", mail);
        break;
    case WAIT_ADDR:
        copy_cmdlist(mail & 0x7FFFFFFFu, s_cmd_words);
        run_cmdlist();
        dsp_push_mail(DSP_YIELD, 1);
        s_state = WAIT_TASK;
        break;
    case WAIT_TASK:
        mail = 0xCDD10000u | (mail & 0xFFFF);
        if (mail == MAIL_RESUME) { dsp_push_mail(DSP_RESUME, 1); s_state = WAIT_SIZE; }
        else if (mail == MAIL_CONTINUE || mail == MAIL_NEW_UCODE) s_state = WAIT_SIZE;
        else if (mail == MAIL_RESET) { logf_("[ax] reset requested"); s_state = WAIT_SIZE; }
        else logf_("[ax] unknown task mail %08X", mail);
        break;
    }
}
