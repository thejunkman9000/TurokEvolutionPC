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
#include <stdio.h>
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
static volatile LONG s_first_person;  // last poll: the player's view drove the camera

CORE_API void core_mouse_look(float dyaw, float dpitch) {
    AcquireSRWLockExclusive(&s_lock);
    s_dyaw += dyaw;
    s_dpitch += dpitch;
    ReleaseSRWLockExclusive(&s_lock);
}

// 1 while mouse look applies (first-person control), 0 in flight, cutscenes and menus.
CORE_API int core_first_person(void) { return (int)s_first_person; }

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
    static int yaw_only;
    if (frames == -1) { const char *e = getenv("TUROK_MOUSETEST"); frames = e ? atoi(e) : 0; if (frames < 0) { frames = -frames; yaw_only = 1; } }
    if (!frames || !player_view()) return;
    n++;
    int t = n - frames;
    if (t == 0) { y0 = camera_yaw(); core_mouse_look(0.5f, 0.0f); }
    if (t == 3) logf_("[mousetest] yaw +0.5 -> camera yaw moved %.3f", wrap_pi(camera_yaw() - y0));
    if (yaw_only) return;  // TUROK_MOUSETEST=-<frames>: just the yaw step
    if (t == 10) { p0 = camera_pitch(); core_mouse_look(0.0f, 0.3f); }
    if (t >= 10 && t <= 50 && (t <= 16 || t % 5 == 0))
        logf_("[mousetest] pitch step +0.3: frame %d moved %.3f (actor %.2f deg)", t - 10, camera_pitch() - p0,
              u2f(rd32(rd32(PLAYER_VIEW_PTR) + VIEW_PITCH)));
    if (t == 60) p0 = camera_pitch();
    if (t >= 60 && t < 90) core_mouse_look(0.0f, -0.01f);  // slow drag down
    if (t >= 60 && t <= 110 && t % 5 == 0)
        logf_("[mousetest] drag -0.01/frame: frame %d moved %.3f (asked %.3f)", t - 60, camera_pitch() - p0,
              -0.01f * (t - 60 < 30 ? t - 60 : 30));
    // Steady turn: 0.02 rad per poll for 30 polls; the camera should move 0.02 every poll.
    static float ly;
    if (t >= 140 && t < 172) {
        float y = camera_yaw();
        if (t > 140) logf_("[mousetest] steady turn poll %d: camera moved %.4f", t - 140, wrap_pi(y - ly));
        ly = y;
        if (t < 170) core_mouse_look(0.02f, 0.0f);
    }
    if (t == 120) core_mouse_look(0.0f, 2.0f);   // past the limits: clamped to +-70 deg
    if (t == 125) core_mouse_look(0.0f, -4.0f);
    if (t == 122 || t == 127) logf_("[mousetest] clamp: camera pitch %.3f", camera_pitch());
}

// ---- frame stamps for the host's view reprojection ----------------------------------
// The host shows the newest game frame at every display refresh. To keep mouse look
// smooth on displays faster than 60 Hz it rotates that frame by the mouse motion the
// game has not consumed yet. For that each published frame carries the running total
// of look motion consumed up to the poll that produced it (applied or dropped alike,
// so the host's "sent" total stays comparable) and the view pitch it was drawn at.
static double s_used_yaw, s_used_pitch;
typedef struct { double yaw, pitch; float pitch_deg; uint32_t valid; } LookStamp;
static LookStamp s_stamps[8];
static int s_stamp_head, s_stamp_count;
static SRWLOCK s_stamp_lock = SRWLOCK_INIT;
static LookStamp s_last_stamp;

// GXCopyDisp (CPU side): the game finished building a frame. Queue its stamp; the GX
// thread pops it when it reaches the display copy (gx_render.c, publish_frame).
void fn_80133480(PPCContext *c) {
    uint32_t v = player_view();
    LookStamp st = {s_used_yaw, s_used_pitch, v ? u2f(rd32(v + VIEW_PITCH)) : 0.f, v != 0};
    AcquireSRWLockExclusive(&s_stamp_lock);
    if (s_stamp_count == 8) { s_stamp_head = (s_stamp_head + 1) & 7; s_stamp_count--; }
    s_stamps[(s_stamp_head + s_stamp_count++) & 7] = st;
    ReleaseSRWLockExclusive(&s_stamp_lock);
    orig_80133480(c);
}

