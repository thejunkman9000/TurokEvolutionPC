// MonoGame host: runs turok_core.dll and draws the frames it publishes with
// the TEV ubershader (Shaders/Tev.fx).
using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Audio;
using Microsoft.Xna.Framework.Input;

namespace TurokHost;

internal sealed class TurokGame : Game
{
    private const int EfbHeight = 480;
    // EFB width: 640 (4:3), or 854 with --widescreen. In widescreen, full-width perspective
    // draws are stretched to the wide EFB with a narrower projection (same vertical FOV,
    // more horizontally); everything else (HUD, 2D, shadow pass) stays 4:3, centered.
    private readonly int EfbWidth;
    private readonly float _wideFactor;
    private readonly int _centerOff;
    private readonly int RenderScale;  // internal resolution = 640x480 * RenderScale

    private readonly GraphicsDeviceManager _graphics;
    private readonly string _isoPath;
    private RenderTarget2D? _target;
    private SpriteBatch? _sprites;
    private Effect? _tev;
    private Effect? _copyFx;
    private BasicEffect? _clearFx;
    private Texture2D? _white;
    // EFB-to-texture copies, by copy id (persist across frames like the game's textures)
    private readonly Dictionary<uint, RenderTarget2D> _efbCopies = new();
    // Guest (EFB texel) size of each copy: the render targets are upscaled and, in widescreen, widened.
    private readonly Dictionary<uint, Vector2> _efbCopyGuestSize = new();
    private readonly Vector4[] _texSize = new Vector4[8];

    // Last frame copied out of the core (redrawn until a newer one arrives).
    private TevVertex[] _vertices = Array.Empty<TevVertex>();
    private readonly List<Batch> _batches = new();
    private readonly List<MaterialParams> _materials = new();
    private Color _clearColor = Color.Black;
    private uint _lastSerial;
    private long _framesRepeated, _framesSkipped;
    // View reprojection: mouse motion sent to the core (running totals, radians) vs the
    // amount the shown frame had consumed; the difference rotates the frame's 3D draws.
    private double _sentYaw, _sentPitch;
    private double _frameLookYaw, _frameLookPitch;
    private float _frameLookPitchDeg;
    private bool _frameLookValid;
    private Matrix _viewRot = Matrix.Identity;
    private bool _viewRotActive;
    private readonly int _batchInfoFrame = int.TryParse(Environment.GetEnvironmentVariable("TUROK_BATCHINFO"), out var bi) ? bi : -1;
    private System.Text.StringBuilder? _batchInfo;
    private readonly Vector2 _reprojTest = ParseReprojTest();  // presents that showed an old frame / game frames never shown
    private int _framesReceived;

    private readonly Dictionary<uint, (Texture2D Tex, int LastUsed)> _textures = new();

    // Audio: the core's AI DMA output, streamed through a DynamicSoundEffectInstance.
    private DynamicSoundEffectInstance? _sound;
    private int _soundRate;
    private readonly short[] _audioFrames = new short[32000 * 2];
    private const int AudioChunkFrames = 1600;  // 50 ms at 32 kHz

    private string _status = "starting";
    private bool _fullscreenKeyHeld;
    // Mouse look: click in the window to capture the mouse, Esc to release.
    private bool _mouseCaptured;
    private bool _escHeld;
    private Effect? _presentFx;
    private readonly Settings _settings = Settings.Load();
    private Keys _pictureKeyHeld;
    private string? _pictureMessage;
    private double _pictureMessageUntil;
    private readonly float _baseSpeed;
    private float _speed = 1f;  // effective game speed (Tab fast-forward, intro skip)
    private const float FastForward = 4f;
    private RawMouse? _rawMouse;
    private readonly System.Diagnostics.Stopwatch _refreshWatch = System.Diagnostics.Stopwatch.StartNew();
    private double _refreshMs = 16.7;  // smoothed display refresh interval
    private int _refreshCount;
    private int _prevWheel;
    private readonly bool _invertLook;
    private readonly List<(PadButtons button, int age)> _pulses = new();
    private bool _wasActive, _prevLeftDown, _swallowClick;
    private const float MouseRadiansPerPixel = 0.0022f;  // at --mouse-sens 1
    private readonly float _mouseRadPerPixel;
    private readonly float _debugView = float.TryParse(Environment.GetEnvironmentVariable("TUROK_DEBUG_VIEW"), out var dv) ? dv : 0;
    private readonly List<int> _screenshotFrames = new();
    private readonly List<int> _ramDumpFrames = new();
    private readonly List<(int frame, uint addr, float add)> _pokes = new();
    private bool _screenshotPending;
    private readonly List<(int Frame, PadButtons Button, int Hold)> _script = new();
    private readonly List<(int Frame, string Axis, int Hold)> _stickScript = new();

    private sealed class Batch
    {
        public Matrix Projection;
        public bool Reproject;  // full-screen perspective draw: rotated with the mouse between game frames
        public int First, Count;
        public int Material;
        public readonly Texture2D?[] Textures = new Texture2D?[8];
        public readonly SamplerState[] Samplers = new SamplerState[8];
        public Rectangle Viewport, Scissor;
        public readonly uint[] TextureIds = new uint[8];
        public uint Kind, CopyId, CopyMode, ClearColor;
        public Rectangle CopySrc;
        public Vector2 CopyGuestSize;
        public float ClearDepth;
        public readonly uint[] CopyRefs = new uint[8];  // EFB copy ids referenced per tex map (0 = none)
    }

    // A CoreMaterial converted to effect parameter arrays and state objects.
    private sealed class MaterialParams
    {
        public readonly Vector4[] ColorIn = new Vector4[16], AlphaIn = new Vector4[16];
        public readonly Vector4[] ColorOp = new Vector4[16], AlphaOp = new Vector4[16];
        public readonly Vector4[] Stage = new Vector4[16], Swap = new Vector4[16], Konst = new Vector4[16];
        public readonly Vector4[] SwapTable = new Vector4[4], Regs = new Vector4[4], MapCoord = new Vector4[2];
        public Vector4 AlphaTest, Misc, Fog, FogMode, FogColor, IndMisc;
        public readonly Vector4[] IndCmd0 = new Vector4[16], IndCmd1 = new Vector4[16], IndMtx = new Vector4[6];
        public readonly Vector4[] IndStage = new Vector4[4], CoordSize = new Vector4[8];
        public BlendState Blend = BlendState.Opaque;
        public DepthStencilState Depth = DepthStencilState.None;
        public uint Cull;
    }

    public sealed record Options(int Scale = 2, bool Fullscreen = false, bool VSync = true, bool Widescreen = false,
                                 float MouseSensitivity = 1f, bool InvertLook = false, float Speed = 1f);

