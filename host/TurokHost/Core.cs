// P/Invoke bindings for turok_core.dll. Keep in sync with recomp/runtime/core_api.h.
using System.Runtime.InteropServices;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

namespace TurokHost;

internal enum CoreState { NotStarted = 0, Starting = 1, Running = 2, Failed = 3, Exited = 4 }

// Same memory layout as CoreVertex in core_api.h (84 bytes), so frames are bulk-copied.
[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct TevVertex : IVertexType
{
    public Vector3 Position;
    public Color Color0, Color1;
    public Vector2 T0, T1, T2, T3, T4, T5, T6, T7;

    public static readonly VertexDeclaration Declaration = new(
        new VertexElement(0, VertexElementFormat.Vector3, VertexElementUsage.Position, 0),
        new VertexElement(12, VertexElementFormat.Color, VertexElementUsage.Color, 0),
        new VertexElement(16, VertexElementFormat.Color, VertexElementUsage.Color, 1),
        new VertexElement(20, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 0),
        new VertexElement(28, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 1),
        new VertexElement(36, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 2),
        new VertexElement(44, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 3),
        new VertexElement(52, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 4),
        new VertexElement(60, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 5),
        new VertexElement(68, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 6),
        new VertexElement(76, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 7));

    VertexDeclaration IVertexType.VertexDeclaration => Declaration;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct CoreMaterial
{
    public fixed float ColorIn[64], AlphaIn[64], ColorOp[64], AlphaOp[64];
    public fixed float Stage[64], Swap[64], Konst[64];
    public fixed float SwapTable[16], Regs[16], MapCoord[8];
    public fixed float AlphaTest[4], Misc[4];
    public uint Blend, ZMode, Cull, Reserved;
    public fixed float Fog[4], FogMode[4], FogColor[4];
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct CoreBatch
{
    public fixed float Proj[16];   // row-major, column-vector convention
    public uint FirstVertex, VertexCount;
    public uint Material;
    public fixed int Textures[8];
    public fixed uint Sampler[8];  // raw TX_SETMODE0 per tex map
    public fixed int Viewport[4];  // x, y, w, h (EFB pixels)
    public fixed int Scissor[4];   // x0, y0, x1, y1
    public uint Kind;              // 0 draw, 1 EFB-to-texture copy
    public uint CopyId, CopyMode;  // CopyMode: CORE_COPY_* | 0x100 half | 0x200 clear
    public fixed int CopySrc[4];   // x, y, w, h (EFB pixels)
    public uint ClearColor;
    public float ClearDepth;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct CoreTexture
{
    public uint Id, Width, Height, Levels;
    public uint* Rgba;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal unsafe struct CoreFrame
{
    public uint Serial, EfbWidth, EfbHeight, ClearColor;
    public uint BatchCount, VertexCount, TextureCount, MaterialCount;
    public CoreBatch* Batches;
    public TevVertex* Vertices;
    public CoreTexture* Textures;
    public CoreMaterial* Materials;
}

[System.Flags]
internal enum PadButtons : ushort
{
    Left = 0x0001, Right = 0x0002, Down = 0x0004, Up = 0x0008,
    Z = 0x0010, R = 0x0020, L = 0x0040,
    A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800, Start = 0x1000,
}

internal static unsafe partial class Core
{
    private const string Dll = "turok_core";

    [LibraryImport(Dll, EntryPoint = "core_start", StringMarshalling = StringMarshalling.Utf8)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static partial bool Start(string isoPath, string logPath);

    [LibraryImport(Dll, EntryPoint = "core_state")]
    private static partial int StateRaw(byte* msg, int msgLen);

    public static CoreState State(out string message)
    {
        byte* buf = stackalloc byte[512];
        var st = (CoreState)StateRaw(buf, 512);
        message = Marshal.PtrToStringUTF8((nint)buf) ?? "";
        return st;
    }

    [LibraryImport(Dll, EntryPoint = "core_acquire_frame")]
    public static partial CoreFrame* AcquireFrame();

    [LibraryImport(Dll, EntryPoint = "core_release_frame")]
    public static partial void ReleaseFrame();

    [LibraryImport(Dll, EntryPoint = "core_audio_rate")]
    public static partial int AudioRate();

    [LibraryImport(Dll, EntryPoint = "core_read_audio")]
    public static partial int ReadAudio(short* dst, int maxFrames);

    [LibraryImport(Dll, EntryPoint = "core_request_gx_dump")]
    public static partial void RequestGxDump();

    [LibraryImport(Dll, EntryPoint = "core_set_pad")]
    public static partial void SetPad(int port, ushort buttons, sbyte stickX, sbyte stickY,
                                      sbyte cstickX, sbyte cstickY, byte triggerL, byte triggerR);
}
