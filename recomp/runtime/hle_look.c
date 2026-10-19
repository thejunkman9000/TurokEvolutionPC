// Mouse look.
//
// The game turns the view from the C-stick through a dead zone, a steep response
// curve, a ~5-frame ramp and a top speed of about 180 deg/s (yaw) / 120 deg/s
// (pitch), with 2-3 frames of latency, so mapping mouse motion onto the stick feels
// laggy and slow. Instead:
//
//  * Yaw is written directly: *(0x803113AC) is the player's view actor and +0xF0 its
//    yaw in radians (the game derives +0xC4 and the camera from it).
//  * Pitch has no simple state to write (it ends up as a bone rotation in the
//    player's skeleton), so it is driven through the game's own C-stick Y with a
//    closed loop: the mouse moves a target pitch, and each frame the raw stick value
//    is chosen from the measured response curve to close the gap, predicting the
//    motion still in flight.
//
// Everything is applied once per game frame at the C-stick poll (GetAxisX(port 0,
// 407), fn_80007734), and only while the player's view actor drives the rendered
// camera, so cutscenes and other cameras are left alone. How these were found:
// analysis/NOTES.md ("Mouse look").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <math.h>
#include "ppc.h"
#include "host.h"
#include "hw.h"
#include "core_api.h"
#include "../generated/funcs.h"

#define PLAYER_VIEW_PTR 0x803113ACu  // view actor; +0xF0 yaw (radians)
#define VIEW_YAW 0xF0u
#define CSTICK_ID 407
#define PI 3.14159265f
#define TWO_PI 6.28318531f
#define PITCH_LIMIT 1.20f            // the game stops at about +-1.22 rad

extern int g_cy_override;  // pad.c: raw C-stick Y forced when in [-128, 127]
extern int g_pad_cy;       // pad.c: raw C-stick Y from the host's controller

static SRWLOCK s_lock = SRWLOCK_INIT;
static float s_dyaw, s_dpitch;

CORE_API void core_mouse_look(float dyaw, float dpitch) {
    AcquireSRWLockExclusive(&s_lock);
    s_dyaw += dyaw;
    s_dpitch += dpitch;
    ReleaseSRWLockExclusive(&s_lock);
}

static float wrap_pi(float a) {
    a = fmodf(a + PI, TWO_PI);
    if (a < 0) a += TWO_PI;
    return a - PI;
}

// Rendered camera angles, from the forward vector the renderer caches at 0x802E22F8.
static float camera_yaw(void) { return atan2f(u2f(rd32(0x802E22F8)), u2f(rd32(0x802E2300))); }
static float camera_pitch(void) {
    float x = u2f(rd32(0x802E22F8)), y = u2f(rd32(0x802E22FC)), z = u2f(rd32(0x802E2300));
    return atan2f(y, sqrtf(x * x + z * z));
}

// The view actor while it drives the rendered camera (first-person control), else 0.
static uint32_t player_view(void) {
    uint32_t v = rd32(PLAYER_VIEW_PTR);
    if (v < 0x80003000u || v >= 0x81800000u - 0x100u) return 0;
    float yaw = u2f(rd32(v + VIEW_YAW));
    if (!(fabsf(yaw) < 100.f)) return 0;
    if (fabsf(wrap_pi(yaw - camera_yaw() - PI)) > 0.15f) return 0;  // actor yaw = camera yaw + pi
    return v;
}

// ---- pitch: closed loop through the C-stick ----------------------------------------
// Measured steady-state pitch rate (rad/frame) for raw C-stick Y (positive = down).
// Below 36 is the dead zone; past ~62 the game switches to its fast look speed.
static const struct { int raw; float rate; } k_curve[] = {
    {35, 0.0f}, {36, 0.00122f}, {38, 0.00168f}, {42, 0.00261f}, {46, 0.00353f}, {50, 0.00535f},
    {54, 0.00650f}, {58, 0.00759f}, {61, 0.00841f}, {64, 0.0279f}, {67, 0.0333f}, {70, 0.0342f},
};
#define N_CURVE (int)(sizeof k_curve / sizeof k_curve[0])
#define SLOW_MAX 0.00841f
#define FAST_MIN 0.0279f
#define FAST_MAX 0.0342f
#define LATENCY 3

static float s_target;              // pitch the mouse asks for
static int s_tracking;              // the loop is steering (target differs from camera)
static float s_inflight[LATENCY];   // rates commanded in the last frames, not yet visible
static float s_dither;