    public TurokGame(string isoPath, Options? options = null)
    {
        options ??= new Options();
        _mouseRadPerPixel = MouseRadiansPerPixel * options.MouseSensitivity;
        _invertLook = options.InvertLook;
        _baseSpeed = Math.Clamp(options.Speed, 0.25f, 8f);
        _isoPath = isoPath;
        RenderScale = Math.Clamp(options.Scale, 1, 4);
        EfbWidth = options.Widescreen ? 854 : 640;
        _wideFactor = EfbWidth / 640f;
        _centerOff = (EfbWidth - 640) / 2;
        _graphics = new GraphicsDeviceManager(this)
        {
            PreferredBackBufferWidth = options.Widescreen ? 1280 : 1280,
            PreferredBackBufferHeight = options.Widescreen ? 720 : 960,
            SynchronizeWithVerticalRetrace = options.VSync,
            HardwareModeSwitch = false,  // borderless fullscreen
            IsFullScreen = options.Fullscreen,
            GraphicsProfile = GraphicsProfile.HiDef,
        };
        IsMouseVisible = true;
        Window.AllowUserResizing = true;
        // Run one update+draw per display refresh (vsync), not MonoGame's fixed 60 Hz
        // step, which drifts against the display and drops/doubles frames.
        IsFixedTimeStep = false;
        Window.Title = "Turok: Evolution (recompiled)";
        // TUROK_SCREENSHOT=120,400,...: capture those frames (exit after the last
        // one if TUROK_EXIT_AFTER_SCREENSHOT=1).
        foreach (var part in (Environment.GetEnvironmentVariable("TUROK_SCREENSHOT") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(part, out var n)) _screenshotFrames.Add(n);
        // TUROK_RAMDUMP=f1,f2,...: write guest RAM to ram_<frame>.bin at those frames (debugging).
        foreach (var part in (Environment.GetEnvironmentVariable("TUROK_RAMDUMP") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(part, out var n)) _ramDumpFrames.Add(n);
        // TUROK_POKE=frame:hexaddr:delta,...: add delta to the big-endian float at addr (debugging).
        foreach (var part in (Environment.GetEnvironmentVariable("TUROK_POKE") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var f = part.Split(':');
            if (f.Length == 3 && int.TryParse(f[0], out var pf) && uint.TryParse(f[1], System.Globalization.NumberStyles.HexNumber, null, out var pa) &&
                float.TryParse(f[2], System.Globalization.CultureInfo.InvariantCulture, out var pd))
                _pokes.Add((pf, pa, pd));
        }
        // TUROK_INPUT=frame:Button[:holdFrames],...  e.g. "150:Down:4,200:A:6".
        // Sticks: LX+ LX- LY+ LY- (main) and CX+ CX- CY+ CY- (C-stick) at full deflection.
        foreach (var part in (Environment.GetEnvironmentVariable("TUROK_INPUT") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var f = part.Split(':');
            if (f.Length < 2 || !int.TryParse(f[0], out var at)) continue;
            int hold = f.Length > 2 && int.TryParse(f[2], out var h) ? h : 6;
            if (Enum.TryParse<PadButtons>(f[1], true, out var btn)) _script.Add((at, btn, hold));
            else if (f[1].Length == 3 && "LXLYCXCY".Contains(f[1][..2].ToUpperInvariant())) _stickScript.Add((at, f[1].ToUpperInvariant(), hold));
        }
    }

    protected override void Initialize()
    {
        base.Initialize();
        _rawMouse = new RawMouse(Window.Handle);
        if (!Core.Start(_isoPath, "turok_recomp.log"))
        {
            Core.State(out var msg);
            _status = "failed to start: " + msg;
        }
    }

    protected override void LoadContent()
    {
        // PreserveContents: the EFB is unbound mid-frame for EFB-to-texture copies.
        _target = new RenderTarget2D(GraphicsDevice, EfbWidth * RenderScale, EfbHeight * RenderScale, false,
                                     SurfaceFormat.Color, DepthFormat.Depth24Stencil8, 0, RenderTargetUsage.PreserveContents);
        _sprites = new SpriteBatch(GraphicsDevice);
        using (var s = typeof(TurokGame).Assembly.GetManifestResourceStream("Tev.mgfxo")!)
        using (var ms = new MemoryStream())
        {
            s.CopyTo(ms);
            _tev = new Effect(GraphicsDevice, ms.ToArray());
        }
        using (var s = typeof(TurokGame).Assembly.GetManifestResourceStream("Copy.mgfxo")!)
        using (var ms = new MemoryStream())
        {
            s.CopyTo(ms);
            _copyFx = new Effect(GraphicsDevice, ms.ToArray());
        }
        using (var s = typeof(TurokGame).Assembly.GetManifestResourceStream("Present.mgfxo")!)
        using (var ms = new MemoryStream())
        {
            s.CopyTo(ms);
            _presentFx = new Effect(GraphicsDevice, ms.ToArray());
        }
        _clearFx = new BasicEffect(GraphicsDevice) { VertexColorEnabled = true, World = Matrix.Identity,
                                                     View = Matrix.Identity, Projection = Matrix.Identity };
        _white = new Texture2D(GraphicsDevice, 1, 1);
        _white.SetData(new[] { Color.White });
    }

    // Picture adjustments: F5/F6 brightness, F7/F8 contrast, F9/F10 gamma (down/up),
    // shown in the title bar and saved to settings.json.
    private void PictureKeys(KeyboardState kb)
    {
        (Keys key, int which, float step)[] map =
        {
            (Keys.F5, 0, -0.05f), (Keys.F6, 0, 0.05f), (Keys.F7, 1, -0.05f), (Keys.F8, 1, 0.05f),
            (Keys.F9, 2, -0.05f), (Keys.F10, 2, 0.05f),
        };
        Keys down = Keys.None;
        foreach (var (key, which, step) in map)
        {
            if (!kb.IsKeyDown(key)) continue;
            down = key;
            if (key == _pictureKeyHeld || !IsActive) break;
            if (which == 0) _settings.Brightness = Math.Clamp(_settings.Brightness + step, 0.5f, 2f);
            if (which == 1) _settings.Contrast = Math.Clamp(_settings.Contrast + step, 0.5f, 2f);
            if (which == 2) _settings.Gamma = Math.Clamp(_settings.Gamma + step, 0.5f, 2.5f);
            _settings.Save();
            _pictureMessage = $"brightness {_settings.Brightness:F2}  contrast {_settings.Contrast:F2}  gamma {_settings.Gamma:F2}";
            _pictureMessageUntil = Environment.TickCount64 + 3000;
            break;
        }
        _pictureKeyHeld = down;
    }

    private void SetMouseCapture(bool on)
    {
        _mouseCaptured = on;
        IsMouseVisible = !on;
        if (on) Mouse.SetPosition(Window.ClientBounds.Width / 2, Window.ClientBounds.Height / 2);
    }

    // Each update follows a vsync'd present. On a ~60 Hz display, tick the guest's
    // retrace once per refresh (every other refresh at ~120 Hz) so every game frame is
    // shown exactly once; otherwise the core keeps its own 59.94 Hz timer.
    private void PaceRetrace()
    {
        double ms = _refreshWatch.Elapsed.TotalMilliseconds;
        _refreshWatch.Restart();
        if (ms > 0.5 && ms < 100) _refreshMs += (ms - _refreshMs) * 0.05;
        _refreshCount++;
        if (!_graphics.SynchronizeWithVerticalRetrace || _speed != 1f) return;
        if (_refreshMs > 15.0 && _refreshMs < 18.5) Core.VSync();
        else if (_refreshMs > 7.5 && _refreshMs < 9.2 && (_refreshCount & 1) == 0) Core.VSync();
    }

    // ---- input ---------------------------------------------------------------------
    protected override void Update(GameTime gameTime)
    {
        PaceRetrace();
        // Game speed: --speed, times 4 while Tab is held (fast-forward).
        bool ff = IsActive && Keyboard.GetState().IsKeyDown(Keys.Tab);
        Core.SetSpeed(_baseSpeed * (ff ? FastForward : 1f));
        _speed = Core.GetSpeed();
        var kb = Keyboard.GetState();
        var gp = GamePad.GetState(PlayerIndex.One);
        if (kb.IsKeyDown(Keys.F12)) _screenshotPending = true;
        bool toggle = kb.IsKeyDown(Keys.F11) ||
                      (kb.IsKeyDown(Keys.Enter) && (kb.IsKeyDown(Keys.LeftAlt) || kb.IsKeyDown(Keys.RightAlt)));
        if (toggle && !_fullscreenKeyHeld && IsActive) _graphics.ToggleFullScreen();
        _fullscreenKeyHeld = toggle;
        bool altHeld = kb.IsKeyDown(Keys.LeftAlt) || kb.IsKeyDown(Keys.RightAlt);
        PictureKeys(kb);

        // Modern PC/console FPS controls mapped onto the game's default "FPS" layout
        // (Options > Control Setup): stick move/strafe, C-stick turn/look, R fire,
        // L duck, Z zoom, A jump, B next weapon, Y previous weapon, X weapon mode.
        // In first-person play the modern layout applies; in menus, cutscenes and
        // flight the buttons map straight through so the game's prompts stay right.
        bool firstPerson = Core.FirstPerson();
        var mouse = Mouse.GetState();
        PadButtons b = 0;
        void Map(bool down, PadButtons button) { if (down) b |= button; }
        bool Key(Keys k) => kb.IsKeyDown(k);
        bool Pad(Buttons btn) => gp.IsButtonDown(btn);

        // Esc: pause/resume in play (and release/recapture the mouse), back in menus.
        bool esc = Key(Keys.Escape);
        bool escPressed = esc && !_escHeld && IsActive;
        _escHeld = esc;
        if (escPressed && firstPerson) { _pulses.Add((PadButtons.Start, 0)); SetMouseCapture(!_mouseCaptured); }
        Map(esc && !firstPerson, PadButtons.B);

        Map((Key(Keys.Enter) && !altHeld) || Pad(Buttons.Start), PadButtons.Start);
        Map(Key(Keys.Up) || Pad(Buttons.DPadUp), PadButtons.Up);
        Map(Key(Keys.Down) || Pad(Buttons.DPadDown), PadButtons.Down);
        Map(Key(Keys.Left) || Pad(Buttons.DPadLeft), PadButtons.Left);
        Map(Key(Keys.Right) || Pad(Buttons.DPadRight), PadButtons.Right);
        Map(Key(Keys.Space) || Pad(Buttons.A), PadButtons.A);  // jump / select
        if (firstPerson)
        {
            // Keyboard: Ctrl/C duck, Q/E previous/next weapon, F or R weapon mode.
            // Pad: B or left-stick click duck, LB/RB previous/next weapon, Y next weapon, X weapon mode.
            bool duck = Key(Keys.LeftControl) || Key(Keys.C) || Pad(Buttons.B) || Pad(Buttons.LeftStick);
            if (duck) b |= PadButtons.L;
            Map(Key(Keys.E) || Pad(Buttons.RightShoulder) || Pad(Buttons.Y), PadButtons.B);
            Map(Key(Keys.Q) || Pad(Buttons.LeftShoulder), PadButtons.Y);
            Map(Key(Keys.F) || Key(Keys.R) || Pad(Buttons.X) || (_mouseCaptured && mouse.MiddleButton == ButtonState.Pressed), PadButtons.X);
        }
        else
        {
            Map(Key(Keys.Back) || Pad(Buttons.B), PadButtons.B);
            Map(Pad(Buttons.X), PadButtons.X);
            Map(Pad(Buttons.Y), PadButtons.Y);
            Map(Pad(Buttons.RightShoulder), PadButtons.Z);
        }

        // Movement: WASD / left stick; hold Shift to walk.
        float lx = gp.ThumbSticks.Left.X, ly = gp.ThumbSticks.Left.Y;
        if (Key(Keys.A)) lx = -1; if (Key(Keys.D)) lx = 1;
        if (Key(Keys.W)) ly = 1;  if (Key(Keys.S)) ly = -1;
        if (Key(Keys.LeftShift) || Key(Keys.RightShift)) { lx *= 0.5f; ly *= 0.5f; }

        var center = new Point(Window.ClientBounds.Width / 2, Window.ClientBounds.Height / 2);
        // Capture only on a fresh click inside the window while it already had focus, so the
        // click that brings the window to the front (or lands on another window) is ignored.
        bool leftDown = mouse.LeftButton == ButtonState.Pressed;
        bool clicked = leftDown && !_prevLeftDown;
        bool focusedBefore = _wasActive;
        _prevLeftDown = leftDown;
        _wasActive = IsActive;
        if (!_mouseCaptured && IsActive && focusedBefore && clicked &&
            new Rectangle(0, 0, Window.ClientBounds.Width, Window.ClientBounds.Height).Contains(mouse.X, mouse.Y))
        {
            SetMouseCapture(true);
            _swallowClick = true;  // the capturing click doesn't also fire
            mouse = Mouse.GetState();
        }
        if (_swallowClick && !leftDown) _swallowClick = false;
        if (_mouseCaptured && !IsActive) SetMouseCapture(false);

        // Mouse look. On foot it turns the view directly (core_mouse_look) like a PC
        // shooter; elsewhere (flight) the motion is fed to the C-stick instead.
        float cx = gp.ThumbSticks.Right.X, cy = gp.ThumbSticks.Right.Y;
        // The game's "Invert Look" defaults to on (stick up looks down); flip the pad's
        // right stick so up looks up, unless --invert-look keeps the game's behaviour.
        if (!_invertLook) cy = -cy;
        if (_mouseCaptured)
        {
            float dx = mouse.X - center.X, dy = mouse.Y - center.Y;
            Mouse.SetPosition(center.X, center.Y);  // keep the (hidden) cursor in the window
            if (_rawMouse is { Available: true }) { var (rx, ry) = _rawMouse.Take(); dx = rx; dy = ry; }
            if (firstPerson)
            {
                if (dx != 0 || dy != 0)
                {
                    float yaw = -dx * _mouseRadPerPixel, pitch = -dy * _mouseRadPerPixel;
                    Core.MouseLook(yaw, pitch);
                    _sentYaw += yaw; _sentPitch += pitch;
                }
            }
            else
            {
                float sx = dx * 0.06f, sy = dy * 0.06f * (_invertLook ? -1 : 1);
                if (Math.Abs(sx) > Math.Abs(cx)) cx = Math.Clamp(sx, -1f, 1f);
                if (Math.Abs(sy) > Math.Abs(cy)) cy = Math.Clamp(sy, -1f, 1f);
            }
            // Wheel: previous/next weapon, one press per notch.
            int wheel = mouse.ScrollWheelValue;
            int notches = (wheel - _prevWheel) / 120;
            _prevWheel = wheel;
            if (firstPerson)
                for (int i = 0; i < Math.Min(Math.Abs(notches), 4); i++)
                    _pulses.Add((notches > 0 ? PadButtons.Y : PadButtons.B, 0));
        }
        else { _prevWheel = mouse.ScrollWheelValue; _rawMouse?.Take(); }

        // Fire / zoom: left / right mouse, right / left trigger.
        bool mouseFire = _mouseCaptured && !_swallowClick && mouse.LeftButton == ButtonState.Pressed;
        bool mouseZoom = _mouseCaptured && mouse.RightButton == ButtonState.Pressed;
        float rt = Math.Max(gp.Triggers.Right, mouseFire ? 1f : 0f);
        if (rt > 0.5f) b |= PadButtons.R;
        float lt = 0;
        if (firstPerson) { if (mouseZoom || gp.Triggers.Left > 0.5f) b |= PadButtons.Z; }
        else lt = gp.Triggers.Left;
        if ((b & PadButtons.L) != 0) lt = 1;
        if (lt > 0.9f) b |= PadButtons.L;

        // Button pulses (weapon wheel, Esc pause): 4 frames down, 4 up, one after another.
        if (_pulses.Count > 0)
        {
            var (button, age) = _pulses[0];
            if (age < 4) b |= button;
            if (age >= 7) _pulses.RemoveAt(0); else _pulses[0] = (button, age + 1);
        }
        foreach (var (frame, button, hold) in _script)
            if (_framesReceived >= frame && _framesReceived < frame + hold) b |= button;
        foreach (var (frame, axis, hold) in _stickScript)
        {
            if (_framesReceived < frame || _framesReceived >= frame + hold) continue;
            float v = axis[2] == '+' ? 1 : -1;
            switch (axis[..2]) { case "LX": lx = v; break; case "LY": ly = v; break; case "CX": cx = v; break; case "CY": cy = v; break; }
        }
        static sbyte Axis(float v) => (sbyte)Math.Clamp((int)(v * 100), -100, 100);
        Core.SetPad(0, (ushort)b, Axis(lx), Axis(ly), Axis(cx), Axis(cy),
                    (byte)(lt * 255), (byte)(rt * 255));

        PumpAudio();

        var st = Core.State(out var msg);
        _status = st switch
        {
            CoreState.Failed => "core stopped: " + msg,
            CoreState.Exited => "game exited",
            CoreState.Running => $"running - frame {_framesReceived}",
            _ => _status,
        };
        Window.Title = "Turok: Evolution (recompiled) - " +
                       (_pictureMessage != null && Environment.TickCount64 < _pictureMessageUntil ? _pictureMessage : _status);
        base.Update(gameTime);
    }

    // EFB x coordinate / width in the (possibly widescreen) render target
    private int MapX(int x, bool widen) => widen ? (int)MathF.Round(x * _wideFactor) : x + _centerOff;
    private int MapW(int w, bool widen) => widen ? (int)MathF.Round(w * _wideFactor) : w;

    // ---- audio -----------------------------------------------------------------------
    private readonly Queue<short> _audioQueue = new();
    private int _audioSubmitted, _audioStarved;

    private unsafe void PumpAudio()
    {
        int rate = Core.AudioRate();
        if (_sound == null || rate != _soundRate)
        {
            _sound?.Dispose();
            _sound = new DynamicSoundEffectInstance(rate, AudioChannels.Stereo);
            _soundRate = rate;
            _sound.Play();
        }
        // Drain everything the core produced since last time.
        int n;
        do
        {
            fixed (short* p = _audioFrames) n = Core.ReadAudio(p, _audioFrames.Length / 2);
            for (int i = 0; i < 2 * n; i++) _audioQueue.Enqueue(_audioFrames[i]);
        } while (n == _audioFrames.Length / 2);

        // Fast-forward: the game produces audio faster than real time; drop it.
        if (_speed != 1f) _audioQueue.Clear();
        // If we fell behind (clock drift, a stall), drop the oldest audio to keep latency low.
        int maxSamples = rate / 4 * 2;  // 250 ms of stereo samples
        while (_audioQueue.Count > maxSamples) { _audioQueue.Dequeue(); _audioQueue.Dequeue(); }

        while (_sound.PendingBufferCount < 3 && _audioQueue.Count >= AudioChunkFrames * 2)
        {
            var bytes = new byte[AudioChunkFrames * 4];
            for (int i = 0; i < AudioChunkFrames * 2; i++)
            {
                short v = _audioQueue.Dequeue();
                bytes[2 * i] = (byte)v; bytes[2 * i + 1] = (byte)(v >> 8);
            }
            _sound.SubmitBuffer(bytes);
            _audioSubmitted++;
        }
        if (_audioSubmitted > 100 && _sound.PendingBufferCount == 0) _audioStarved++;  // steady state only
    }

    protected override void OnExiting(object sender, ExitingEventArgs args)
    {
        File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "host_stats.txt"),
            $"audio: rate {_soundRate} submitted {_audioSubmitted} buffers ({_audioSubmitted * AudioChunkFrames / (double)Math.Max(1, _soundRate):F1} s), starved updates {_audioStarved}\n" +
            $"draw: {_drawCount} calls, avg {_drawMsTotal / Math.Max(1, _drawCount):F2} ms, max {_drawMsMax:F1} ms; game frames received {_framesReceived}, repeated presents {_framesRepeated}, skipped {_framesSkipped}; refresh {_refreshMs:F2} ms\n");
        base.OnExiting(sender, args);
    }

    // ---- frames ----------------------------------------------------------------------
    private unsafe void PullFrame()
    {
        CoreFrame* f = Core.AcquireFrame();
        if (f == null) { if (_framesReceived > 0) _framesRepeated++; return; }
        try
        {
            if (f->Serial == _lastSerial) { _framesRepeated++; return; }
            if (_lastSerial != 0 && f->Serial - _lastSerial > 1) _framesSkipped += f->Serial - _lastSerial - 1;
            _lastSerial = f->Serial;
            _frameLookYaw = f->LookYaw; _frameLookPitch = f->LookPitch;
            _frameLookPitchDeg = f->LookPitchDeg; _frameLookValid = f->LookValid != 0;
            if (_batchInfo != null && _batchInfoFrame + 1 == _framesReceived)
                File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "batchinfo.txt"), _batchInfo.ToString());
            _framesReceived++;
            _clearColor = new Color(f->ClearColor);

            if (_vertices.Length < f->VertexCount) _vertices = new TevVertex[f->VertexCount * 2];
            new ReadOnlySpan<TevVertex>(f->Vertices, (int)f->VertexCount).CopyTo(_vertices);

            var textures = new Texture2D?[f->TextureCount];
            for (int i = 0; i < f->TextureCount; i++) textures[i] = GetTexture(&f->Textures[i]);

            while (_materials.Count < f->MaterialCount) _materials.Add(new MaterialParams());
            for (int i = 0; i < f->MaterialCount; i++) ConvertMaterial(&f->Materials[i], _materials[i]);

            while (_batches.Count < f->BatchCount) _batches.Add(new Batch());
            _batches.RemoveRange((int)f->BatchCount, _batches.Count - (int)f->BatchCount);
            for (int i = 0; i < f->BatchCount; i++)
            {
                CoreBatch* cb = &f->Batches[i];
                var b = _batches[i];
                float* m = cb->Proj;
                // Core matrices are row-major for column vectors; MonoGame uses row vectors.
                b.Projection = new Matrix(m[0], m[4], m[8], m[12],
                                          m[1], m[5], m[9], m[13],
                                          m[2], m[6], m[10], m[14],
                                          m[3], m[7], m[11], m[15]);
                b.First = (int)cb->FirstVertex; b.Count = (int)cb->VertexCount;
                b.Material = (int)cb->Material;
                b.Kind = cb->Kind; b.CopyId = cb->CopyId; b.CopyMode = cb->CopyMode;
                bool wideCopy = _centerOff != 0 && cb->CopySrc[0] <= 0 && cb->CopySrc[2] >= 600;
                b.CopySrc = new Rectangle(MapX(cb->CopySrc[0], wideCopy), cb->CopySrc[1], MapW(cb->CopySrc[2], wideCopy), cb->CopySrc[3]);
                b.ClearColor = cb->ClearColor; b.ClearDepth = cb->ClearDepth;
                b.CopyGuestSize = (cb->CopyMode & 0x100) != 0
                    ? new Vector2(Math.Max(1, (int)cb->CopySrc[2] / 2), Math.Max(1, (int)cb->CopySrc[3] / 2))
                    : new Vector2(cb->CopySrc[2], cb->CopySrc[3]);
                for (int t = 0; t < 8; t++)
                {
                    b.CopyRefs[t] = cb->Textures[t] >= 0 && f->Textures[cb->Textures[t]].Rgba == null ? f->Textures[cb->Textures[t]].Id : 0;
                    b.Textures[t] = cb->Textures[t] >= 0 ? textures[cb->Textures[t]] : null;
                    b.TextureIds[t] = cb->Textures[t] >= 0 ? f->Textures[cb->Textures[t]].Id : 0;
                    b.Samplers[t] = Sampler(cb->Sampler[t]);
                }
                bool perspective = m[15] == 0f;
                // World draws only: the first-person weapon is drawn in view space right in
                // front of the camera (|z| < 0.6) and must stay attached to it.
                b.Reproject = perspective && cb->Viewport[2] >= 600 && cb->Viewport[3] >= 400;
                if (b.Reproject)
                {
                    float far = 0;
                    TevVertex* vv = f->Vertices + cb->FirstVertex;
                    for (int v = 0; v < cb->VertexCount; v++) far = Math.Max(far, -vv[v].Position.Z);
                    b.Reproject = far > 2f;
                }
                if (_batchInfoFrame == _framesReceived)
                {
                    float zmin = float.MaxValue, zmax = float.MinValue;
                    for (int v = 0; v < cb->VertexCount; v++) { float z = f->Vertices[cb->FirstVertex + v].Position.Z; zmin = Math.Min(zmin, z); zmax = Math.Max(zmax, z); }
                    _batchInfo ??= new System.Text.StringBuilder();
                    _batchInfo.AppendLine($"{i} kind {cb->Kind} n {cb->VertexCount} persp {perspective} P11 {m[0]:F3} P22 {m[5]:F3} P33 {m[10]:F5} P34 {m[11]:F3} vp {cb->Viewport[0]},{cb->Viewport[1]},{cb->Viewport[2]},{cb->Viewport[3]} z {zmin:F2}..{zmax:F2}");
                }
                bool widen = _centerOff != 0 && perspective && cb->Viewport[0] <= 16 && cb->Viewport[2] >= 600;
                if (widen)
                {
                    // narrower x projection so the wider viewport isn't stretched
                    float k = 1f / _wideFactor;
                    b.Projection.M11 *= k; b.Projection.M21 *= k; b.Projection.M31 *= k; b.Projection.M41 *= k;
                }
                b.Viewport = new Rectangle(MapX(cb->Viewport[0], widen) * RenderScale, cb->Viewport[1] * RenderScale,
                                           MapW(cb->Viewport[2], widen) * RenderScale, cb->Viewport[3] * RenderScale);
                bool fullScissor = cb->Scissor[0] <= 0 && cb->Scissor[2] >= 640;
                int sx0 = widen && fullScissor ? 0 : MapX(cb->Scissor[0], widen);
                int sx1 = widen && fullScissor ? EfbWidth : MapX(cb->Scissor[2], widen);
                b.Scissor = new Rectangle(sx0 * RenderScale, cb->Scissor[1] * RenderScale,
                                          (sx1 - sx0) * RenderScale, (cb->Scissor[3] - cb->Scissor[1]) * RenderScale);
            }
        }
        finally
        {
            Core.ReleaseFrame();
        }
        PurgeTextures();
    }

