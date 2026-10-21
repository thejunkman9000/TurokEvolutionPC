// Public API of turok_core.dll (consumed by the MonoGame host via P/Invoke).
// Keep the structs in sync with host/TurokHost/Core.cs.
#pragma once
#include <stdint.h>

#ifdef TUROK_CORE_BUILD
#define CORE_API __declspec(dllexport)
#else
#define CORE_API __declspec(dllimport)
#endif

enum { CORE_NOT_STARTED = 0, CORE_STARTING = 1, CORE_RUNNING = 2, CORE_FAILED = 3, CORE_EXITED = 4 };

// Load main.dol from the disc image and start the game on its own thread.
CORE_API int core_start(const char *iso_path, const char *log_path);
// Current CORE_* state; on CORE_FAILED, msg receives the error.
CORE_API int core_state(char *msg, int msg_len);

// ---- video ----------------------------------------------------------------------
// The game's GX command stream is turned into batches of already-decoded
// triangles. A finished frame is published when the game copies the EFB to
// the display (XFB); the host takes the newest one each Draw.

#pragma pack(push, 4)
typedef struct CoreVertex {
    float x, y, z;       // view space (after the position matrix)
    uint32_t color0;     // lit colour channel 0, RGBA8 (R in the low byte)
    uint32_t color1;     // lit colour channel 1
    float uv[8][2];      // texgen outputs 0..7 (normalised, projective divide done)
} CoreVertex;

// TEV configuration for one draw, already decoded into shader-ready floats.
// Selector values follow the GX register encodings (see gx_material.c).
typedef struct CoreMaterial {
    float color_in[16][4];   // per stage: colour inputs a, b, c, d (GX_CC_*)
    float alpha_in[16][4];   // per stage: alpha inputs a, b, c, d (GX_CA_*)
    float color_op[16][4];   // per stage: bias (0..3, 3 = compare), scale (0..3), subtract, clamp
    float alpha_op[16][4];
    float stage[16][4];      // per stage: tex map (-1 none), ras channel (0, 1, -1 zero), colour dest, alpha dest
    float swap[16][4];       // per stage: ras swap table, tex swap table
    float konst[16][4];      // per stage: selected konst colour (rgb) and alpha
    float swap_table[4][4];  // channel index (0..3) for r, g, b, a
    float regs[4][4];        // initial PREV, C0, C1, C2
    float map_coord[2][4];   // texcoord used by each tex map 0..7
    float alpha_test[4];     // ref0, ref1, comp0, comp1
    float misc[4];           // alpha-test logic op, stage count, 0, 0
    uint32_t blend;          // raw BP CMODE0
    uint32_t zmode;          // raw BP ZMODE
    uint32_t cull;           // 0 none, 1 back, 2 front, 3 all
    uint32_t reserved;
    // Fog (BP 0xEE-0xF2) and what is needed to rebuild 24-bit window Z:
    float fog[4];            // a, c, b_magnitude, b_shift
    float fog_mode[4];       // fsel (0 off, 2 linear, 4 exp, 5 exp2, 6 bexp, 7 bexp2), ortho, z scale, z offset
    float fog_color[4];      // rgb
    // Indirect texturing (bump). Values follow the GX register fields.
    float ind_cmd0[16][4];   // per TEV stage: bt (ind stage), fmt, bias, bump-alpha select
    float ind_cmd1[16][4];   // per TEV stage: matrix index (0 off, 1-3), matrix id (0 static, 1 S, 2 T),
                             //                wrap s * 8 + wrap t, add previous coord
    float ind_mtx[6][4];     // 3 matrices x 2 rows: m0, m1, m2 (signed 11-bit), shift (17 - scale)
    float ind_stage[4][4];   // per indirect stage: tex map, texcoord, s shift, t shift
    float coord_size[8][4];  // per texcoord: setup-unit size (SU_SSIZE+1, SU_TSIZE+1)
    float ind_misc[4];       // number of indirect stages, any stage uses an indirect op
} CoreMaterial;

typedef struct CoreBatch {
    float proj[16];      // row-major 4x4, GX projection converted to a D3D-style clip space
    uint32_t first_vertex, vertex_count;  // triangle list
    uint32_t material;   // index into CoreFrame.materials
    int32_t textures[8]; // per tex map: index into CoreFrame.textures, -1 = unused
    uint32_t sampler[8]; // per tex map: raw TX_SETMODE0 (wrap / filter)
    int32_t viewport[4]; // x, y, w, h in EFB pixels (640x528 space)
    int32_t scissor[4];  // x0, y0, x1, y1
    // kind 1 = EFB-to-texture copy (no vertices): copy src rect into texture
    // copy_id, converting channels per copy_mode, then optionally clear the rect.
    uint32_t kind;
    uint32_t copy_id;
    uint32_t copy_mode;  // CORE_COPY_* channel conversion | 0x100 half scale | 0x200 clear
    int32_t copy_src[4]; // x, y, w, h in EFB pixels
    uint32_t clear_color;  // RGBA8
    float clear_depth;     // 0..1
} CoreBatch;

