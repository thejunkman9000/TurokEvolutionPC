# Turok: Evolution (GC, GTKE51) — static analysis notes

## Binary layout (main.dol)
| Section | Address | Size |
|---|---|---|
| T0 (init / `__start`) | 0x80003100 | 0x1a0 |
| T1 (all code) | 0x800032a0 | 0x250c00 |
| D0–D4 (data / rodata / sdata) | 0x80253ea0 – 0x80312d80 | ~0x77000 |
| BSS | 0x802c9cc0 | 0x48204 |

- Entry: `__start` @ 0x80003100. Stack top r1 = 0x80322da0; SDA bases r2 = 0x803289c0, r13 = 0x803189c0.
- 0x80146a10 is called from `__start` with (addr, 0, len), so it is likely `memset` (or the `__fill_mem` helper that `__init_bss` uses).

## Code regions (tentative)
Based on which functions touch GameCube hardware registers (`tools/hwclassify.py` → `hw_functions.txt`):

- **0x800032a0 – ~0x8011b000: game / engine code** ("newtech" engine, C++).
  - It contains around 15 functions that write the GX FIFO (0xCC008000) directly. These are inlined `GXWGFifo` vertex submissions, so the port must catch them as well as the GX API calls.
- **~0x8011b000 – ~0x80146a00: Dolphin SDK** (OS, DVD, VI, PAD/SI, AI, AR/DSP, EXI/CARD, GX).
  - About 190 hardware-touching functions are here.
  - The SDK version strings were stripped, so these functions have to be identified by signature matching.
- **0x80146a00 – 0x80253ea0: unknown.** This region holds about 1 MB of code: probably MSL/runtime first, then more game code (`Turok4/Code/...`). Still to be confirmed.

## Symbols
- There is no symbol map on the disc.
- The C++ code embeds `Class::Method` names in its assert and log strings (`Iff::Iff`, `GC_MPEG::Load`, `CDynamicMeshResource::Load`, …) and also source paths (`C:/projects/newtech/...`). These strings let us name functions by following which function references each string.

## Outputs
- `main.asm`: full linear disassembly (607,024 instructions; 56 words did not decode).
- `functions.txt`: 9,670 candidate function starts (`bl` targets plus `stwu r1` prologues), with the number of call sites for each.
- `strings.txt`: 6,875 ASCII strings with their addresses.
- `hw_functions.txt`: each function mapped to the hardware block(s) it touches.

## dtk project (`decomp/`, from dtk-template)
- Config: `decomp/config/GTKE51/config.yml`. Re-split with `tools/bin/dtk.exe dol split config/GTKE51/config.yml build/GTKE51`, run from `decomp/`.
- dtk can't analyse 0x800032a0–0x800048a0: it is hand-written exception/debug-monitor assembly that jumps between routines and has entry points nothing calls directly. The config uses `skip_cfa_ranges` for it, so the recompiler will need to treat that range by hand.
- Result: **10,526 functions**, split assembly in `decomp/build/GTKE51/asm/`.

## Symbols recovered
- **SDK version = Super Mario Sunshine's (GMSE01).** Its GX function sizes match Turok's exactly; Wind Waker's do not. The Sunshine decomp's symbols and splits are copied in `reference/`.
- `tools/sdkmatch.py` aligns each Sunshine library object against Turok by function size and order, locally per object, because the two games link the libraries in different orders.
  - **648 confident names** (OS, DVD, GX, VI, PAD, SI, AI, AR, DSP, EXI, CARD, THP, MTX, DB, TRK, part of Runtime). They are applied to `symbols.txt`.
  - `tools/checkhw.py` cross-checks them against hardware-register usage: 167 agree, and the 5 flagged turn out to be correct.
  - Isolated (WEAK) matches are not applied. Spot checks show they are unreliable.
- Turok's **MSL (C runtime) is a different version** from Sunshine's, so only a few functions matched. If those functions need names, the next step is a signature set for a different CodeWarrior version.
- `tools/strnames.py` named **12 game functions** from `Class::Method` strings (`Iff::*`, `GC_MPEG::*`, `Font::load`, …). It also found 45 functions that reference source paths (`analysis/game_sources.txt`).
- `tools/vtables.py`: the game was built **without RTTI**, so vtables carry no class names.
  - It found 454 vtables with 3,209 distinct virtual targets, all of which are already dtk functions. So indirect-call targets are covered.
  - The scanner merges some neighboring vtables; 17 tables hit its 400-slot cap. Fix that before using vtables to name classes.
- Total: 660 named functions out of 10,526.

## Static recompiler (`tools/recomp/ppcrecomp.py` → `recomp/`)
- Generates C for 10,544 entry points (620k instructions; the only unimplemented words are data/padding) into `recomp/generated/`.
  - In-function branches become gotos, `bl` becomes a direct call, `blr` a return.
  - `bctr`/`bctrl`/`blrl` go through `call_indirect()`, which looks up the function table.
- Build:
  1. Run `python -I tools/recomp/ppcrecomp.py extracted/main.dol decomp/config/GTKE51/symbols.txt recomp/hle_overrides.txt recomp/generated`.
  2. Run `tools\msvc.cmd ninja -C recomp\build` (configure first with `cmake -S recomp -B recomp\build -G Ninja`). A full build takes about 2 minutes.
- Run with `recomp\build\turok.exe "Turok - Evolution NTSC-U.iso"`. It logs to `turok_recomp.log`.
  - `TUROK_WATCHDOG=<s>` dumps the guest call stack after that many seconds.
  - `TUROK_WATCH=<hex addr>` is a guest write watchpoint.