    private static unsafe void Copy(float* src, Vector4[] dst)
    {
        for (int i = 0; i < dst.Length; i++)
            dst[i] = new Vector4(src[4 * i], src[4 * i + 1], src[4 * i + 2], src[4 * i + 3]);
    }

    private static unsafe void ConvertMaterial(CoreMaterial* m, MaterialParams p)
    {
        Copy(m->ColorIn, p.ColorIn); Copy(m->AlphaIn, p.AlphaIn);
        Copy(m->ColorOp, p.ColorOp); Copy(m->AlphaOp, p.AlphaOp);
        Copy(m->Stage, p.Stage); Copy(m->Swap, p.Swap); Copy(m->Konst, p.Konst);
        Copy(m->SwapTable, p.SwapTable); Copy(m->Regs, p.Regs); Copy(m->MapCoord, p.MapCoord);
        p.AlphaTest = new Vector4(m->AlphaTest[0], m->AlphaTest[1], m->AlphaTest[2], m->AlphaTest[3]);
        p.Misc = new Vector4(m->Misc[0], m->Misc[1], m->Misc[2], m->Misc[3]);
        p.Fog = new Vector4(m->Fog[0], m->Fog[1], m->Fog[2], m->Fog[3]);
        p.FogMode = new Vector4(m->FogMode[0], m->FogMode[1], m->FogMode[2], m->FogMode[3]);
        p.FogColor = new Vector4(m->FogColor[0], m->FogColor[1], m->FogColor[2], 1);
        Copy(m->IndCmd0, p.IndCmd0); Copy(m->IndCmd1, p.IndCmd1); Copy(m->IndMtx, p.IndMtx);
        Copy(m->IndStage, p.IndStage); Copy(m->CoordSize, p.CoordSize);
        p.IndMisc = new Vector4(m->IndMisc[0], m->IndMisc[1], m->IndMisc[2], m->IndMisc[3]);
        p.Blend = BlendFor(m->Blend);
        p.Depth = DepthFor(m->ZMode);
        p.Cull = m->Cull;
    }

