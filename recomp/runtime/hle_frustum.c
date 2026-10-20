// Widescreen frustum culling.
//
// The host draws the world wider than 4:3 (it narrows the projection's X row by
// 640/EFB width; see "Dynamic widescreen" in analysis/NOTES.md), but the game culls
// against its own 4:3 frustum, so objects at the widened edges would be missing.
//
// Camera objects (the active one is at 0x802CC3B8) cache cot(hfov/2) at +0x108 and
// cot(vfov/2) at +0x10C (recomputed when flag 0x2 of +0x1A8 is set). Two builders
// derive the culling volumes from +0x108:
//
//  * fn_8000EB88: the side/top/bottom/near/far planes at +0x144..+0x1A0, used by the
//    per-object test fn_8000F8E8 (cached under flag 0x10, space key at +0x1A4);
//  * fn_8000F2A4: the four corner rays at +0x110..+0x134 that the scene turns into its
//    view cone for world/sector culling (cached under flag 0x20, key at +0x140).
//
// Both are wrapped: +0x108 is divided by the host's widening factor for the call and
// put back afterwards, so only horizontal culling widens. The rendered projection
// (fn_80004F20 reads the FOV at +0xEC, not +0x108) and the world-to-screen mapping
// (fn_8000E9DC) stay 4:3. fn_8000F8E8's inline fast accept still uses the 4:3 value;
// objects outside it fall through to the (widened) plane test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include "ppc.h"
#include "host.h"
#include "core_api.h"
#include "../generated/funcs.h"

#define CAM_COT_H 0x108u
#define CAM_FLAGS 0x1A8u
#define FLAG_COT 0x2u
#define FLAG_PLANES 0x10u
#define FLAG_RAYS 0x20u

static volatile float s_widen = 1.0f;  // EFB width / 640

CORE_API void core_set_aspect(float widen) {
    s_widen = widen < 1.0f ? 1.0f : widen > 4.0f ? 4.0f : widen;
}

// The factor each camera's cached volume was last built with (by camera address), so a
// change of window shape rebuilds it.
typedef struct { uint32_t cam; float planes, rays; } CamWiden;
static CamWiden s_cams[16];

static CamWiden *cam_entry(uint32_t cam) {
    for (int i = 0; i < 16; i++)
        if (s_cams[i].cam == cam) return &s_cams[i];
    for (int i = 0; i < 16; i++)
        if (s_cams[i].cam == 0) { s_cams[i].cam = cam; s_cams[i].planes = s_cams[i].rays = 1.0f; return &s_cams[i]; }
    s_cams[0].cam = cam; s_cams[0].planes = s_cams[0].rays = 1.0f;
    return &s_cams[0];
}

// TUROK_CULL_SCALE=x: multiply the culling width by x on top (debugging: 0.5 makes
// objects visibly vanish at the sides, showing which culling these builders control).
static float cull_scale(void) {
    static float s = -1;
    if (s < 0) { const char *e = getenv("TUROK_CULL_SCALE"); s = e ? (float)atof(e) : 1.0f; if (s <= 0) s = 1.0f; }
    return s;
}

int no_cull(void);
#define CAM_COT_V 0x10Cu
#define NOCULL_WIDEN 3.0f  // extra widening of the scene cone/planes in no-cull mode

static void call_widened(PPCContext *c, void (*orig)(PPCContext *), uint32_t flag, float *built) {
    uint32_t cam = c->r[3];
    float kv = no_cull() ? NOCULL_WIDEN : 1.0f;
    float k = s_widen * cull_scale() * kv;
    if (k == 1.0f && *built == 1.0f) { orig(c); return; }
    uint32_t args[8];
    for (int i = 0; i < 8; i++) args[i] = c->r[3 + i];
    uint32_t lr = c->lr;
    int force = *built != k;
    if (rd32(cam + CAM_FLAGS) & FLAG_COT) {
        // Let the game compute the cotangents first (that also builds a 4:3 volume).
        orig(c);
        for (int i = 0; i < 8; i++) c->r[3 + i] = args[i];
        c->lr = lr;
        force = 1;
    }
    if (force) wr32(cam + CAM_FLAGS, rd32(cam + CAM_FLAGS) | flag);
    uint32_t cot = rd32(cam + CAM_COT_H), cotv = rd32(cam + CAM_COT_V);
    wr32(cam + CAM_COT_H, f2u(u2f(cot) / k));
    wr32(cam + CAM_COT_V, f2u(u2f(cotv) / kv));
    orig(c);
    wr32(cam + CAM_COT_H, cot);
    wr32(cam + CAM_COT_V, cotv);
    *built = k;
}

