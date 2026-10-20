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

static void call_widened(PPCContext *c, void (*orig)(PPCContext *), uint32_t flag, float *built) {
    uint32_t cam = c->r[3];
    float k = s_widen * cull_scale();
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
    uint32_t cot = rd32(cam + CAM_COT_H);
    wr32(cam + CAM_COT_H, f2u(u2f(cot) / k));
    orig(c);
    wr32(cam + CAM_COT_H, cot);
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
