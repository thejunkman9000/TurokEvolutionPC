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
        string? iso = null;
        var opt = new TurokGame.Options();
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
            case "--fullscreen": opt = opt with { Fullscreen = true }; break;
            case "--no-vsync": opt = opt with { VSync = false }; break;
            case "--scale" when i + 1 < args.Length && int.TryParse(args[i + 1], out var sc): opt = opt with { Scale = sc }; i++; break;
            default: iso ??= args[i]; break;
            }
        }
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