    private unsafe Texture2D? GetTexture(CoreTexture* t)
    {
        if (t->Rgba == null) return null;  // EFB copy: resolved when drawn
        if (_textures.TryGetValue(t->Id, out var e))
        {
            _textures[t->Id] = (e.Tex, _framesReceived);
            return e.Tex;
        }
        int w = (int)t->Width, h = (int)t->Height, given = (int)Math.Max(1u, t->Levels);
        var tex = new Texture2D(GraphicsDevice, w, h, given > 1, SurfaceFormat.Color);
        uint* src = t->Rgba;
        uint[]? prev = null;
        int pw = 0, ph = 0;
        for (int l = 0; l < tex.LevelCount; l++)
        {
            int lw = Math.Max(1, w >> l), lh = Math.Max(1, h >> l);
            uint[] level;
            if (l < given)
            {
                level = new uint[lw * lh];
                new ReadOnlySpan<uint>(src, level.Length).CopyTo(level);
                src += level.Length;
            }
            else level = BoxDownsample(prev!, pw, ph, lw, lh);  // levels the game didn't supply
            tex.SetData(l, null, level, 0, level.Length);
            prev = level; pw = lw; ph = lh;
        }
        _textures[t->Id] = (tex, _framesReceived);
        return tex;
    }

    private static uint[] BoxDownsample(uint[] src, int sw, int sh, int dw, int dh)
    {
        var dst = new uint[dw * dh];
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
            {
                uint r = 0, g = 0, b = 0, a = 0;
                for (int k = 0; k < 4; k++)
                {
                    int sx = Math.Min(sw - 1, 2 * x + (k & 1)), sy = Math.Min(sh - 1, 2 * y + (k >> 1));
                    uint p = src[sy * sw + sx];
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; a += p >> 24;
                }
                dst[y * dw + x] = (r / 4) | (g / 4) << 8 | (b / 4) << 16 | (a / 4) << 24;
            }
        return dst;
    }

