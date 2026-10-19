// Offline test of dsp_ax.c voice processing: SRC (polyphase / linear) and ITD.
// Includes the runtime's dsp_ax.c directly and drives process_voice_ms with a
// PCM16 log sine sweep in a fake ARAM. Writes WAVs for each SRC case and checks
// that an ITD voice's right channel is the left one delayed by the target shift.
//
//   (VS x64 prompt) cl /O2 /I recompuntime toolsxtestxtest.c /Fe:axtest.exe
//   axtest <out dir>
//   python tools/axtest/srcplot.py <out dir> <out dir>/src_test.png
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
uint8_t *g_ram;
static uint8_t s_aram[16 << 20];
uint8_t *hw_aram(void) { return s_aram; }
void dsp_push_mail(uint32_t m, int irq) { (void)m; (void)irq; }
void logf_(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
#include "dsp_ax.c"

static void wav(const char *path, const int16_t *lr, int frames) {
    FILE *f = fopen(path, "wb");
    uint32_t rate = 32000, data = frames * 4, riff = 36 + data, br = rate * 4, sixteen = 16;
    uint16_t fmt[] = {1, 2}, al[] = {4, 16};
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f);
    fwrite(fmt, 2, 2, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(al, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(lr, 4, frames, f); fclose(f);
}

#define NSRC (32000 * 4)
// render one voice: PCM16 source in ARAM, returns L/R interleaved
static void render(uint16_t src_type, uint16_t coef, uint32_t ratio, int itd_r, int16_t *lr, int ms) {
    uint16_t pb[PB_WORDS] = {0};
    pb[PB_RUNNING] = 1; pb[PB_SRC_TYPE] = src_type; pb[PB_COEF_SELECT] = coef;
    pb[PB_MIXER_CONTROL] = 0x0003;                      // main L + R, no ramp
    pb[PB_MIXER + 0] = 0x8000; pb[PB_MIXER + 2] = 0x8000;
    pb[PB_VOL] = 0x7FFF;
    pb[PB_LOOPING] = 1; pb[PB_FORMAT] = 0x0A;           // PCM16
    pb[PB_GAIN] = 0x0800;
    pb[PB_LOOP_HI] = 0; pb[PB_LOOP_LO] = 0;
    pb[PB_END_HI] = (NSRC - 1) >> 16; pb[PB_END_LO] = (uint16_t)(NSRC - 1);
    pb[PB_RATIO_HI] = (uint16_t)(ratio >> 16); pb[PB_RATIO_LO] = (uint16_t)ratio;
    if (itd_r >= 0) {
        pb[PB_ITD_ON] = 1; pb[PB_ITD_BUF_HI] = 0x8000; pb[PB_ITD_BUF_LO] = 0x4000;
        pb[PB_ITD_TARGET_L] = 0; pb[PB_ITD_TARGET_R] = (uint16_t)itd_r;
    }
    for (int m = 0; m < ms; m++) {
        memset(s_bus, 0, sizeof s_bus);
        process_voice_ms(pb, 0);
        for (int i = 0; i < SAMPLES_PER_MS; i++) {
            lr[2 * (m * 32 + i)] = clamp16(s_bus[MAIN_L][i]);
            lr[2 * (m * 32 + i) + 1] = clamp16(s_bus[MAIN_R][i]);
        }
    }
}

int main(int argc, char **argv) {
    g_ram = calloc(1, 0x01800000);
    init_coefs();
    char p[512];
    // source: log sine sweep 50 Hz -> 15.9 kHz (relative to a 32 kHz source) over 4 s, amplitude 0.5
    double ph = 0;
    for (int n = 0; n < NSRC; n++) {
        double t = (double)n / NSRC, f = 50 * pow(15900.0 / 50, t);
        ph += 2 * 3.14159265358979 * f / 32000;
        int16_t v = (int16_t)(16383 * sin(ph));
        s_aram[2 * n] = (uint8_t)(v >> 8); s_aram[2 * n + 1] = (uint8_t)v;
    }
    int16_t *lr = malloc(8000 * 32 * 4);
    struct { const char *name; uint16_t src, coef; uint32_t ratio; } cases[] = {
        {"up_linear", 1, 0, 0x8000}, {"up_poly0", 0, 0, 0x8000}, {"up_poly2", 0, 2, 0x8000},
        {"down_linear", 1, 0, 0x18000}, {"down_poly0", 0, 0, 0x18000}, {"down_poly1", 0, 1, 0x18000},
    };
    for (int c = 0; c < 6; c++) {
        int ms = cases[c].ratio < 0x10000 ? 8000 : 2600;  // one pass over the 4 s source
        render(cases[c].src, cases[c].coef, cases[c].ratio, -1, lr, ms);
        snprintf(p, sizeof p, "%s/%s.wav", argv[1], cases[c].name); wav(p, lr, ms * 32);
    }
    // ITD: right ear 20 samples late; the shift should ramp 0 -> 20 over 20 ms and then hold
    render(1, 0, 0x10000, 20, lr, 200);
    int best = 0; double bc = -1;
    for (int lag = -31; lag <= 31; lag++) {
        double cc = 0;
        for (int i = 3200; i < 6000; i++) cc += (double)lr[2 * i] * lr[2 * (i + lag) + 1];
        if (cc > bc) { bc = cc; best = lag; }
    }
    printf("ITD: right channel lags left by %d samples (target 20)\n", best);
    int same = 1;
    for (int i = 32 * 30; i < 32 * 200 - 20; i++) same &= lr[2 * i] == lr[2 * (i + 20) + 1];
    printf("ITD: right == left delayed by 20 after ramp: %s\n", same ? "yes" : "no");
    snprintf(p, sizeof p, "%s/itd20.wav", argv[1]); wav(p, lr, 200 * 32);
    // coefficient sanity: phase sums (DC gain) and the first rows
    for (int t = 0; t < 3; t++) {
        int mn = 1 << 30, mx = 0;
        for (int ph2 = 0; ph2 < 128; ph2++) {
            int s = 0; for (int k = 0; k < 4; k++) s += s_coefs[t][ph2 * 4 + k];
            if (s < mn) mn = s; if (s > mx) mx = s;
        }
        printf("table %d: phase sums %d..%d; phase 0 = %d %d %d %d, phase 64 = %d %d %d %d\n", t, mn, mx,
               s_coefs[t][0], s_coefs[t][1], s_coefs[t][2], s_coefs[t][3],
               s_coefs[t][256], s_coefs[t][257], s_coefs[t][258], s_coefs[t][259]);
    }
    return 0;
}
