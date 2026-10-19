// Console runner for turok_core.dll: starts the game headless and reports
// state / frame statistics. Useful for debugging without the MonoGame host.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "core_api.h"

int main(int argc, char **argv) {
    const char *iso = argc > 1 ? argv[1] : "Turok - Evolution NTSC-U.iso";
    int seconds = argc > 2 ? atoi(argv[2]) : 15;
    if (!core_start(iso, "turok_recomp.log")) {
        char msg[512]; core_state(msg, sizeof msg);
        fprintf(stderr, "start failed: %s\n", msg);
        return 1;
    }
    unsigned frames = 0, last_batches = 0, last_verts = 0, last_tex = 0;
    FILE *wav = fopen("turok_audio.wav", "wb");
    unsigned audio_frames = 0; int peak = 0;
    static int16_t abuf[48000 * 2];
    if (wav) fwrite(abuf, 1, 44, wav);  // header placeholder
    for (int i = 0; i < seconds * 10; i++) {
        Sleep(100);
        int n = core_read_audio(abuf, 48000);
        for (int k = 0; k < 2 * n; k++) { int v = abuf[k] < 0 ? -abuf[k] : abuf[k]; if (v > peak) peak = v; }
        if (wav && n) fwrite(abuf, 4, n, wav);
        audio_frames += n;
        const CoreFrame *f = core_acquire_frame();
        if (f) {
            frames++;
            last_batches = f->batch_count; last_verts = f->vertex_count; last_tex = f->texture_count;
            core_release_frame();
        }
        char msg[512];
        int st = core_state(msg, sizeof msg);
        if (st == CORE_FAILED) { fprintf(stderr, "core failed: %s\n", msg); return 1; }
        if (st == CORE_EXITED) break;
    }
    if (wav) {  // finish the WAV header (PCM16 stereo)
        uint32_t rate = (uint32_t)core_audio_rate(), data = audio_frames * 4, riff = 36 + data;
        uint32_t byte_rate = rate * 4; uint16_t fmt[] = {1, 2}; uint16_t align[] = {4, 16};
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        uint32_t sixteen = 16; fwrite(&sixteen, 4, 1, wav); fwrite(fmt, 2, 2, wav); fwrite(&rate, 4, 1, wav);
        fwrite(&byte_rate, 4, 1, wav); fwrite(align, 2, 2, wav); fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
        fclose(wav);
    }
    printf("audio: %u frames (%.1f s at %d Hz), peak %d\n", audio_frames, audio_frames / (double)core_audio_rate(), core_audio_rate(), peak);
    printf("frames seen: %u (last: %u batches, %u vertices, %u textures)\n", frames, last_batches, last_verts, last_tex);
    return 0;
}