    private void PurgeTextures()
    {
        if (_framesReceived % 120 != 0) return;
        var stale = new List<uint>();
        foreach (var (id, e) in _textures)
            if (_framesReceived - e.LastUsed > 600) stale.Add(id);
        foreach (var id in stale) { _textures[id].Tex.Dispose(); _textures.Remove(id); }
    }

    // ---- GX -> MonoGame state ------------------------------------------------------------
    private static readonly Dictionary<uint, BlendState> BlendCache = new();
    private static readonly Dictionary<uint, DepthStencilState> DepthCache = new();
    private static readonly Dictionary<uint, SamplerState> SamplerCache = new();

    // CMODE0: bit0 blend, bit3 colour update, bit4 alpha update, dst 5-7, src 8-10, bit11 subtract
    private static BlendState BlendFor(uint cm)
    {
        uint key = cm & 0xFFF;
        if (BlendCache.TryGetValue(key, out var s)) return s;
        static Blend Src(uint f) => f switch
        {
            0 => Blend.Zero, 1 => Blend.One, 2 => Blend.DestinationColor, 3 => Blend.InverseDestinationColor,
            4 => Blend.SourceAlpha, 5 => Blend.InverseSourceAlpha, 6 => Blend.DestinationAlpha, _ => Blend.InverseDestinationAlpha,
        };
        static Blend Dst(uint f) => f switch
        {
            0 => Blend.Zero, 1 => Blend.One, 2 => Blend.SourceColor, 3 => Blend.InverseSourceColor,
            4 => Blend.SourceAlpha, 5 => Blend.InverseSourceAlpha, 6 => Blend.DestinationAlpha, _ => Blend.InverseDestinationAlpha,
        };
        var mask = ColorWriteChannels.None;
        if ((cm & 8) != 0) mask |= ColorWriteChannels.Red | ColorWriteChannels.Green | ColorWriteChannels.Blue;
        if ((cm & 16) != 0) mask |= ColorWriteChannels.Alpha;
        s = new BlendState { ColorWriteChannels = mask };
        if ((cm & 1) != 0)
        {
            if ((cm & 0x800) != 0)
            {
                s.ColorBlendFunction = s.AlphaBlendFunction = BlendFunction.ReverseSubtract;
                s.ColorSourceBlend = s.AlphaSourceBlend = Blend.One;
                s.ColorDestinationBlend = s.AlphaDestinationBlend = Blend.One;
            }
            else
            {
                s.ColorSourceBlend = s.AlphaSourceBlend = Src((cm >> 8) & 7);
                s.ColorDestinationBlend = s.AlphaDestinationBlend = Dst((cm >> 5) & 7);
            }
        }
        else
        {
            s.ColorSourceBlend = s.AlphaSourceBlend = Blend.One;
            s.ColorDestinationBlend = s.AlphaDestinationBlend = Blend.Zero;
        }
        BlendCache[key] = s;
        return s;
    }