// Culling planes: fn_8000EB88(cam, space, out pointers...).
void fn_8000EB88(PPCContext *c) {
    CamWiden *e = cam_entry(c->r[3]);
    call_widened(c, orig_8000EB88, FLAG_PLANES, &e->planes);
}

// View-cone corner rays: fn_8000F2A4(cam, space, out pointers...).
void fn_8000F2A4(PPCContext *c) {
    CamWiden *e = cam_entry(c->r[3]);
    call_widened(c, orig_8000F2A4, FLAG_RAYS, &e->rays);
}

// ---- Point/sphere visibility tests ---------------------------------------------------------
// fn_8000F5A0 (point) and fn_8000F750 / fn_800103F8 (sphere, radius in f1) test against
// the camera's cotangents at +0x108/+0x10C directly, returning 1 when visible. They are
// widened the same way: the cotangent is divided by the host factor for the call (after
// letting the game refresh it first when its dirty flag is set).
//
// TUROK_NOCULL=1 makes every test here report "visible" (render everything in view of
// any object system that asks); TUROK_CULLSTATS=1 logs how often each test rejects.
static int env_flag(const char *name, int *cache) {
    if (*cache < 0) { const char *e = getenv(name); *cache = e && *e && *e != '0'; }
    return *cache;
}
int no_cull(void) { static int v = -1; return env_flag("TUROK_NOCULL", &v); }
static int cull_stats(void) { static int v = -1; return env_flag("TUROK_CULLSTATS", &v); }

typedef struct { const char *name; unsigned calls, rejects; } CullStat;
static CullStat s_stat[4] = { {"point F5A0"}, {"sphere F750"}, {"sphere 103F8"}, {"box 101F0"} };
static void stat_add(int i, int visible) {
    if (!cull_stats()) return;
    s_stat[i].calls++;
    if (!visible) s_stat[i].rejects++;
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    logf_("[cull] %s %u/%u rejected | %s %u/%u | %s %u/%u | %s %u/%u", s_stat[0].name, s_stat[0].rejects, s_stat[0].calls,
          s_stat[1].name, s_stat[1].rejects, s_stat[1].calls, s_stat[2].name, s_stat[2].rejects, s_stat[2].calls,
          s_stat[3].name, s_stat[3].rejects, s_stat[3].calls);
    for (int k = 0; k < 4; k++) s_stat[k].calls = s_stat[k].rejects = 0;
}

static void test_widened(PPCContext *c, void (*orig)(PPCContext *), int which) {
    uint32_t cam = c->r[3];
    float k = s_widen * cull_scale();
    if (k != 1.0f) {
        PPCContext save = *c;
        if (rd32(cam + CAM_FLAGS) & FLAG_COT) { orig(c); *c = save; }  // refresh the cotangents
        uint32_t cot = rd32(cam + CAM_COT_H);
        wr32(cam + CAM_COT_H, f2u(u2f(cot) / k));
        orig(c);
        wr32(cam + CAM_COT_H, cot);
    } else {
        orig(c);
    }
    stat_add(which, c->r[3] != 0);
    if (no_cull()) c->r[3] = 1;
}

void fn_8000F5A0(PPCContext *c) { test_widened(c, orig_8000F5A0, 0); }
void fn_8000F750(PPCContext *c) { test_widened(c, orig_8000F750, 1); }
void fn_800103F8(PPCContext *c) { test_widened(c, orig_800103F8, 2); }

// Box test (camera inside the box, else the plane test in fn_8000F8E8).
void fn_800101F0(PPCContext *c) {
    orig_800101F0(c);
    stat_add(3, c->r[3] != 0);
    if (no_cull()) c->r[3] = 1;
}