void look_stamp_pop(double *yaw, double *pitch, float *pitch_deg, uint32_t *valid) {
    AcquireSRWLockExclusive(&s_stamp_lock);
    if (s_stamp_count) { s_last_stamp = s_stamps[s_stamp_head]; s_stamp_head = (s_stamp_head + 1) & 7; s_stamp_count--; }
    ReleaseSRWLockExclusive(&s_stamp_lock);
    *yaw = s_last_stamp.yaw; *pitch = s_last_stamp.pitch; *pitch_deg = s_last_stamp.pitch_deg; *valid = s_last_stamp.valid;
}

static void apply_mouse_look(void) {
    mouse_test();
    AcquireSRWLockExclusive(&s_lock);
    float dyaw = s_dyaw, dpitch = s_dpitch;
    s_dyaw = s_dpitch = 0;
    ReleaseSRWLockExclusive(&s_lock);
    s_used_yaw += dyaw;
    s_used_pitch += dpitch;
    uint32_t v = player_view();
    static unsigned polls;
    polls++;
    // Debug: TUROK_LOOKTIMING=1 logs the interval between game input polls once a second.
    static int timing = -1; static uint64_t last, mn = ~0ull, mx, sum; static unsigned cnt;
    if (timing == -1) timing = getenv("TUROK_LOOKTIMING") != NULL;
    if (timing) {
        uint64_t now = host_time_ticks();
        if (last) { uint64_t d = now - last; if (d < mn) mn = d; if (d > mx) mx = d; sum += d; cnt++; }
        last = now;
        if (cnt == 60) { logf_("[looktiming] polls: avg %.2f ms min %.2f max %.2f", sum * 1000.0 / TB_FREQ / cnt, mn * 1000.0 / TB_FREQ, mx * 1000.0 / TB_FREQ); cnt = 0; sum = 0; mn = ~0ull; mx = 0; }
    }
    if (InterlockedExchange(&s_first_person, v != 0) != (v != 0)) logf_("[look] first-person %s (poll %u)", v ? "on" : "off", polls);
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
    if (c->r[4] == CSTICK_ID && c->r[3] == 0 && player_view()) {  // debug: TUROK_POSLOG=1 logs the player position; TUROK_TELEPORT=frame:x,y,z[,yaw,pitch] (game frames; F4 captures print these)
        static unsigned fr; fr++;
        uint32_t v = player_view();
        const char *t = getenv("TUROK_TELEPORT");
        unsigned at; float x, y, z, yaw, pitch;
        int k = t ? sscanf(t, "%u:%f,%f,%f,%f,%f", &at, &x, &y, &z, &yaw, &pitch) : 0;
        if (k >= 4 && fr == at) {
            wr32(v + 0x34, f2u(x)); wr32(v + 0x38, f2u(y)); wr32(v + 0x3C, f2u(z));
            if (k == 6) { wr32(v + 0xF0, f2u(yaw)); wr32(v + 0x41C, f2u(pitch)); }
            logf_("[pos] teleported"); }
        if (getenv("TUROK_POSLOG") && fr % 60 == 0)
            logf_("[pos] f%u view %08X %.2f %.2f %.2f", fr, v, u2f(rd32(v + 0x34)), u2f(rd32(v + 0x38)), u2f(rd32(v + 0x3C)));
    }
    orig_80007734(c);
}


// Per-object opacity (cross-fade) setter: fn_80106BEC(r3 = renderer, r4 = material
// slot, f1 = opacity 0..1). It sets the konst alpha (opacity * 255) and the alpha
// test reference (opacity * 244) used by blended foliage. Debug/experiment: with
// fades forced (TUROK_NOFADE=1 or core_debug_toggle(2), host F2) everything draws opaque.
volatile int g_no_fade = -1;
void fn_80106BEC(PPCContext *c) {
    if (g_no_fade < 0) g_no_fade = getenv("TUROK_NOFADE") != NULL;
    if (g_no_fade > 0) c->f[1] = 1.0;
    orig_80106BEC(c);
}
