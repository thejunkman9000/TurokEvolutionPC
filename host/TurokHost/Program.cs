using System;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace TurokHost;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        // TurokEvolution.exe [disc.iso] [--fullscreen] [--scale N] [--no-vsync]
        //                    [--4x3 | --widescreen] (3D view: original 4:3, or fixed 16:9; default follows the window)
        //                    [--level data/levels/<pod>/<name>/<name>.atr]  (New Game starts that level)
        //                    [--mouse-sens X]  (mouse look speed multiplier, default 1)
        //                    [--invert-look]   (keep the game's inverted look on the pad's right stick)
        //                    [--speed X]       (game speed, e.g. 2; hold Tab to fast-forward 4x)
        //                    [--show-intro]    (play the logos and intro movie; skipped by default)
        //                    [--original-textures] (hardware texture filtering: no anisotropic/trilinear upgrade
        //                                       or distant alpha-test coverage boost)
        //                    [--cull]          (keep the game's object culling; by default everything near
        //                                       the view is drawn, so nothing pops in or out)
        string? iso = null;
        var opt = new TurokGame.Options();
        bool showIntro = false, cull = false;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
            case "--fullscreen": opt = opt with { Fullscreen = true }; break;
            case "--no-vsync": opt = opt with { VSync = false }; break;
            case "--widescreen": opt = opt with { Aspect = TurokGame.AspectMode.Wide16x9 }; break;
            case "--4x3": opt = opt with { Aspect = TurokGame.AspectMode.Original4x3 }; break;
            case "--invert-look": opt = opt with { InvertLook = true }; break;
            case "--show-intro": showIntro = true; break;
            case "--cull": cull = true; break;
            case "--original-textures": TurokGame.s_enhanceTextures = false; break;
            case "--speed" when i + 1 < args.Length && float.TryParse(args[i + 1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var sp):
                opt = opt with { Speed = sp }; i++; break;
            case "--level" when i + 1 < args.Length: Environment.SetEnvironmentVariable("TUROK_LEVEL", args[++i]); break;
            case "--mouse-sens" when i + 1 < args.Length && float.TryParse(args[i + 1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var ms):
                opt = opt with { MouseSensitivity = ms }; i++; break;
            case "--scale" when i + 1 < args.Length && int.TryParse(args[i + 1], out var sc): opt = opt with { Scale = sc }; i++; break;
            default: iso ??= args[i]; break;
            }
        }
        // Intro skip: the core swaps the boot sequence (logos, legal, intro movie) for the
        // title screen, fast-forwards it and presses Start, landing on the main menu.
        // An explicit TUROK_SKIP_INTRO (e.g. 0 for scripted test runs) wins.
        if (Environment.GetEnvironmentVariable("TUROK_SKIP_INTRO") == null)
            Environment.SetEnvironmentVariable("TUROK_SKIP_INTRO", showIntro ? "0" : "frontendbackground");
        // No-cull: the core makes the game's per-object visibility tests pass and widens its
        // scene cone (recomp/runtime/hle_frustum.c). An explicit TUROK_NOCULL wins.
        if (Environment.GetEnvironmentVariable("TUROK_NOCULL") == null)
            Environment.SetEnvironmentVariable("TUROK_NOCULL", cull ? "0" : "1");
        iso ??= FindIso();
        if (iso == null || !File.Exists(iso))
        {
            MessageBox.Show("Pass the path to your Turok: Evolution (NTSC-U) disc image, or put the .iso next to the game.",
                            "Turok: Evolution", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        using var game = new TurokGame(Path.GetFullPath(iso), opt);
        game.Run();
    }

    // Look for an .iso next to the executable or in a few parent directories.
    private static string? FindIso()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
        {
            var hit = dir.GetFiles("*.iso").FirstOrDefault();
            if (hit != null) return hit.FullName;
        }
        return null;
    }
}
