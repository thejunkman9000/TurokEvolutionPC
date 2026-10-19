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
    private const int EfbWidth = 640, EfbHeight = 480;
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
    private readonly Vector4[] _texSize = new Vector4[8];

    // Last frame copied out of the core (redrawn until a newer one arrives).
    private TevVertex[] _vertices = Array.Empty<TevVertex>();
    private readonly List<Batch> _batches = new();
    private readonly List<MaterialParams> _materials = new();
    private Color _clearColor = Color.Black;
    private uint _lastSerial;
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
    private const float MouseSensitivity = 0.06f;  // stick deflection per pixel per update
    private readonly float _debugView = float.TryParse(Environment.GetEnvironmentVariable("TUROK_DEBUG_VIEW"), out var dv) ? dv : 0;
    private readonly List<int> _screenshotFrames = new();
    private bool _screenshotPending;
    private readonly List<(int Frame, PadButtons Button, int Hold)> _script = new();
    private readonly List<(int Frame, string Axis, int Hold)> _stickScript = new();

    private sealed class Batch
    {
        public Matrix Projection;
        public int First, Count;
        public int Material;
        public readonly Texture2D?[] Textures = new Texture2D?[8];
        public readonly SamplerState[] Samplers = new SamplerState[8];
        public Rectangle Viewport, Scissor;
        public readonly uint[] TextureIds = new uint[8];
        public uint Kind, CopyId, CopyMode, ClearColor;
        public Rectangle CopySrc;
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

    public sealed record Options(int Scale = 2, bool Fullscreen = false, bool VSync = true);

    public TurokGame(string isoPath, Options? options = null)
    {
        options ??= new Options();
        _isoPath = isoPath;
        RenderScale = Math.Clamp(options.Scale, 1, 4);
        _graphics = new GraphicsDeviceManager(this)
        {
            PreferredBackBufferWidth = 1280,
            PreferredBackBufferHeight = 960,
            SynchronizeWithVerticalRetrace = options.VSync,
            HardwareModeSwitch = false,  // borderless fullscreen
            IsFullScreen = options.Fullscreen,
            GraphicsProfile = GraphicsProfile.HiDef,
        };
        IsMouseVisible = true;
        Window.AllowUserResizing = true;
        Window.Title = "Turok: Evolution (recompiled)";
        // TUROK_SCREENSHOT=120,400,...: capture those frames (exit after the last
        // one if TUROK_EXIT_AFTER_SCREENSHOT=1).
        foreach (var part in (Environment.GetEnvironmentVariable("TUROK_SCREENSHOT") ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(part, out var n)) _screenshotFrames.Add(n);
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
        _clearFx = new BasicEffect(GraphicsDevice) { VertexColorEnabled = true, World = Matrix.Identity,
                                                     View = Matrix.Identity, Projection = Matrix.Identity };
        _white = new Texture2D(GraphicsDevice, 1, 1);
        _white.SetData(new[] { Color.White });
    }

    // ---- input ---------------------------------------------------------------------
    protected override void Update(GameTime gameTime)
    {
        var kb = Keyboard.GetState();
        var gp = GamePad.GetState(PlayerIndex.One);
        if (kb.IsKeyDown(Keys.F12)) _screenshotPending = true;
        bool toggle = kb.IsKeyDown(Keys.F11) ||
                      (kb.IsKeyDown(Keys.Enter) && (kb.IsKeyDown(Keys.LeftAlt) || kb.IsKeyDown(Keys.RightAlt)));
        if (toggle && !_fullscreenKeyHeld && IsActive) _graphics.ToggleFullScreen();
        _fullscreenKeyHeld = toggle;
        bool altHeld = kb.IsKeyDown(Keys.LeftAlt) || kb.IsKeyDown(Keys.RightAlt);

        PadButtons b = 0;
        void Map(bool down, PadButtons button) { if (down) b |= button; }
        Map((kb.IsKeyDown(Keys.Enter) && !altHeld) || gp.Buttons.Start == ButtonState.Pressed, PadButtons.Start);
        Map(kb.IsKeyDown(Keys.Space) || gp.Buttons.A == ButtonState.Pressed, PadButtons.A);
        Map(kb.IsKeyDown(Keys.LeftShift) || gp.Buttons.B == ButtonState.Pressed, PadButtons.B);
        Map(kb.IsKeyDown(Keys.E) || gp.Buttons.X == ButtonState.Pressed, PadButtons.X);
        Map(kb.IsKeyDown(Keys.Q) || gp.Buttons.Y == ButtonState.Pressed, PadButtons.Y);
        Map(kb.IsKeyDown(Keys.Tab) || gp.Buttons.RightShoulder == ButtonState.Pressed, PadButtons.Z);
        Map(kb.IsKeyDown(Keys.Up) || gp.DPad.Up == ButtonState.Pressed, PadButtons.Up);
        Map(kb.IsKeyDown(Keys.Down) || gp.DPad.Down == ButtonState.Pressed, PadButtons.Down);
        Map(kb.IsKeyDown(Keys.Left) || gp.DPad.Left == ButtonState.Pressed, PadButtons.Left);
        Map(kb.IsKeyDown(Keys.Right) || gp.DPad.Right == ButtonState.Pressed, PadButtons.Right);

        float lx = gp.ThumbSticks.Left.X, ly = gp.ThumbSticks.Left.Y;
        if (kb.IsKeyDown(Keys.A)) lx = -1; if (kb.IsKeyDown(Keys.D)) lx = 1;
        if (kb.IsKeyDown(Keys.W)) ly = 1;  if (kb.IsKeyDown(Keys.S)) ly = -1;
        var mouse = Mouse.GetState();
        var center = new Point(Window.ClientBounds.Width / 2, Window.ClientBounds.Height / 2);
        if (!_mouseCaptured && IsActive && mouse.LeftButton == ButtonState.Pressed &&
            Window.ClientBounds.Contains(Window.ClientBounds.X + mouse.X, Window.ClientBounds.Y + mouse.Y))
        {
            _mouseCaptured = true;
            IsMouseVisible = false;
            Mouse.SetPosition(center.X, center.Y);
            mouse = Mouse.GetState();
        }
        if (_mouseCaptured && (kb.IsKeyDown(Keys.Escape) || !IsActive))
        {
            _mouseCaptured = false;
            IsMouseVisible = true;
        }
        float mdx = 0, mdy = 0;
        if (_mouseCaptured)
        {
            mdx = (mouse.X - center.X) * MouseSensitivity;
            mdy = -(mouse.Y - center.Y) * MouseSensitivity;
            Mouse.SetPosition(center.X, center.Y);
        }
        bool mouseFire = _mouseCaptured && mouse.LeftButton == ButtonState.Pressed;
        bool mouseAlt = _mouseCaptured && mouse.RightButton == ButtonState.Pressed;
        float lt = Math.Max(gp.Triggers.Left, kb.IsKeyDown(Keys.Z) || mouseAlt ? 1f : 0f);
        float rt = Math.Max(gp.Triggers.Right, mouseFire ? 1f : 0f);
        if (lt > 0.9f) b |= PadButtons.L;
        if (rt > 0.9f) b |= PadButtons.R;
        float cx = gp.ThumbSticks.Right.X, cy = gp.ThumbSticks.Right.Y;
        if (kb.IsKeyDown(Keys.J)) cx = -1; if (kb.IsKeyDown(Keys.L)) cx = 1;
        if (kb.IsKeyDown(Keys.I)) cy = 1;  if (kb.IsKeyDown(Keys.K)) cy = -1;
        if (Math.Abs(mdx) > Math.Abs(cx)) cx = Math.Clamp(mdx, -1f, 1f);
        if (Math.Abs(mdy) > Math.Abs(cy)) cy = Math.Clamp(mdy, -1f, 1f);
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
        Window.Title = "Turok: Evolution (recompiled) - " + _status;
        base.Update(gameTime);
    }

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
            $"draw: {_drawCount} calls, avg {_drawMsTotal / Math.Max(1, _drawCount):F2} ms, max {_drawMsMax:F1} ms; game frames received {_framesReceived}\n");
        base.OnExiting(sender, args);
    }

    // ---- frames ----------------------------------------------------------------------
    private unsafe void PullFrame()
    {
        CoreFrame* f = Core.AcquireFrame();
        if (f == null) return;
        try
        {
            if (f->Serial == _lastSerial) return;
            _lastSerial = f->Serial;
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
                b.CopySrc = new Rectangle(cb->CopySrc[0], cb->CopySrc[1], cb->CopySrc[2], cb->CopySrc[3]);
                b.ClearColor = cb->ClearColor; b.ClearDepth = cb->ClearDepth;
                for (int t = 0; t < 8; t++)
                {
                    b.CopyRefs[t] = cb->Textures[t] >= 0 && f->Textures[cb->Textures[t]].Rgba == null ? f->Textures[cb->Textures[t]].Id : 0;
                    b.Textures[t] = cb->Textures[t] >= 0 ? textures[cb->Textures[t]] : null;
                    b.TextureIds[t] = cb->Textures[t] >= 0 ? f->Textures[cb->Textures[t]].Id : 0;
                    b.Samplers[t] = Sampler(cb->Sampler[t]);
                }
                b.Viewport = new Rectangle(cb->Viewport[0] * RenderScale, cb->Viewport[1] * RenderScale,
                                           cb->Viewport[2] * RenderScale, cb->Viewport[3] * RenderScale);
                b.Scissor = new Rectangle(cb->Scissor[0] * RenderScale, cb->Scissor[1] * RenderScale,
                                          (cb->Scissor[2] - cb->Scissor[0]) * RenderScale,
                                          (cb->Scissor[3] - cb->Scissor[1]) * RenderScale);
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
        s = new SamplerState
        {
            AddressU = Wrap(mode & 3),
            AddressV = Wrap((mode >> 2) & 3),
            Filter = filter,
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

    private void DrawFrame()
    {
        PullFrame();
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
            gd.DepthStencilState = m.Depth;
            gd.BlendState = m.Blend;

            P["Proj"].SetValue(b.Projection);
            P["ColorIn"].SetValue(m.ColorIn); P["AlphaIn"].SetValue(m.AlphaIn);
            P["ColorOp"].SetValue(m.ColorOp); P["AlphaOp"].SetValue(m.AlphaOp);
            P["Stage"].SetValue(m.Stage); P["Swap"].SetValue(m.Swap); P["Konst"].SetValue(m.Konst);
            P["SwapTable"].SetValue(m.SwapTable); P["Regs"].SetValue(m.Regs); P["MapCoord"].SetValue(m.MapCoord);
            P["AlphaTest"].SetValue(m.AlphaTest);
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
                _texSize[t] = new Vector4(bound.Width, bound.Height, 0, 0);
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
        _sprites!.Begin(samplerState: SamplerState.LinearClamp);
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
                            $"stage1 map {m.Stage[1].X} uv0 ({v.T0.X:F2},{v.T0.Y:F2}) uv1 ({v.T1.X:F2},{v.T1.Y:F2}) c0 {v.Color0} c1 {v.Color1}");
            }
        }
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
}
