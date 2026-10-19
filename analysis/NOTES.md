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
    - Keys: WASD = stick, IJKL = C-stick, Enter = Start, Space = A, Shift = B, E = X, Q = Y, Tab = Z, Z / left mouse = L / R, arrows = D-pad.
  - Debug environment variables:
    - `TUROK_INPUT=frame:Button[:hold],...`: scripted input.
    - `TUROK_SCREENSHOT=f1,f2,...`: capture those frames.
    - `TUROK_GXDUMP=frame`: log every draw of that frame.
    - `TUROK_DUMP_TEXTURES=frame`: write that frame's textures plus `batches.txt`.
    - `TUROK_DEBUG_VIEW=1|2|3|4`: stage-0 texture, UV, lit color, or fog amount.
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
    - Each voice runs per 1 ms: parameter-block updates → accelerator (ADPCM / PCM8 / PCM16 from ARAM, with loop/end handling) → linear resampling → volume envelope → low-pass filter → mixing into 9 buses with ramps.
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
- Known gaps:
  - Indirect texturing (bump), lines and points.
  - Z-buffer EFB copies.
  - Range-adjusted fog.
  - Audio: no polyphase resampling (it needs the DSP ROM coefficients; linear is used instead), no initial time delay (ITD), and no DVD streaming audio (AI streaming is untested).
  - TEV math is float, not 8-bit integer.
- GX bit layouts that bit me:
  - Clear color: AR = red bits 0-7, alpha bits 8-15. GB = blue bits 0-7, green bits 8-15.
  - Scissor: y in bits 0-10, x in bits 12-22.
  - TLUT and texture addresses need masking to 0x01FFFFFF.

## Next steps
1. ~~dtk setup~~ and ~~SDK naming~~ are done (see above).
2. Make the region 0x80146a00–0x80253ea0 a priority: work out where MSL ends and game code resumes.
3. Build the recompiler: lift PPC to C, one function per symbol, with paired-single (Gekko) ops handled.
4. Write the HLE layer to replace the SDK region: GX → D3D11/Vulkan, DVD → `supertree0.tre` and file reads, PAD → XInput, AI/AX → WASAPI.
