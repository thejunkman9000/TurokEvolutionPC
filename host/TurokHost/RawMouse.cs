using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace TurokHost;

// Raw mouse motion (WM_INPUT) for mouse look. Unlike reading the cursor and
// re-centring it every update, raw input reports every device count (no pointer
// acceleration, nothing lost when the cursor is warped) and accumulates between
// updates, so each game frame gets exactly the motion that happened since the last.
internal sealed class RawMouse : NativeWindow, IDisposable
{
    private const int WM_INPUT = 0x00FF;
    private const uint RID_INPUT = 0x10000003;
    private const uint RIM_TYPEMOUSE = 0;
    private const ushort MOUSE_MOVE_ABSOLUTE = 1;

    [StructLayout(LayoutKind.Sequential)]
    private struct RAWINPUTDEVICE { public ushort UsagePage, Usage; public uint Flags; public IntPtr Target; }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint size);

    [DllImport("user32.dll")]
    private static extern unsafe uint GetRawInputData(IntPtr hRawInput, uint command, void* data, ref uint size, uint headerSize);

    private long _dx, _dy;
    public bool Available { get; }

    public RawMouse(IntPtr window)
    {
        AssignHandle(window);
        var dev = new[] { new RAWINPUTDEVICE { UsagePage = 0x01, Usage = 0x02, Flags = 0, Target = window } };
        Available = RegisterRawInputDevices(dev, 1, (uint)Marshal.SizeOf<RAWINPUTDEVICE>());
    }

    // Motion since the last call, in device counts.
    public (int dx, int dy) Take() => ((int)Interlocked.Exchange(ref _dx, 0), (int)Interlocked.Exchange(ref _dy, 0));

    protected override unsafe void WndProc(ref Message m)
    {
        if (m.Msg == WM_INPUT)
        {
            // RAWINPUTHEADER (16 + 2 pointers) followed by RAWMOUSE.
            int headerSize = 8 + 2 * IntPtr.Size;
            byte* buf = stackalloc byte[64];
            uint size = 64;
            if (GetRawInputData(m.LParam, RID_INPUT, buf, ref size, (uint)headerSize) != unchecked((uint)-1) &&
                *(uint*)buf == RIM_TYPEMOUSE)
            {
                ushort flags = *(ushort*)(buf + headerSize);
                if ((flags & MOUSE_MOVE_ABSOLUTE) == 0)
                {
                    Interlocked.Add(ref _dx, *(int*)(buf + headerSize + 12));
                    Interlocked.Add(ref _dy, *(int*)(buf + headerSize + 16));
                }
            }
        }
        base.WndProc(ref m);
    }

    public void Dispose() => ReleaseHandle();
}