// Raw stick magnitude for a wanted rate magnitude (and the rate it will actually give).
static int raw_for_rate(float r, float *got) {
    if (r < k_curve[1].rate * 0.5f) { *got = 0; return 0; }
    if (r >= FAST_MAX) { *got = FAST_MAX; return 70; }
    if (r > SLOW_MAX && r < FAST_MIN) {
        // Between the two look speeds: alternate slow/fast frames to average r.
        s_dither += (r - SLOW_MAX) / (FAST_MIN - SLOW_MAX);
        if (s_dither >= 1.0f) { s_dither -= 1.0f; *got = FAST_MIN; return 64; }
        *got = SLOW_MAX;
        return 61;
    }
    for (int i = 1; i < N_CURVE; i++)
        if (r <= k_curve[i].rate) {
            float t = (r - k_curve[i - 1].rate) / (k_curve[i].rate - k_curve[i - 1].rate);
            *got = r;
            return (int)lroundf(k_curve[i - 1].raw + t * (k_curve[i].raw - k_curve[i - 1].raw));
        }
    *got = FAST_MAX;
    return 70;
}

static void pitch_loop(float dpitch, int active) {
    float p = camera_pitch();
    if (!active || g_pad_cy != 0) {  // not in control, or the controller is looking: follow it
        s_target = p;
        s_tracking = 0;
        g_cy_override = 1000;
        for (int i = 0; i < LATENCY; i++) s_inflight[i] = 0;
        return;
    }
    if (!s_tracking) s_target = p;
    s_target += dpitch;
    if (s_target > PITCH_LIMIT) s_target = PITCH_LIMIT;
    if (s_target < -PITCH_LIMIT) s_target = -PITCH_LIMIT;
    float pending = 0;
    for (int i = 0; i < LATENCY; i++) pending += s_inflight[i];
    float err = s_target - (p + pending);
    float want = err * 0.6f;  // close most of the remaining gap per frame (higher overshoots)
    float got = 0;
    int raw = raw_for_rate(fabsf(want), &got);
    s_tracking = !(raw == 0 && fabsf(err) < 0.004f && dpitch == 0);  // settled: hand back
    for (int i = LATENCY - 1; i > 0; i--) s_inflight[i] = s_inflight[i - 1];
    s_inflight[0] = want >= 0 ? got : -got;
    // Looking up (positive pitch) is negative raw Y.
    g_cy_override = s_tracking ? (want >= 0 ? -raw : raw) : 1000;
}

// TUROK_MOUSETEST=<frames>: after that many first-person frames, inject a yaw step
// and pitch moves and log how the rendered camera follows (sign/scale/latency check).
static void mouse_test(void) {
    static int frames = -1, n;
    static float y0, p0;
    if (frames == -1) { const char *e = getenv("TUROK_MOUSETEST"); frames = e ? atoi(e) : 0; }
    if (!frames || !player_view()) return;
    n++;
    int t = n - frames;
    if (t == 0) { y0 = camera_yaw(); core_mouse_look(0.5f, 0.0f); }
    if (t == 3) logf_("[mousetest] yaw +0.5 -> camera yaw moved %.3f", wrap_pi(camera_yaw() - y0));
    if (t == 10) { p0 = camera_pitch(); core_mouse_look(0.0f, 0.3f); }
    if (t >= 10 && t <= 50 && t % 5 == 0) logf_("[mousetest] pitch step +0.3: frame %d moved %.3f", t - 10, camera_pitch() - p0);
    if (t == 60) p0 = camera_pitch();
    if (t >= 60 && t < 90) core_mouse_look(0.0f, -0.01f);  // slow drag down
    if (t >= 60 && t <= 110 && t % 5 == 0)
        logf_("[mousetest] drag -0.01/frame: frame %d moved %.3f (asked %.3f)", t - 60, camera_pitch() - p0,
              -0.01f * (t - 60 < 30 ? t - 60 : 30));
}

static void apply_mouse_look(void) {
    mouse_test();
    AcquireSRWLockExclusive(&s_lock);
    float dyaw = s_dyaw, dpitch = s_dpitch;
    s_dyaw = s_dpitch = 0;
    ReleaseSRWLockExclusive(&s_lock);
    uint32_t v = player_view();
    if (v && dyaw != 0) {
        float yaw = fmodf(u2f(rd32(v + VIEW_YAW)) + dyaw, TWO_PI);
        if (yaw < 0) yaw += TWO_PI;
        wr32(v + VIEW_YAW, f2u(yaw));
    }
    pitch_loop(v ? dpitch : 0, v != 0 && (dpitch != 0 || s_tracking));
}

// GetAxisX(port, id): the input poll reads the C-stick once per game frame.
void fn_80007734(PPCContext *c) {
    if (c->r[4] == CSTICK_ID && c->r[3] == 0) apply_mouse_look();
    orig_80007734(c);
}
