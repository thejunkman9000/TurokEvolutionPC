# Turok: Evolution — native Windows port (static recompilation)

A native Windows build of **Turok: Evolution** (GameCube, NTSC-U, `GTKE51`), made by statically
recompiling the game's PowerPC executable to C and running it on a small hardware/OS runtime,
with a MonoGame (Direct3D 11) front end for graphics, audio and input.

**No game code or assets are included.** You need your own disc image; everything game-specific is
extracted from it at build and run time.

## Status

Boots through the intro movie, legal screens, title and menus into gameplay. Graphics (GX/TEV,
lighting, fog, EFB copies, indirect texturing), audio (AX DSP), controller/keyboard input and the
memory card (Dolphin-compatible GCI saves) all work. Level 1 runs at its 30 fps target with headroom.
Not every level has been played through yet — see `analysis/NOTES.md` for details and known gaps.

## Building

Requirements: Windows 10/11 x64, Python 3, Visual Studio 2022+ (or Build Tools) with
"Desktop development with C++", and the .NET 9 SDK.

```powershell
.\build.ps1 -Iso "D:\Games\Turok - Evolution NTSC-U.iso"
```

The first build takes a few minutes (about 600,000 recompiled instructions are compiled by MSVC).

## Playing

```powershell
host\TurokHost\bin\Release\net9.0-windows\TurokEvolution.exe "D:\Games\Turok - Evolution NTSC-U.iso"
```

Options: `--fullscreen`, `--scale 1..4` (internal resolution, multiples of 640x480; default 2),
`--widescreen` (16:9: the 3D view gets wider, HUD and menus stay 4:3), `--no-vsync`. If the `.iso` sits next to the executable (or a few folders above it) the path can be omitted.

Level select: `--level data/levels/<pod>/<name>/<name>.atr` makes **New Game** start that level instead of the
first jungle level, e.g. `--level data/levels/pod-07-dark-jungle/darkjungle_1/darkjungle_1.atr`.
`analysis/level_paths.txt` lists the level paths found in the game's archive.

| Action | Keyboard / mouse | Xbox controller |
|---|---|---|
| Move | W A S D | left stick |
| Look | mouse (click to capture, Esc to release), or I J K L | right stick |
| Fire (R) | left mouse button | right trigger |
| L | right mouse button / Z | left trigger |
| A / B / X / Y | Space / Shift / E / Q | A / B / X / Y |
| Z | Tab | right shoulder |
| Start | Enter | Start |
| D-pad | arrow keys | D-pad |
| Fullscreen | F11 or Alt+Enter | |
| Screenshot | F12 | |

Saves are written to `saves\GTKE51\Card A\` next to the executable, as `.gci` files that Dolphin can import.

## How it works

1. `decomp/` — a [decomp-toolkit](https://github.com/encounter/decomp-toolkit) project that splits
   `main.dol` into 10,526 functions; SDK functions were named by matching against Super Mario Sunshine
   (same SDK build). `decomp/config/GTKE51/symbols.txt` is the symbol map the recompiler uses.
2. `tools/recomp/ppcrecomp.py` turns every function into C (`recomp/generated/`, not committed).
3. `recomp/runtime/` emulates the hardware the code talks to (memory, interrupts, VI/PI/DI/EXI/AI/DSP,
   the GX command processor) and replaces a few SDK pieces with host code (PAD, CARD, AX audio).
   The GX command stream is turned into decoded draw batches + TEV materials.
4. `host/TurokHost/` renders those with a TEV ubershader, plays audio and feeds input.

`analysis/NOTES.md` is the detailed engineering log.