    // ZMODE: bit0 enable, func 1-3, bit4 update
    private static DepthStencilState DepthFor(uint z)
    {
        uint key = z & 0x1F;
        if (DepthCache.TryGetValue(key, out var s)) return s;
        bool enable = (z & 1) != 0;
        var func = ((z >> 1) & 7) switch
        {
            0 => CompareFunction.Never, 1 => CompareFunction.Less, 2 => CompareFunction.Equal, 3 => CompareFunction.LessEqual,
            4 => CompareFunction.Greater, 5 => CompareFunction.NotEqual, 6 => CompareFunction.GreaterEqual, _ => CompareFunction.Always,
        };
        s = new DepthStencilState
        {
            DepthBufferEnable = enable,
            DepthBufferWriteEnable = enable && (z & 16) != 0,
            DepthBufferFunction = func,
        };
        DepthCache[key] = s;
        return s;
    }

    // TX_SETMODE0: wrap s 0-1, wrap t 2-3 (0 clamp, 1 repeat, 2 mirror), bit4 linear mag filter,
    // min filter 5-7 (0 near, 1 near-mip-near, 2 near-mip-lin, 4 lin, 5 lin-mip-near, 6 lin-mip-lin),
    // LOD bias 9-16 (signed, 1/32 units)
    internal static bool s_enhanceTextures = true;

    private static SamplerState Sampler(uint mode)
    {
        uint key = mode & 0x1FFFF;
        if (SamplerCache.TryGetValue(key, out var s)) return s;
        static TextureAddressMode Wrap(uint w) => w switch
        {
            1 => TextureAddressMode.Wrap, 2 => TextureAddressMode.Mirror, _ => TextureAddressMode.Clamp,
        };
        bool mag = (mode & 16) != 0;
        uint minf = (mode >> 5) & 7;
        // TX_SETMODE0 min filter: 0 near, 1 near-mip-near, 2 near-mip-lin, 4 lin, 5 lin-mip-near, 6 lin-mip-lin
        bool min = minf >= 4, mip = minf == 2 || minf == 6;
        var filter = (min, mag, mip) switch
        {
            (true, true, true) => TextureFilter.Linear,
            (true, true, false) => TextureFilter.LinearMipPoint,
            (false, false, true) => TextureFilter.PointMipLinear,
            (false, false, false) => TextureFilter.Point,
            (true, false, true) => TextureFilter.MinLinearMagPointMipLinear,
            (true, false, false) => TextureFilter.MinLinearMagPointMipPoint,
            (false, true, true) => TextureFilter.MinPointMagLinearMipLinear,
            _ => TextureFilter.MinPointMagLinearMipPoint,
        };
        // Enhancement (default; --original-textures turns it off): linear-minified mipmapped
        // textures get trilinear + 8x anisotropic filtering, so distant surfaces stay sharp
        // and mip transitions blend instead of popping.
        bool upgrade = s_enhanceTextures && min && (minf == 5 || minf == 6);
        s = new SamplerState
        {
            AddressU = Wrap(mode & 3),
            AddressV = Wrap((mode >> 2) & 3),
            Filter = upgrade ? TextureFilter.Anisotropic : filter,
            MaxAnisotropy = upgrade ? 8 : 4,
            MipMapLevelOfDetailBias = (sbyte)((mode >> 9) & 0xFF) / 32f,
        };
        SamplerCache[key] = s;
        return s;
    }

    // ---- EFB -> texture copies ------------------------------------------------------------
    private static readonly DepthStencilState ClearDepthState = new()
    {
        DepthBufferEnable = true, DepthBufferWriteEnable = true, DepthBufferFunction = CompareFunction.Always,
    };

