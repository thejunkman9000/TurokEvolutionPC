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

### Controls

Modern PC / console FPS controls, mapped onto the game's default "FPS" layout (Options > Control Setup).
Click in the window to capture the mouse; the mouse turns the view directly, like a PC shooter
(`--mouse-sens 1.5` makes it faster).

| Action | Keyboard / mouse | Xbox controller |
|---|---|---|
| Move / strafe | W A S D (hold Shift to walk) | left stick |
| Look | mouse | right stick (up looks up) |
| Fire | left mouse button | right trigger |
| Zoom | right mouse button | left trigger |
| Jump | Space | A |
| Duck | Ctrl or C (hold) | B or left-stick click |
| Next / previous weapon | mouse wheel down / up, E / Q | RB / LB (Y = next) |
| Weapon mode (alternate ammo) | F, R or middle mouse button | X |
| Pause / resume | Esc (also frees / recaptures the mouse) | Start |
| Menus: select / back | Space / Esc or Backspace (Enter = Start) | A / B |
| Menu navigation | arrow keys or W A S D | D-pad or left stick |
| Fullscreen | F11 or Alt+Enter | |
| Screenshot | F12 | |
| Fast-forward (4x, while held) | Tab | |
| Brightness down / up | F5 / F6 | |
| Contrast down / up | F7 / F8 | |
| Gamma down / up | F9 / F10 | |
| Debug: force object fades opaque (experiment) | F2 | |
| Debug: toggle light-driven alpha fades | F3 | |
| Debug: save a capture of this frame (`captures\`) | F4 | |

The modern layout applies while you control the player; in menus and cutscenes the buttons pass
straight through (A/B/X/Y = A/B/X/Y). The game ships with *Invert Look* on, so the controller's right
stick is flipped to make up look up; `--invert-look` keeps the game's inverted behaviour (or turn
*Invert Look* off in Options > Control Setup > Advanced Options and pass `--invert-look`).

Startup and speed:
- The logos, legal screens and intro movie are skipped by default; the game starts at the main menu within a few
  seconds. `--show-intro` plays them.
- `--speed 2` runs the whole game at double speed; hold Tab to fast-forward 4x (sound is muted while sped up).

Picture: brightness, contrast and gamma (F5-F10) show their values in the title bar and are saved to
`settings.json` next to the executable. The default gamma is 1.15, a small mid-tone lift because the game was
tuned for CRT TVs.

Texture enhancements (on by default, `--original-textures` for the hardware look):
- 8x anisotropic and trilinear filtering on mipmapped textures.
- Alpha-tested foliage keeps its coverage at a distance: distant palm fronds and leaves no longer thin out or pop
  between mip levels.

Smoothness: the game runs at 60 fps. On a 60 Hz display its frames are paced by the display's refresh. On faster
displays (120/144 Hz) mouse look still turns at the full refresh rate: each refresh redraws the latest game frame
rotated by any mouse motion the game hasn't caught up with yet (only the world; the weapon and HUD stay put).

Saves are written to `saves\GTKE51\Card A\` next to the executable, as `.gci` files that Dolphin can import.
Before a save is overwritten, the previous version is kept in `saves\GTKE51\Card A\Backups\` (newest 20 per
save). To restore one, copy it back over the save and remove the date from its name.

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