- Runtime pieces:
  - Memory and MMIO logging.
  - EXI (RTC/SRAM/UART), DSP/ARAM, VI retrace at 60 Hz, PI interrupt causes, PE draw-done/token interrupts.
  - A GX FIFO parser that tracks vertex sizes.
  - The decrementer.
  - Interrupt delivery at backward branches and `mtmsr`, through the SDK's exception table.
- HLE so far: `__OSInitAudioSystem`/`__OSStopAudioSystem` (DSP handshake) and `OSLoadContext`.
  - The game creates **no threads** (`OSCreateThread` is dead-stripped), so no fibers are needed.
- Status: boots through OSInit, the SDK init, VI retrace and GXDrawDone, and into the game's main loop (it polls the reset button each frame). No graphics, input, DVD or audio yet.
- Gotcha: the DOL's bss range spans `.sdata`, so it must be cleared *before* the sections are loaded.

## Native core DLL + MonoGame host
- `recomp/` builds **`turok_core.dll`** (the recompiled game plus the runtime) and `turok_run.exe` (headless runner: `turok_run.exe <iso> <seconds>`).
  - The API is in `recomp/runtime/core_api.h`, mirrored in `host/TurokHost/Core.cs`.
  - The game runs on its own thread. Fatal errors stop only that thread and are reported through `core_state`.
