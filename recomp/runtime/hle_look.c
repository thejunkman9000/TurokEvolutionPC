// Mouse look.
//
// The game turns the view from the C-stick through a dead zone, a squared response
// curve, a smoothing ramp (20% per frame) and a top speed of about 180 deg/s (yaw) /
// 120 deg/s (pitch), with 2-3 frames of latency, so mapping mouse motion onto the
// stick feels laggy and slow. Instead both angles are written directly on the
// player's view actor, *(0x803113AC):
//
//  * +0xF0 is its yaw in radians (the game derives +0xC4 and the camera from it).
//  * +0x41C is its look pitch in degrees, clamped by the game to +-70. The player's
//    look update (fn_801BB0A8 -> fn_801BAD84) integrates the stick into it.
//
// The game keeps integrating the controller's stick on top, so gamepad look still
// works. Everything is applied once per game frame at the C-stick poll (GetAxisX(port
// 0, 407), fn_80007734), before the player update, and only while the player's view
// actor drives the rendered camera, so cutscenes and other cameras are left alone.
// How these were found: analysis/NOTES.md ("Mouse look").
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
#define VIEW_PITCH 0x41Cu             // look pitch (degrees, positive = up)
#define PITCH_LIMIT 70.0f             // the game's own clamp (fn_801BAD84)
#define RAD2DEG 57.2957795f

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
    if (!(fabsf(u2f(rd32(v + VIEW_PITCH))) <= PITCH_LIMIT + 1.f)) return 0;
    if (fabsf(wrap_pi(yaw - camera_yaw() - PI)) > 0.15f) return 0;  // actor yaw = camera yaw + pi
    return v;
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
    if (t >= 10 && t <= 50 && (t <= 16 || t % 5 == 0))
        logf_("[mousetest] pitch step +0.3: frame %d moved %.3f (actor %.2f deg)", t - 10, camera_pitch() - p0,
              u2f(rd32(rd32(PLAYER_VIEW_PTR) + VIEW_PITCH)));
    if (t == 60) p0 = camera_pitch();
    if (t >= 60 && t < 90) core_mouse_look(0.0f, -0.01f);  // slow drag down
    if (t >= 60 && t <= 110 && t % 5 == 0)
        logf_("[mousetest] drag -0.01/frame: frame %d moved %.3f (asked %.3f)", t - 60, camera_pitch() - p0,
              -0.01f * (t - 60 < 30 ? t - 60 : 30));
    if (t == 120) core_mouse_look(0.0f, 2.0f);   // past the limits: clamped to +-70 deg
    if (t == 125) core_mouse_look(0.0f, -4.0f);
    if (t == 122 || t == 127) logf_("[mousetest] clamp: camera pitch %.3f", camera_pitch());
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
    if (v && dpitch != 0) {
        float p = u2f(rd32(v + VIEW_PITCH)) + dpitch * RAD2DEG;
        if (p > PITCH_LIMIT) p = PITCH_LIMIT;
        if (p < -PITCH_LIMIT) p = -PITCH_LIMIT;
        wr32(v + VIEW_PITCH, f2u(p));
    }
}

// GetAxisX(port, id): the input poll reads the C-stick once per game frame.
void fn_80007734(PPCContext *c) {
    if (c->r[4] == CSTICK_ID && c->r[3] == 0) apply_mouse_look();
    orig_80007734(c);
}
