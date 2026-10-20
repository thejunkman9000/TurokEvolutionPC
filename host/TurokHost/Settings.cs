using System;
using System.IO;
using System.Text.Json;

namespace TurokHost;

// Player settings remembered between runs (settings.json next to the executable).
internal sealed class Settings
{
    public float Brightness { get; set; } = 1.0f;
    public float Contrast { get; set; } = 1.0f;
    public float Gamma { get; set; } = 1.15f;  // the GameCube was tuned for CRT TVs; lift mid-tones a little

    private static string FilePath => Path.Combine(AppContext.BaseDirectory, "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath)) return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch (Exception) { }  // unreadable: defaults
        return new Settings();
    }

    public void Save()
    {
        try { File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true })); }
        catch (Exception) { }
    }
}