- **`host/TurokHost`**: the MonoGame 3.8.6 WindowsDX (D3D11) app, targeting .NET 9.
  - Build with `dotnet build -c Release`. Run with `TurokEvolution.exe [iso]`; it also finds an .iso in parent folders.
  - It draws each published `CoreFrame` with the **TEV ubershader** (`Shaders/Tev.fx`) into a 2× EFB render target and letterboxes that to the window.
  - The shader is compiled by `mgfxc` (installed in `tools/bin/dotnet`) as a build step and embedded in the exe.
  - The shader samples all 8 maps up front, then loops over up to 16 TEV stages: color/alpha combiners including compare modes, ras/tex swap tables, konst, destination registers. Last comes the two-comparator alpha test.
  - The GX blend factors/subtract/write masks, Z mode and texture wrap/filter are mapped to cached MonoGame state objects.
  - Audio: a `DynamicSoundEffectInstance` (32 kHz stereo) is fed 50 ms chunks, keeping about 150 ms queued and trimming backlog above 250 ms.
    - Verified: zero steady-state starvation over 141 s spanning menus, level load and gameplay.
    - `turok_run.exe` writes `turok_audio.wav`; `tools/spectrogram.py` renders it.
  - Screenshots: F12, or `TUROK_SCREENSHOT=<frame>` (plus `TUROK_EXIT_AFTER_SCREENSHOT=1`).
  - Input: keyboard and XInput are mapped to GameCube pad bits in `core_set_pad`; the core's `PADRead` HLE (`hle_pad.c`) feeds them to the game.
    - Modern FPS bindings (README "Controls") over the game's default FPS layout: stick move/strafe, C-stick turn/look, R fire, L duck, Z zoom, A jump, B next weapon, Y previous weapon, X weapon mode, D-pad unused.
    - `core_first_person()` (true while the player's view actor drives the camera, including the pause menu and flight) switches the host between the modern layout and plain pass-through for menus.
    - The game's Invert Look defaults to on, so the host flips the pad's right-stick Y unless `--invert-look` is passed. Mouse look writes angles directly and is unaffected.
  - Debug environment variables:
    - `TUROK_INPUT=frame:Button[:hold],...`: scripted input.
    - `TUROK_SCREENSHOT=f1,f2,...`: capture those frames.
    - `TUROK_GXDUMP=frame`: log every draw of that frame.
    - `TUROK_DUMP_TEXTURES=frame`: write that frame's textures plus `batches.txt`.
    - `TUROK_DEBUG_VIEW=1|2|3|4|5|6`: stage-0 texture, UV, lit color, fog amount, indirect draws in magenta, or indirect draws with no depth test (to see occluded water).
    - Stick tokens for `TUROK_INPUT`: `LX+` `LX-` `LY+` `LY-` `CX+` `CX-` `CY+` `CY-`.
    - `TUROK_CULL_FLIP=1`: swap the cull windings.
    - Core only: `TUROK_ONLY_VAT=n` and `TUROK_LOG_REDIRECT=1`.
- Hardware added for the renderer:
  - DVD interface backed by the ISO (`di.c`).
  - AI sample counter.
  - **Audio DSP:** mailbox emulation (boot ROM handshake, microcode upload, DSP_INIT), then an **AX microcode HLE** (`dsp_ax.c`).
    - The game uploads AX microcode 0x07F88145 (HashEctor; the same build as F-Zero GX / Ikaruga), 0x1F20 bytes from 0x802976A0.
    - Protocol: `BABE<size>` + command-list address → run the list → `DSP_YIELD` with an interrupt → `CDD1000x` task mail.
    - All GC AX commands are implemented.
    - Each voice runs per 1 ms: parameter-block updates → accelerator (ADPCM / PCM8 / PCM16 from ARAM, with loop/end handling) → resampling by the voice's SRC type (4-tap polyphase / linear / none) → volume envelope → low-pass filter → ITD → mixing into 9 buses with ramps.
    - Output goes to the R/L-interleaved RAM buffer at 32 kHz.
  - **AI DMA:** 0x5030/0x5036 start, 640-byte (5 ms) buffers, AIDINT on start and at each buffer boundary.
    - Played buffers go to a ring buffer (`audio_out.c`), which the host reads with `core_read_audio`.
    - Deadlines accumulate rather than being scheduled from "now", so stalls are caught up instead of drifting.
- GX front end (`gx_fifo.c` → `gx_render.c`, `gx_texture.c`):
  - The CP/XF/BP state tracker handles indexed attributes and LOAD_INDX.
  - Vertices are decoded on the CPU, transformed to view space, and triangulated.
  - **GX lighting** runs on the CPU: both channels, all 8 lights, spot/specular attenuation, diffuse functions, and the material/ambient sources.
  - **All 8 texgens** run too, including dual-transform post matrices, normalization and projective divide.
  - The pixel pipeline is captured per draw as a `CoreMaterial` (`gx_material.c`) and deduplicated per frame.
  - The projection is remapped to D3D depth, and viewport, scissor, blend and depth state are carried per batch.
  - Every texture format is decoded (including CMPR, the C4/C8/C14 palette formats and TLUT loads), with a content-hashed cache.
  - A frame is published on the EFB→XFB copy.
- **Memory card** (`hle_card.c`): the CARD API is replaced at its 16 public entry points the game calls.
  - Slot A is backed by Dolphin-compatible GCI files in `saves/GTKE51/Card A/`; slot B is empty.
  - Async callbacks are queued and delivered at the next interrupt safe point (`irq_queue_callback`).
- **GX FIFO routing now matches hardware:**
  - Gather-pipe writes form 32-byte bursts at the PI write pointer in RAM.
  - The CP parses only when the CPU and GPU FIFOs are linked (CP control bit 4) and reads are enabled.
  - This makes `GXBeginDisplayList` and `GXRedirectWriteGatherPipe` work. The game skins creatures on the CPU (`psq_st` to the gather pipe, redirected into vertex arrays).
  - Writing WPAR (`mtspr 921`) resets the gather pipe. Without this, redirected vertex data came out shifted by a byte and the skinned meshes turned into shards.
- **Status: playable path from boot to level 1.** Legal screens, intro video (THP), title scene, main menu and New Game all work, with saves created on the emulated card and persisting across runs; screenshots are in `analysis/`.
- **EFB→texture copies** (`gx_render.c` `efb_copy_to_texture`, host `EfbCopy`):
  - A copy becomes a kind-1 batch in the frame. The host copies that region of the (content-preserving) EFB render target into a persistent per-address render target, with channel conversion (`Shaders/Copy.fx`: R/RA/A/G/B/intensity/RGB), then clears the rectangle to the clear color and depth.
  - Texture reads at a copy's address return the copy, unless the CPU has since rewritten that memory (hash check).
  - Turok renders its projected shadow map this way: 240×240 at (0,0), R4 format, to 0x006A97A0 every frame.
- **Mipmaps:** chains are decoded when the min filter uses mips (level count from TX_SETMODE1 max LOD); the host box-filters any missing small levels. Min/mag/mip filter modes and LOD bias map to sampler states.
- **Fog:** BP 0xEE-0xF2 are captured in the material, and the TEV shader rebuilds 24-bit window Z from the viewport Z scale/offset. All five GX falloff curves, perspective and ortho, follow Dolphin's formula. Level 1 uses linear perspective fog with a slowly changing color.
- **Culling:** GENMODE 1 = cull counter-clockwise, 2 = cull clockwise, 3 = all. Verified on the title scene; `TUROK_CULL_FLIP=1` swaps it.
- **Locked-cache DMA** (SPRs 922/923, with a physical memory address) is emulated. The THP video decoder needs it; without it the intro movie and credits video decoded to garbage.
- The intro movie now plays (after boot, and after New Game), so scripted menu input needs a Start press to skip it, and menu timing varies between runs.
- Soak test: 4¾ minutes of scripted play (walking, combat with damage taken, menus) with no crashes and no audio starvation.
- **Performance:** level 1 went from 14 fps to about 40 fps (the game's target is 30).
  - The profiler showed 63% of the guest thread in `gx_texture_for_map`, hashing whole textures on every draw.
  - Texture lookups are now memoized per frame, with a TLUT-load generation counter for palette textures, and EFB copies are verified once per frame.
  - Profiling: `TUROK_PROFILE=<start s>,<duration s>` logs the top self and inclusive functions of the guest thread. A `[stats]` line each second reports fps, draws and vertices per frame, and line/point, indirect and Z-copy usage.
  - Host draw cost is about 0.6 ms per frame; `host_stats.txt` is written on exit.
- **60 fps in the heaviest levels** (GX on its own thread, vertex worker pool):
  - Fps from the in-level `[stats]` lines on the scripted New Game path, excluding the loading seconds; uncapped (`--no-vsync`), HEAD before → after. The heavy views are 6.9k-8.6k draws and 65k-85k verts per frame:

    | level | before avg / min | after avg / min | with vsync, after |
    |---|---|---|---|
    | sc-11-a (city senate) | 36.7 / 29.8 | 59.8 / 56.8 | 59.9 |
    | sc-12-a | 46.5 / 29.8 | 59.7 / 53.0 | 59.8 |
    | rwp1 | 53.2 / 29.9 | 59.9 / 59.8 | 59.8 |
    | darkjungle_1 | 31.0 / 19.4 | 59.8 / 56.6 | 59.9 |

    The remaining dips are level-load stalls and single guest-side hitches (game streaming); the GPU thread's per-frame max stays under 9 ms.
  - Why 30 fps, not 49: the main loop ends each frame with GXSetDrawSync and spins on GXReadDrawSync (fn_8000B9B8 → fn_80132300) before VIWaitForRetrace. A frame that misses the retrace waits for the next one, so heavy views locked to exactly 30. The sc-11-a pipe view was busy 20.5 ms per frame, 9.5 ms of it in gx_draw.
  - The command processor now runs on a **GPU thread** (`gx_fifo.c`). The guest copies the bytes the CP reads into an 8 MB ring; the thread parses them and runs all of `gx_render.c`.
    - This matches the hardware: commands read guest RAM (display lists, vertex arrays, textures, TLUTs) when parsed, and PE token/finish interrupts are raised when the thread reaches them.
    - The game's only GPU sync is the one draw-sync token per frame (no GXDrawDone), so it already tolerates an asynchronous GPU.
    - The PE status bits are updated atomically. The CP status register reports GP idle only once the ring is drained.
    - `TUROK_GX_THREAD=0` parses on the guest thread as before.
  - **Vertex jobs** (`gx_render.c`): in Dark Jungle each vertex averages ~1.7 spot lights and ~2.4 dual-transform texgens (sqrt and divides), about 63 ns per vertex.
    - The GPU thread decodes vertices into a per-frame arena, snapshots what the transform needs (`XfSnap`: matrices, light parameters, channel/texgen controls, post matrices), and reserves the output range.
    - Worker threads (`TUROK_GX_WORKERS`, default ncpu-4 capped at 4; 0 = inline) light, texgen and expand into triangles. They never touch guest RAM or GX state.
    - The frame is published only after all its jobs finish. A vertex-array realloc first drains the jobs.
    - Anything that changes what lighting/texgen read must go through `xf_snapshot`.
  - **Cheaper GPU-thread work:**
    - The vertex format (VCD/VAT) is decoded once per draw.
    - The state capture (material, textures, samplers, scissor) is reused while no BP register or XF viewport has been written since the previous draw. Dark Jungle: 104 captures for 8.5k draws.
    - An XF snapshot is shared by consecutive draws until XF or the matrix indices change.
  - **Guest-side fixes:**
    - `getenv("TUROK_LOG_REDIRECT")` was called on every redirected gather-pipe burst. This was 6% of the guest thread as `strchr`, from CPU skinning.
    - `call_indirect` uses a direct-mapped table instead of a binary search.
    - The GQR scales come from tables instead of `ldexp`.
    - Gather-pipe writes are dispatched first in `mmio_write`.
    - The CP FIFO feeds contiguous ranges instead of 32-byte pieces.
  - **Verified unchanged:** a temporary build ran the old and new pipelines side by side.
    - For every draw, the state matched the full recapture (~6M draws). For every published frame, the vertex array was byte-identical to the old single-threaded pipeline (4,800 frames each of darkjungle_1 and sc-11-a, with 4 workers).
    - The host is unchanged. Screenshots across runs differ only in animation phase, because the game runs on wall-clock time.
  - **`[stats]` fields** (per frame):
    - `busy`: guest time outside VIWaitForRetrace (hle.c override), with the per-second max.
    - `gpu`: GPU-thread parse time, with the per-frame max.
    - `draw`: the gx_draw part of that.
    - `xf`: transform time summed over the workers.
  - Gotcha: the host process runs with CET shadow stacks (.NET 9). A `longjmp` that skips the unwinder (`_JUMP_BUFFER.Frame = 0`) kills the host instantly with no log, though `turok_run.exe` survives it. Interrupt returns must keep the normal unwinding longjmp.
  - Gotcha: `build.ps1` writes `%TEMP%\turok_native_build.cmd`, so parallel builds in different worktrees race on it. Use a private copy for incremental builds.
- **Indirect texturing (bump)** follows Dolphin's integer semantics:
  - Indirect stages are sampled at shifted coordinates, then each stage's format, bias and bump alpha are applied, followed by the static or S/T-dynamic matrices and the wrap/add-previous steps, with s24 wrap.
  - Stages sample with explicit gradients. Setup-unit texcoord sizes come from BP 0x30+.
  - Used by about 25 draws per frame in level 1 (the river by the bridge). `TUROK_DEBUG_VIEW=5` shows indirect draws in magenta.
- **Host options:** `TurokEvolution.exe [iso] [--fullscreen] [--scale 1-4] [--4x3 | --widescreen] [--no-vsync] [--level <atr path>]`. F11 or Alt+Enter toggles borderless fullscreen.
- **Widescreen** (superseded by "Dynamic widescreen" below; `--widescreen` still gives this fixed 16:9): the EFB is 854 wide. Perspective draws whose viewport spans the full 640 width are stretched across it, with the projection's X row scaled by 640/854. Every other draw, and any EFB copy narrower than full width (such as the shadow map), stays 4:3 and is shifted to the center.
- **Mouse look:** click inside the window to capture the mouse, Esc to release. Motion turns the view directly through `core_mouse_look` (yaw and pitch are written on the player's view actor), not through the C-stick. Left/right buttons map to R/L. `--mouse-sens X` scales it (0.0022 rad/pixel at 1).
  - The game's own stick turning is unusable for a mouse. Measured by overriding the value GetAxisX returns:
    - Yaw: dead zone up to 0.3 normalized. 0.5 → 0.0075, 0.75 → 0.020, 1.0 → 0.052 rad/frame (≈180°/s max). 2 frames of latency, then a ~5-frame ramp.
    - Pitch (raw C-stick Y): dead zone up to raw 35. Raw 36-61 → 0.0012-0.0084 rad/frame, then a jump to a fast speed of 0.028-0.034 rad/frame from raw 64. 3 frames of latency, then a ramp.
  - **Yaw is written directly.** `*(0x803113AC)` (.sbss) is the player's view actor, and +0xF0 is its yaw in radians, [0, 2π).
    - The rendered camera yaw is actor yaw − π. The game derives +0xC4 and the camera from it.
    - Found by snapshotting RAM during a steady scripted turn, keeping floats whose deltas match the rendered yaw deltas, then poking each candidate. Only actor+0xF0 moved the camera.
    - Several other actors keep copies at +0xC4/+0xF0. Actor +0xEC is body pitch (writing it tilts the player model), not look pitch.
  - **Pitch is written directly too:** view actor +0x41C is the look pitch in **degrees** (positive = up), clamped by the game to ±70° (= ±1.222 rad).
    - Found by tracing code, not RAM: the per-frame player look update `fn_801BB0A8` (called from `fn_801B929C`) takes the look axes the input events store at player +0x19D8/+0x19DC (Y) and +0x19E0/+0x19E4 (X).
      - It applies the 0.3 dead zone and rescale, squares the value (`fn_801BAA28`), smooths it 20% per frame into +0x458/+0x45C (previous values at +0x460/+0x464; this is the ramp), then calls `fn_801BAB44` (yaw: adds to +0xF0) and `fn_801BAD84` (pitch).
      - `fn_801BAD84` adds `look Y × speed (+0x92C × 0.5) × dt × sensitivity level` to +0x41C, applies the invert-Y option, and clamps to ±70 (constant at 0x802800BC). +0x420 is a second angle clamped to ±45.
      - Options struct at player +0x3CC: +0x4 selects a spring/absolute mode (+0x41C is lerped toward 70 × stick instead of integrated, so mouse pitch would drift back there; it is 0 by default), +0x14 invert Y, +0x18/+0x1C X/Y sensitivity levels.
    - The actor-script query `fn_800C4240` (ids 0x1F-0x22 = main X/Y, C-stick X/Y) is not on the player's look path. It is only reached through the animation-controller driver `fn_800CBC58`, and was never called for those ids during gameplay.
    - The earlier RAM search and the group pokes in the 0x8068xxxx skeleton region missed +0x41C. The skeleton pokes most likely moved pitch through bones derived from it.
    - `hle_look.c` adds the mouse delta (converted to degrees) and clamps, at the C-stick poll before the player update. The game's own stick integration still runs on top, so gamepad look keeps working (checked with a scripted `CY+`/`CY-`). The old closed loop through C-stick Y, and the `g_cy_override` hook in PADRead, are gone.
    - Self-test (`TUROK_MOUSETEST=2700`): yaw +0.5 → 0.500 after 3 frames. Pitch +0.3 step → 0.300 on the next frame (actor 17.19°), exact from then on. A −0.01 rad/frame drag is exact every frame (−0.050 at 5, −0.300 at 30), with no trailing. +2/−4 rad steps clamp at ±1.222. The old loop needed ~20 frames to settle a step and trailed a drag by ~5 frames.
    - `TUROK_MOUSETEST` counts C-stick polls in first person, which run ahead of the host frame counter, so the screenshot frame must be well past 2700 (5600 works with the standard New Game input script). Delete `saves/` before scripted runs: with a save present, the same input script ends up in the credits.
  - The rendered camera's forward vector is cached at 0x802E22F8 (x, y, z; y up): yaw = atan2(x, z), pitch = atan2(y, |xz|). Writes happen only while actor yaw − π matches it, so cutscenes and other cameras are untouched.
  - Input plumbing found on the way:
    - PADStatus array at 0x802CC718.
    - Per-port controller struct (184 bytes) at 0x802CC748. Normalized axes: main +0x3C/+0x40, C-stick +0x44/+0x48.
    - Input events 406 (main stick) and 407 (C-stick) via fn_80007D40.
    - Axis getters GetAxisX/Y = fn_80007734/fn_800079E0 (r3 = port, r4 = id), called once per frame from fn_80052D3C through a descriptor table at 0x8025DBDC.
    - Per-player input state, 84 bytes each, at 0x803106EC (+0x10/+0x14 C-stick, Y negated).
    - Actor scripts query inputs through fn_800C4240, which applies the 0.3 dead zone.
  - Debug helpers added for this: `TUROK_RAMDUMP=f1,..` (host writes `ram_<frame>.bin`), `TUROK_POKE=frame:addr:delta` (host), `TUROK_MOUSETEST` (core).
  - Capture needs a fresh click while the window already has focus, so the click that activates the window doesn't grab the mouse. The capturing click doesn't fire, and losing focus releases the mouse.
- **Level select:** every level, front-end screens included, is loaded by .atr path through `fn_80019700` (r4 = path, e.g. `data\levels\u_jcowlishaw\screens\mainmenu\mainmenu.atr`).
  - `runtime/hle_level.c` wraps it and logs `[level] load ...`.
  - With `TUROK_LEVEL` (or the host's `--level`) set, the first non-front-end load (New Game's `pod01_01_ttjungle`) is redirected. The replacement string is written at 0x80001800, in the OS's free low memory.
  - Dead ends: Game.txt only names the front-end `begin.atr`. The `Pod01_01_TTJungle` literal at 0x8026DF74 is only the default level stored in a new save (fn_80155578).
  - The variable is read with `GetEnvironmentVariableA`, because the CRT's environment copy predates the host setting it.
- **Level sweep** (one level per pod, entered via New Game; 12 levels from pod 2 flight training to the pod 18 T-rex area): all load and run without a fatal error at 49-60 fps, up to about 8000 draws and 61k verts per frame.
  - No level uses lines, points or Z copies.
  - Indirect texturing appears in Dark Jungle and the escape flight.
- **Build:** `build.ps1 -Iso <path>` builds a clean clone end to end (verified): extract the DOL, generate C, MSVC via vswhere + CMake/Ninja (run through a temporary .cmd), install mgfxc locally, build the host.
- **Rendering accuracy pass:**
  - **Indirect + EFB copies:** indirect stages address textures in guest texels (`TexSize`), but EFB copies are render targets upscaled by `--scale` (and widened in widescreen). The shader used the target's size, so Dark Jungle's and the escape flight's full-screen distortion pass sampled the top-left quarter of the scene copy: the whole 3D view looked zoomed in 2x. The host now tracks each copy's guest size.
  - The indirect math was rechecked line by line against Dolphin (abg lookup, format shifts/bias, alpha bump, s.10 matrices with the 17-scale shift, S/T dynamic matrices, wrap, add-previous, s24 wrap). No other bugs found.
    - Level 1's river is a static-matrix bump (`[[25,0,0],[0,25,0]]`, scale 18) of a 32x32 water texture by a 32x32 IA texture. It renders as flowing streaks. It sits behind the treeline ahead of the start; `TUROK_DEBUG_VIEW=6` shows it.
    - `TUROK_DUMP_TEXTURES` now lists each indirect batch's setup and EFB-copy refs in `batches.txt`.
  - **TEV is integer now** (Dolphin's semantics):
    - a/b/c take the low 8 bits of their input, and d the full s11 value.
    - Lerp: C widened to 0..256, bias on D, scale folded into the lerp with the hardware's +127/128 rounding, then /2 for scale 1/2.
    - Clamp to 0..255 or -1024..1023.
    - The last stage's output wraps to 8 bits.
    - Alpha test and the fog blend (`ifog = round(f*256)`, `>> 8`) are integer.
    - Visible change is small: at most 1-2 levels on movies and menus. In-game differences are confined to alpha-test edges and animated objects. No banding or brightness regressions on the menus, level 1 or sc-11-a.
  - **Range-adjusted fog** (BP 0xE8 bit 10) is logged as `[fog] range adjust on` and is still not implemented. No level enables it (one level per pod checked, plus level 1, Dark Jungle and sc-11-a).
  - Every level checked uses an RGB8 EFB (`[pe]` log), so RGBA6 6-bit quantization/dither is not needed.
- Known gaps:
  - Lines and points (not used by any of the 12 sampled levels).
  - Z-buffer EFB copies.
  - Range-adjusted fog (no sampled level uses it; logged if one does).
  - Audio: the polyphase coefficients are a generated stand-in, not the DSP ROM's (see "Audio fidelity" below). The game doesn't use polyphase or ITD, so this has no audible effect.
- GX bit layouts that bit me:
  - Clear color: AR = red bits 0-7, alpha bits 8-15. GB = blue bits 0-7, green bits 8-15.
  - Scissor: y in bits 0-10, x in bits 12-22.
  - TLUT and texture addresses need masking to 0x01FFFFFF.

## Audio fidelity (AX HLE)
- **Polyphase SRC** (src_type 0) is implemented as in Dolphin's `ResampleAudio`: 4 history taps, phase = `(frac >> 9) << 2`, and `coef_select` picks one of four 128-phase tables.
  - The real tables are in the DSP ROM, which isn't shipped.
  - `init_coefs()` rebuilds them the way Dolphin's free DSP ROM does (`docs/DSP/free_dsp_rom/generate_coefs.py`): windowed sincs at 0.5 / 0.75 / 1.0 of Nyquist (Hamming / Kaiser β=9π/4 / Hamming), normalised to unity DC gain.
  - Src types 1 (linear) and 2 (none) are unchanged. `TUROK_AX_LINEAR=1` forces linear for polyphase voices, for A/B.
- **ITD** (PB words 27-33: on, buffer hi/lo, shift L/R, target L/R) delays the voice for the left and right buses by up to 31 samples each.
  - The previous millisecond's output is kept in the voice's main-RAM buffer.
  - Each shift moves one sample per millisecond toward its target.
  - Surround buses get the undelayed signal.
  - Dolphin doesn't implement ITD (it only logs a quirk), so this follows the SDK's field semantics. It isn't checked against hardware.
- **`TUROK_AX_STATS=1`** logs, every 10 s, the running voices' SRC type, coef_select, ITD, LPF, aux sends and formats. It also logs the command histogram and whether aux returns were non-silent.
- **What the game actually uses.** Level 1, Dark Jungle 1, RWP1, Sc-11-a and the T-rex area were checked with the stats logger:
  - 64 parameter blocks, every running voice ADPCM, src_type 1 (**linear**), coef 0, no ITD, no LPF.
  - So polyphase and ITD are correct for other code paths but change nothing audible in Turok.
  - The command list is always `00 02 03 10 12 0E 0F`: setup, PB list, MixAUXBLR (aux B L/R out to the game's callback and back), compressor, output.
  - Aux B is the reverb send: about half of the voices use it, and the returned buffers are non-silent in ~100% of gameplay frames, so the callback round trip works.
  - Some voices set aux A sends, but the game never issues 0x04/0x13, so aux A is dropped. That's also what hardware does with this list.
- **Intro movie audio** doesn't go through AX: no voices run during it.
  - THP audio is mixed by the CPU into the AI DMA buffer.
  - The periodic ~30 ms bursts in it are content, not clicks: smooth waveform, identical before and after the change.
  - The gameplay segments of all captures have essentially no broadband click candidates.
- **Streaming audio is not used.**
  - The DOL links no `DVDPrepareStream*` / `DVDCancelStream*` / `AISetStreamPlayState` callers outside the SDK itself.
  - `DVDLowAudioStream` is reached only from `stateBusy`; the AI stream volume calls come from SDK init and reset.
  - So DI 0xE1-0xE4 and AI streaming stay stubs.
- **Offline test:** `tools/axtest/axtest.c` includes `dsp_ax.c` and drives one voice with a PCM16 sweep.
  - Polyphase coef 0 at ratio 0.5 leaves image energy at −38.8 dB, against −18.8 dB for linear. Coef 2 gives −20.5 dB.
  - An ITD voice with target R=20 gives a right channel that equals the left delayed by exactly 20 samples once ramped.
  - `tools/axtest/srcplot.py` draws the spectrograms. Captures and plots go to `analysis/audio_captures/` (ignored).

## Next steps
1. ~~dtk setup~~ and ~~SDK naming~~ are done (see above).
2. Make the region 0x80146a00–0x80253ea0 a priority: work out where MSL ends and game code resumes.
3. Build the recompiler: lift PPC to C, one function per symbol, with paired-single (Gekko) ops handled.
4. Write the HLE layer to replace the SDK region: GX → D3D11/Vulkan, DVD → `supertree0.tre` and file reads, PAD → XInput, AI/AX → WASAPI.

## Smooth mouse look, speed control, intro skip

- **Why mouse look was choppy.** The game polls input and renders at 60 Hz with ±3 ms jitter, and the camera advances evenly per game frame (verified with the steady-turn self-test). The problems were all presentation:
  - The guest's retrace came from a free-running 59.94 Hz timer that beat against the display.
  - MonoGame's fixed timestep added duplicated and dropped presents.
  - On a 144 Hz display, 60 fps frames are held for alternately 2 and 3 refreshes. The stats showed 10,600 repeated presents for 6,200 game frames.
  - Mouse deltas were sampled once per host update via cursor warping.
- **Fixes:**
  - **Raw mouse input** (`host/TurokHost/RawMouse.cs`, WM_INPUT). Device counts accumulate between updates.
  - **Display-paced retrace.** `IsFixedTimeStep = false`. On a ~60 Hz display the host calls `core_vsync()` after every present (every other one at ~120 Hz), and `vi_update` (hw.c) raises the retrace on those ticks. It falls back to the timer when no tick has arrived for 50 ms: other refresh rates, vsync off, or fast-forward.
  - **View reprojection between game frames:**
    - Each published frame carries a look stamp: the running total of mouse look consumed up to the poll that produced it, plus its view pitch. The stamp is queued when the game calls `GXCopyDisp` on the CPU side and popped by `publish_frame` on the GX thread.
    - The host keeps the total it has sent. At every refresh it rotates the frame's world draws by the difference, yaw about world up and then pitch, using Rx(−p1)·Ry(−dy)·Rx(p0) in view space. The difference is capped at 0.15 rad, because the game frustum-culls and large offsets reveal gaps at the edges.
    - Only full-screen perspective draws with some vertex farther than 2 units are rotated. The first-person weapon is drawn in view space at |z| < 0.6 and stays attached. The HUD is orthographic.
    - Direction check: `TUROK_REPROJ_TEST=yaw,pitch`. Per-draw dump: `TUROK_BATCHINFO=<frame>` writes batchinfo.txt with projection, viewport and z range.
- **Speed control.**
  - `core_set_speed` scales guest time (`host_time_ticks`), keeping it continuous across changes, so timers, retrace and audio speed up together. The host mutes audio while sped up.
  - `--speed X`; holding Tab gives 4x.
- **Intro skip** (default; `--show-intro` disables).
  - With `TUROK_SKIP_INTRO=frontendbackground`, the level loader swaps `bootupsequence` for the front-end background, which chains to the title screen.
  - The front end runs at 4x until `mainmenu` loads.
  - The PAD HLE taps Start ~1 s after the title screen loads.
  - The main menu appears within a few seconds. Scripted test runs pass `TUROK_SKIP_INTRO=0` to keep the original boot timing.

## Picture controls and texture enhancements

- **Present pass:** `Shaders/Present.fx` applies gamma, then contrast around mid-grey, then brightness gain when scaling the EFB into the window. Settings live in `settings.json` (Settings.cs). F5-F10 adjust them, and the title bar shows the values. Screenshots (F12) stay raw EFB.
- **Distant foliage "pop-in"** (user report: palm fronds in level 1):
  - These are alpha-tested cut-outs. Their smaller mip levels average thin leaflets below the alpha threshold, so fronds thin out with distance and pop at mip-point transitions.
  - TX_SETMODE1's max LOD is already honoured, through the number of uploaded levels.
  - No geometry LOD keywords were found in the DOL or archive strings. The "partdef visible near/far" fields (fn_8005C740) belong to particle effects.
- **Fixes (default on; `--original-textures` off):**
  - Min-linear mipmapped samplers are upgraded to 8x anisotropic, which is trilinear.
  - In the TEV shader, the coarsest sampled LOD (from the explicit gradients, clamped to the uploaded level count in `TexSize.z`) boosts the alpha used for GREATER/GEQUAL alpha tests by 30% per level.
  - Result: the level 1 canopy is visibly fuller at a distance, with no blocky edges.

## Saves: backups and test isolation

- **Incident (2026-10-19):** the user's progress (a couple of levels) was replaced by a fresh level-1 save. The cause was my automated test runs, not the game's autosave: they started a New Game, answered "Yes" to the overwrite prompt, and used the real `saves\` folder. Save Game0.gci was last written at 22:21:26, between the two texture-comparison runs.
- **Fixes:**
  - `TUROK_SAVE_DIR` points the card at another folder. Every scripted run must set it.
  - The card HLE copies a save into `Card A\Backups\<name> <date time>.gci` before the first write of a save session (the file not written in the last minute), keeping the newest 20 per save.
- **Bushes vanishing up close (user report).** This needs a capture from the spot.
  - A walking script couldn't reach it.
  - One light-driven alpha setup exists: light 7, spot attenuation, at the camera, cos (-8.73, 1000, 0), dist (1, 0, 1.083), dir view-up. My attenuation matches Dolphin's formulas.
  - Debug keys added: F3 toggles light-driven alpha (`core_debug_toggle(1)` / `TUROK_NOALPHALIGHT=1`). F4 (or `TUROK_CAPTURE=<frame>`) writes `captures\<time>\` with screen.png, the frame's textures, and draws.txt (per draw: z range, vertex alpha ranges, alpha test, blend, stage-0 alpha inputs, depth), plus a GX dump in the log. `TUROK_LIGHTLOG=1` logs light-driven alpha setups.

## Dynamic widescreen

- **Default:** the 3D world takes the window's shape. The EFB is `480 x aspect` guest pixels wide (even, clamped to 640..1706, i.e. 4:3..32:9) times `--scale`. `UpdateEfbSize` (TurokGame.cs) checks the back buffer every draw and rebuilds the EFB when the width changes, so window drags, `TUROK_WINDOW` resizes and F11 all apply on the next present. A `Window.ClientSizeChanged` handler keeps the back buffer at the client size. Outside 4:3..32:9 the EFB is fitted into the window with bars. `--4x3` gives 640; `--widescreen` gives a fixed 854 (16:9).
- **Remapping:** each batch keeps its guest (640x480) viewport, scissor, copy rectangle and projection. `RemapBatches` maps them to the current EFB after every new frame and after every EFB rebuild, so the frame on screen is right immediately after a resize.
  - Full-width perspective draws are stretched, with the projection's X row scaled by 640/EFB width. Pixel offsets from the centre are unchanged, so menu 3D models still line up with the 4:3 UI. The first-person weapon is one of these draws and is widened without distortion.
  - Everything else (HUD, 2D, the 240x240 foliage/shadow pass and its copy) stays 4:3, centred. EFB copies of the full width are copied wide; narrower ones are offset to the centre.
  - Orthographic draws of at most 12 vertices that span the full 640 width are stretched too, if every texture they sample is a "fill" (16x16 or smaller) or a full-width EFB copy. This covers fades, flashes and the cutscene letterbox bars, which are 12 vertices with an 8x8 black texture. Without it the bars covered only the 4:3 centre. Full-screen images (menus, FMV) are bigger textures and stay 4:3 with black sides.
  - Indirect texturing: copies keep a 640-wide guest size. The shader builds texel coordinates from the game's coord size (BP 0x30, also 640) and divides by `TexSize`, so the base mapping stays right. Distortion offsets come out up to `EFB width/640` times wider on screen. Dark Jungle's full-screen distortion pass covers the whole width with no seam.
  - View reprojection (`UpdateViewRotation`) multiplies its rotation in before the widened projection, so it carries over as is. It was not re-tested with a real mouse.
  - Screenshots (F12, `TUROK_SCREENSHOT`) save the whole wide EFB.
- **Frustum culling** (`recomp/runtime/hle_frustum.c`). `fn_80105BEC` is not culling: it builds an inverse view rotation (billboards). The culling volume comes from the camera object (the active one is at 0x802CC3B8):
  - The camera holds the viewport at +0xD8..+0xE4, pixel aspect at +0xE8 and hfov at +0xEC. vfov is derived from these by `fn_80010C30`.
  - It caches cot(hfov/2) at +0x108 and cot(vfov/2) at +0x10C, recomputed when flag 0x2 of +0x1A8 is set.
  - `fn_8000EB88` builds the side, top, bottom, near and far planes at +0x144..+0x1A0, cached under flag 0x10 with the space key at +0x1A4. They are used by the per-object test `fn_8000F8E8`: an inline fast accept, then a plane test.
  - `fn_8000F2A4` builds the four corner rays at +0x110..+0x134 (flag 0x20, key at +0x140). The scene (`fn_80107794`) turns them into its world-culling cone.
  - Both builders are wrapped to call the original with +0x108 divided by the host's factor (`core_set_aspect`, EFB width/640) and restore it afterwards. They force a rebuild when the factor changes for that camera.
  - The render projection (`fn_80004F20` uses +0xEC) and the world-to-screen mapping (`fn_8000E9DC`) are untouched. The fast accept stays 4:3, so edge objects fall through to the widened planes.
  - Check: `TUROK_CULL_SCALE=0.5` visibly removes foliage at the sides. At 32:9 with `TUROK_NARROW_CULL=1` (host passes no factor), the flowering bushes at the right edge are missing; with the default they are drawn. At 16:9/21:9 the game's bounding volumes are loose enough that little is lost either way.
- **Testing:** `TUROK_WINDOW=WxH[@frame],...` sets the window client size at start or at a frame; `full@frame` toggles fullscreen. Verified, with screenshots in `analysis/widescreen/` (not committed):
  - Level 1 at 16:9, 21:9, 32:9 and 1000x700 after a live resize.
  - An OS `SetWindowPos` resize to 1484x521 mid-game.
  - A 4:3 window toggled to fullscreen on a 2560x1440 monitor (EFB 854).
  - `--4x3` (EFB 640), the title, menus, FMV, the level-1 intro cutscene and Dark Jungle.