    private void EfbCopy(Batch b)
    {
        var gd = GraphicsDevice;
        var src = new Rectangle(b.CopySrc.X * RenderScale, b.CopySrc.Y * RenderScale,
                                b.CopySrc.Width * RenderScale, b.CopySrc.Height * RenderScale);
        src = Rectangle.Intersect(src, _target!.Bounds);
        if (src.Width <= 0 || src.Height <= 0) return;
        bool half = (b.CopyMode & 0x100) != 0;
        int w = half ? Math.Max(1, src.Width / 2) : src.Width, h = half ? Math.Max(1, src.Height / 2) : src.Height;
        if (!_efbCopies.TryGetValue(b.CopyId, out var rt) || rt.Width != w || rt.Height != h)
        {
            rt?.Dispose();
            rt = new RenderTarget2D(gd, w, h, false, SurfaceFormat.Color, DepthFormat.None);
            _efbCopies[b.CopyId] = rt;
        }
        _efbCopyGuestSize[b.CopyId] = b.CopyGuestSize;

        gd.SetRenderTarget(rt);
        _copyFx!.Parameters["Mode"].SetValue((float)(b.CopyMode & 0xFF));
        _sprites!.Begin(SpriteSortMode.Immediate, BlendState.Opaque, SamplerState.LinearClamp,
                        DepthStencilState.None, RasterizerState.CullNone, _copyFx);
        _sprites.Draw(_target, new Rectangle(0, 0, w, h), src, Color.White);
        _sprites.End();
        gd.SetRenderTarget(_target);

        if ((b.CopyMode & 0x200) != 0)
        {
            // Clear the copied rectangle to the clear colour / depth.
            gd.Viewport = new Viewport(src.X, src.Y, src.Width, src.Height, 0f, 1f);
            gd.ScissorRectangle = src;
            gd.RasterizerState = Raster;
            gd.BlendState = BlendState.Opaque;
            gd.DepthStencilState = ClearDepthState;
            var c = new Color(b.ClearColor);
            float z = b.ClearDepth;
            var quad = new[]
            {
                new VertexPositionColor(new Vector3(-1, -1, z), c), new VertexPositionColor(new Vector3(-1, 1, z), c),
                new VertexPositionColor(new Vector3(1, -1, z), c), new VertexPositionColor(new Vector3(1, 1, z), c),
            };
            _clearFx!.CurrentTechnique.Passes[0].Apply();
            gd.DrawUserPrimitives(PrimitiveType.TriangleStrip, quad, 0, 2);
        }
    }

    // ---- rendering ---------------------------------------------------------------------
    private static readonly RasterizerState Raster = new()
    {
        CullMode = CullMode.None,
        ScissorTestEnable = true,
    };
    // GX GENMODE cull (hardware encoding): 0 none, 1 cull counter-clockwise, 2 cull clockwise
    // (as seen in our D3D clip space), 3 all. Verified on the title scene: with 2 mapped the
    // other way, palm trunks and cliffs lose their front faces.
    // TUROK_CULL_FLIP=1 swaps the two windings (for re-verification).
    private static readonly bool CullFlip = Environment.GetEnvironmentVariable("TUROK_CULL_FLIP") == "1";
    private static readonly RasterizerState[] CullStates =
    {
        Raster,
        new() { CullMode = CullFlip ? CullMode.CullClockwiseFace : CullMode.CullCounterClockwiseFace, ScissorTestEnable = true },
        new() { CullMode = CullFlip ? CullMode.CullCounterClockwiseFace : CullMode.CullClockwiseFace, ScissorTestEnable = true },
    };

    private readonly System.Diagnostics.Stopwatch _drawWatch = new();
    private double _drawMsTotal, _drawMsMax;
    private int _drawCount;

    protected override void Draw(GameTime gameTime)
    {
        _drawWatch.Restart();
        DrawFrame();
        // Measure GPU-submission cost of our own drawing (excludes vsync wait in Present).
        double ms = _drawWatch.Elapsed.TotalMilliseconds;
        _drawMsTotal += ms; _drawMsMax = Math.Max(_drawMsMax, ms); _drawCount++;
        base.Draw(gameTime);
    }