// EFB copy channel conversions (what a texture read of the copy returns)
enum { CORE_COPY_RGBA = 0, CORE_COPY_RRRR, CORE_COPY_RRRA, CORE_COPY_AAAA, CORE_COPY_GGGG, CORE_COPY_BBBB,
       CORE_COPY_YYYY, CORE_COPY_YYYA, CORE_COPY_RGB1, CORE_COPY_RRRG, CORE_COPY_GGGB };

typedef struct CoreTexture {
    uint32_t id;         // stable id for host-side caching
    uint32_t width, height;
    uint32_t levels;     // mip levels in rgba (level 0 first, each half the size, min 1x1)
    const uint32_t *rgba;  // RGBA8 pixels, valid until the frame is released;
                           // NULL = the EFB copy with this id (made by a kind-1 batch)
} CoreTexture;

typedef struct CoreFrame {
    uint32_t serial;
    uint32_t efb_width, efb_height;
    uint32_t clear_color;       // RGBA8
    uint32_t batch_count, vertex_count, texture_count, material_count;
    const CoreBatch *batches;
    const CoreVertex *vertices;
    const CoreTexture *textures;
    const CoreMaterial *materials;
    // Mouse look consumed (radians, running totals as passed to core_mouse_look) by the
    // game frame this was drawn from, and its view pitch in degrees; look_valid = 0
    // when the player's view wasn't the camera. Lets the host reproject the frame.
    double look_yaw, look_pitch;
    float look_pitch_deg;
    uint32_t look_valid;
} CoreFrame;
#pragma pack(pop)

// Newest completed frame (NULL if none since the last call). The returned
// frame stays valid until core_release_frame.
CORE_API const CoreFrame *core_acquire_frame(void);
CORE_API void core_release_frame(void);

// Debug: log every draw of the next frame the core completes to the log file.
CORE_API void core_request_gx_dump(void);
// Debug toggles: 1 = light-driven alpha (distance fades) on/off.
CORE_API void core_debug_toggle(int which);

// Debug: guest main RAM (big-endian, 24 MB at physical 0); size in *size.
CORE_API const uint8_t *core_ram(uint32_t *size);

// ---- audio ------------------------------------------------------------------------
// PCM the game plays through the AI DMA. core_read_audio copies up to max_frames
// interleaved stereo frames (L, R, signed 16-bit host endian) and returns the count.
CORE_API int core_audio_rate(void);
CORE_API int core_read_audio(int16_t *dst, int max_frames);

// ---- input ----------------------------------------------------------------------
// GameCube pad state for port 0..3. buttons uses the PAD_BUTTON_* bit layout.
CORE_API void core_set_pad(int port, uint16_t buttons, int8_t stick_x, int8_t stick_y,
                           int8_t cstick_x, int8_t cstick_y, uint8_t trigger_l, uint8_t trigger_r);

// Mouse look: rotate the player's first-person view directly (radians; positive
// yaw turns left, positive pitch looks up). Accumulated and applied once per game
// frame; ignored when the player's view isn't the active camera.
CORE_API void core_mouse_look(float dyaw, float dpitch);

// 1 while core_mouse_look applies (on-foot first-person control); 0 in flight
// sections, cutscenes and menus, where the host falls back to C-stick emulation.
CORE_API int core_first_person(void);

// Display refresh: call once per presented frame on a ~60 Hz display so the guest's
// retrace interrupt is paced by the display instead of a free-running timer.
CORE_API void core_vsync(void);

// Game speed multiplier (1 = normal; e.g. 4 = fast-forward). Scales the guest's clock,
// so timers, retrace and audio all run faster; the game runs as fast as the CPU allows.
CORE_API void core_set_speed(float speed);
CORE_API float core_get_speed(void);  // effective speed, including the intro skip's fast-forward

// Widescreen: the host draws the world `widen` (EFB width / 640) times wider than 4:3;
// the game's horizontal frustum culling is widened to match (1 = original).
CORE_API void core_set_aspect(float widen);
CORE_API int core_in_menu(void);  // 1 while a front-end menu screen is loaded (host pillarboxes it)
