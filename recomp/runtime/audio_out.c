// Audio output: PCM played by the AI DMA is queued here for the host.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ppc.h"
#include "host.h"
#include "core_api.h"

#define RING_FRAMES (48000 * 2)  // ~2 s
static int16_t s_ring[RING_FRAMES * 2];
static volatile LONG s_read, s_write;  // frame indices, single producer / consumer
static volatile LONG s_rate = 32000;

// pcm: big-endian 16-bit stereo, right channel first (as Dolphin's mixer reads it)
void audio_push_be_stereo(const uint8_t *pcm, uint32_t bytes, uint32_t rate) {
    s_rate = (LONG)rate;
    uint32_t frames = bytes / 4;
    LONG w = s_write;
    for (uint32_t i = 0; i < frames; i++) {
        LONG next = (w + 1) % RING_FRAMES;
        if (next == s_read) break;  // host not draining: drop
        const uint8_t *f = pcm + 4 * i;
        int16_t r = (int16_t)(f[0] << 8 | f[1]), l = (int16_t)(f[2] << 8 | f[3]);
        s_ring[2 * w] = l; s_ring[2 * w + 1] = r;
        w = next;
    }
    InterlockedExchange(&s_write, w);
}

CORE_API int core_audio_rate(void) { return s_rate; }

// Copy up to max_frames stereo frames (interleaved L, R, host endian) into dst.
CORE_API int core_read_audio(int16_t *dst, int max_frames) {
    LONG r = s_read, w = s_write;
    int n = 0;
    while (r != w && n < max_frames) {
        dst[2 * n] = s_ring[2 * r]; dst[2 * n + 1] = s_ring[2 * r + 1];
        r = (r + 1) % RING_FRAMES; n++;
    }
    InterlockedExchange(&s_read, r);
    return n;
}