    // Rotation from the shown frame's camera to where the mouse has turned since: yaw
    // about world up, then pitch, in GX view space (x right, y up, looking down -z).
    private void UpdateViewRotation()
    {
        _viewRotActive = false;
        if (!_frameLookValid || !Core.FirstPerson()) return;
        const float Limit = 70f * MathF.PI / 180f;
        float dy = Math.Clamp((float)(_sentYaw - _frameLookYaw), -0.15f, 0.15f) + _reprojTest.X;
        float p0 = _frameLookPitchDeg * MathF.PI / 180f;
        float p1 = Math.Clamp(p0 + Math.Clamp((float)(_sentPitch - _frameLookPitch), -0.15f, 0.15f) + _reprojTest.Y, -Limit, Limit);
        if (MathF.Abs(dy) < 1e-5f && MathF.Abs(p1 - p0) < 1e-5f) return;
        // Column-vector form M = Rx(-p1) * Ry(-dy) * Rx(p0); MonoGame multiplies row
        // vectors, so the matrix passed on is its transpose.
        static float[,] Rx(float t) { float c = MathF.Cos(t), s = MathF.Sin(t); return new float[,] { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } }; }
        static float[,] Ry(float t) { float c = MathF.Cos(t), s = MathF.Sin(t); return new float[,] { { c, 0, s }, { 0, 1, 0 }, { -s, 0, c } }; }
        static float[,] Mul(float[,] a, float[,] b)
        {
            var r = new float[3, 3];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) r[i, j] += a[i, k] * b[k, j];
            return r;
        }
        var m = Mul(Mul(Rx(-p1), Ry(-dy)), Rx(p0));
        _viewRot = new Matrix(m[0, 0], m[1, 0], m[2, 0], 0,
                              m[0, 1], m[1, 1], m[2, 1], 0,
                              m[0, 2], m[1, 2], m[2, 2], 0,
                              0, 0, 0, 1);
        _viewRotActive = true;
    }

    // TUROK_REPROJ_TEST="yaw,pitch": constant extra rotation (radians) to check the
    // reprojection's direction against a real turn of the same size.
    private static Vector2 ParseReprojTest()
    {
        var e = Environment.GetEnvironmentVariable("TUROK_REPROJ_TEST");
        if (string.IsNullOrEmpty(e)) return Vector2.Zero;
        var f = e.Split(',');
        float.TryParse(f[0], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var y);
        float p = 0;
        if (f.Length > 1) float.TryParse(f[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out p);
        return new Vector2(y, p);
    }

    private void DrawFrame()
    {
        PullFrame();
        UpdateViewRotation();
        var gd = GraphicsDevice;
        gd.SetRenderTarget(_target);
        gd.Clear(ClearOptions.Target | ClearOptions.DepthBuffer, _clearColor, 1f, 0);
        gd.RasterizerState = Raster;

        var fx = _tev!;
        var P = fx.Parameters;
        foreach (var b in _batches)
        {
            if (b.Kind == 1) { EfbCopy(b); continue; }
            if (b.Count == 0 || b.Viewport.Width <= 0 || b.Viewport.Height <= 0) continue;
            var m = _materials[b.Material];
            if (m.Cull == 3) continue;  // cull all
            gd.RasterizerState = CullStates[m.Cull];
            gd.Viewport = new Viewport(b.Viewport.X, b.Viewport.Y, b.Viewport.Width, b.Viewport.Height, 0f, 1f);
            gd.ScissorRectangle = Rectangle.Intersect(b.Scissor, _target!.Bounds);
            // debug view 6: indirect draws ignore depth, so occluded ones (water) can be inspected
            gd.DepthStencilState = _debugView == 6 && m.IndMisc.Y != 0 ? DepthStencilState.None : m.Depth;
            gd.BlendState = m.Blend;

            P["Proj"].SetValue(_viewRotActive && b.Reproject ? _viewRot * b.Projection : b.Projection);
            P["ColorIn"].SetValue(m.ColorIn); P["AlphaIn"].SetValue(m.AlphaIn);
            P["ColorOp"].SetValue(m.ColorOp); P["AlphaOp"].SetValue(m.AlphaOp);
            P["Stage"].SetValue(m.Stage); P["Swap"].SetValue(m.Swap); P["Konst"].SetValue(m.Konst);
            P["SwapTable"].SetValue(m.SwapTable); P["Regs"].SetValue(m.Regs); P["MapCoord"].SetValue(m.MapCoord);
            P["AlphaTest"].SetValue(m.AlphaTest);
            P["Enhance"].SetValue(new Vector4(s_enhanceTextures ? 0.3f : 0f, 0, 0, 0));
            P["Fog"].SetValue(m.Fog); P["FogMode"].SetValue(m.FogMode); P["FogColor"].SetValue(m.FogColor);
            P["IndMisc"].SetValue(m.IndMisc);
            if (m.IndMisc.Y != 0)
            {
                P["IndCmd0"].SetValue(m.IndCmd0); P["IndCmd1"].SetValue(m.IndCmd1); P["IndMtx"].SetValue(m.IndMtx);
                P["IndStage"].SetValue(m.IndStage); P["CoordSize"].SetValue(m.CoordSize);
            }
            P["Misc"].SetValue(new Vector4(m.Misc.X, m.Misc.Y, _debugView, 0));
            for (int t = 0; t < 8; t++)
            {
                Texture2D? tex = b.Textures[t];
                if (b.CopyRefs[t] != 0) tex = _efbCopies.TryGetValue(b.CopyRefs[t], out var rt) ? rt : null;
                var bound = tex ?? _white;
                P["Tex" + t].SetValue(bound);
                // Guest texel size (indirect stages address in texels), not the upscaled copy target's size.
                var size = new Vector2(bound.Width, bound.Height);
                if (b.CopyRefs[t] != 0 && tex != null && _efbCopyGuestSize.TryGetValue(b.CopyRefs[t], out var g)) size = g;
                _texSize[t] = new Vector4(size.X, size.Y, tex is Texture2D t2 ? t2.LevelCount : 1, 0);
            }
            P["TexSize"].SetValue(_texSize);

            fx.CurrentTechnique.Passes[0].Apply();
            for (int t = 0; t < 8; t++) gd.SamplerStates[t] = b.Samplers[t];
            gd.DrawUserPrimitives(PrimitiveType.TriangleList, _vertices, b.First, b.Count / 3, TevVertex.Declaration);
        }

        gd.SetRenderTarget(null);
        gd.Viewport = new Viewport(0, 0, gd.PresentationParameters.BackBufferWidth, gd.PresentationParameters.BackBufferHeight);
        gd.Clear(Color.Black);
        // Letterbox the 4:3 EFB into the window.
        var bb = gd.Viewport.Bounds;
        float scale = Math.Min(bb.Width / (float)_target!.Width, bb.Height / (float)_target.Height);
        int w = (int)(_target.Width * scale), h = (int)(_target.Height * scale);
        var pp = _presentFx!.Parameters;
        pp["Brightness"].SetValue(_settings.Brightness);
        pp["Contrast"].SetValue(_settings.Contrast);
        pp["Gamma"].SetValue(_settings.Gamma);
        _sprites!.Begin(samplerState: SamplerState.LinearClamp, effect: _presentFx);
        _sprites.Draw(_target, new Rectangle((bb.Width - w) / 2, (bb.Height - h) / 2, w, h), Color.White);
        _sprites.End();

        if (Environment.GetEnvironmentVariable("TUROK_GXDUMP") == _framesReceived.ToString()) Core.RequestGxDump();
        if (Environment.GetEnvironmentVariable("TUROK_DUMP_TEXTURES") == _framesReceived.ToString())
        {
            var dir = Path.Combine(AppContext.BaseDirectory, $"textures_{_framesReceived:D5}");
            Directory.CreateDirectory(dir);
            foreach (var (id, e) in _textures)
                if (e.LastUsed == _framesReceived)
                    using (var fs = File.Create(Path.Combine(dir, $"tex_{id:D5}.png"))) e.Tex.SaveAsPng(fs, e.Tex.Width, e.Tex.Height);
            using var bw = new StreamWriter(Path.Combine(dir, "batches.txt"));
            foreach (var b in _batches)
            {
                var m = _materials[b.Material];
                var v = _vertices[b.First];
                bw.WriteLine($"verts {b.Count} stages {m.Misc.Y} maps [{string.Join(",", b.TextureIds)}] stage0 map {m.Stage[0].X} coord {m.MapCoord[0]} " +
                            $"stage1 map {m.Stage[1].X} uv0 ({v.T0.X:F2},{v.T0.Y:F2}) uv1 ({v.T1.X:F2},{v.T1.Y:F2}) c0 {v.Color0} c1 {v.Color1}" +
                            (m.IndMisc.Y != 0 ? $" | ind n {m.IndMisc.X} stage0 {m.IndStage[0]} cmd0 {m.IndCmd0[0]}/{m.IndCmd1[0]} cmd1 {m.IndCmd0[1]}/{m.IndCmd1[1]} " +
                             $"mtx {m.IndMtx[0]} {m.IndMtx[1]} size {m.CoordSize[0]} copies [{string.Join(",", b.CopyRefs)}] vp {b.Viewport}" : ""));
            }
        }
        if (_ramDumpFrames.Remove(_framesReceived)) DumpRam(_framesReceived);
        foreach (var p in _pokes) if (p.frame == _framesReceived) PokeAdd(p.addr, p.add);
        bool scheduled = _screenshotFrames.Remove(_framesReceived);
        if (_screenshotPending || scheduled)
        {
            _screenshotPending = false;
            var path = Path.Combine(AppContext.BaseDirectory, $"screenshot_{_framesReceived:D5}.png");
            using (var fs = File.Create(path)) _target.SaveAsPng(fs, _target.Width, _target.Height);
            if (scheduled && _screenshotFrames.Count == 0 && Environment.GetEnvironmentVariable("TUROK_EXIT_AFTER_SCREENSHOT") == "1")
                Exit();
        }
    }

    private static unsafe void DumpRam(int frame)
    {
        byte* ram = Core.Ram(out uint size);
        using var fs = File.Create(Path.Combine(AppContext.BaseDirectory, $"ram_{frame:D5}.bin"));
        fs.Write(new ReadOnlySpan<byte>(ram, (int)size));
    }

    private static unsafe void PokeAdd(uint addr, float add)
    {
        byte* p = Core.Ram(out _) + (addr & 0x01FFFFFF);
        uint w = (uint)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
        float v = BitConverter.UInt32BitsToSingle(w) + add;
        w = BitConverter.SingleToUInt32Bits(v);
        p[0] = (byte)(w >> 24); p[1] = (byte)(w >> 16); p[2] = (byte)(w >> 8); p[3] = (byte)w;
    }
}
