// SPDX-License-Identifier: GPL-3.0-or-later
// OV-03.cs  —  OV-03 (Pico TB-303 音序器 + 示波器/参数监视, 原生 Windows 程序)
// 版本: OV-03 v1.2
// 编译: csc /target:winexe /out:OV-03.exe /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll 303seq.cs
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Collections.Generic;
using System.Globalization;
using System.Diagnostics;
using System.Runtime.InteropServices;

// ---- 程序集元数据 (版本号方案: v1.0 -> v1.1 ... v1.10; 大版本改前一位 -> v2.0) ----
[assembly: System.Reflection.AssemblyTitle("OV-03")]
[assembly: System.Reflection.AssemblyProduct("OV-03")]
[assembly: System.Reflection.AssemblyCompany("OV-03")]
[assembly: System.Reflection.AssemblyVersion("1.2.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("1.2.0.0")]
[assembly: System.Reflection.AssemblyInformationalVersion("OV-03 v1.2")]

class Step { public int note = 45; public bool acc, slide, rest, tie; }   // 默认 A2

// ================= 皮肤 (主题) =================
// 所有可换肤的颜色/字体/布局都在这里; 由 .303paint 皮肤文件覆盖 (见 LoadSkinFile)
static class Theme
{
    public static string Name = "默认";
    public static string FontName = "Segoe UI";
    public static float  FontSize = 9f;

    // 通用
    public static Color FormBg   = C(24, 24, 28);
    public static Color Text     = Color.Gainsboro;      // 主文字
    public static Color TextDim  = Color.Gray;           // 次要文字
    public static Color Header   = C(130, 150, 190);     // 分组标题
    public static Color Status   = Color.LightGreen;     // 状态栏文字
    public static Color Value    = Color.LightSkyBlue;   // 数值/音名
    public static Color Warn     = Color.Orange;         // 警告/未连接
    public static Color StepNo   = Color.SlateGray;      // 步号

    // 区域背景
    public static Color TopBg    = C(32, 32, 38);
    public static Color StepBg   = C(18, 18, 22);
    public static Color MonBg    = C(20, 20, 24);
    public static Color CellBg   = C(28, 28, 34);
    public static Color CellBgOff= C(18, 18, 20);
    public static Color Divider  = C(120, 120, 145);

    // 按钮 / 开关
    public static Color BtnBg    = C(52, 52, 60);
    public static Color BtnFg    = Color.Gainsboro;
    public static Color BtnBorder= C(80, 80, 90);
    public static Color ToggleBg = C(44, 44, 52);
    public static Color Accent   = Color.DodgerBlue;     // pattern 高亮
    public static Color AccentFg = Color.White;
    public static Color Playhead = Color.Gold;           // 播放头步号
    public static Color PlayFg   = Color.Black;
    // 音序格开关"开"时的颜色
    public static Color OnAcc    = Color.OrangeRed;
    public static Color OnSlide  = Color.DeepSkyBlue;
    public static Color OnRest   = Color.MediumPurple;
    public static Color OnTie    = Color.SeaGreen;
    public static Color NoteTie  = Color.Khaki;

    // 自绘控件
    public static Color ScopeBg  = C(8, 12, 8);
    public static Color ScopeGrid= C(18, 34, 18);
    public static Color ScopeMid = C(28, 55, 28);
    public static Color ScopeTrace = Color.Lime;
    public static Color BarBg    = C(36, 36, 42);
    public static Color BarFill  = C(46, 90, 150);
    public static Color KnobBg   = C(32, 32, 38);
    public static Color KnobFace = C(48, 48, 56);
    public static Color KnobRing = C(95, 95, 108);
    public static Color KnobPtr  = C(120, 200, 255);
    public static Color TogOn    = C(60, 170, 80);
    public static Color TogOff   = C(72, 72, 84);
    public static Color TogKnob  = Color.White;
    public static Color InputBg  = Color.White;          // 下拉框底
    public static Color InputFg  = Color.Black;

    // 布局 (像素): 工具条高 / 步进区高 / 示波器宽占比
    public static int TopH   = 282;
    public static int StepH  = 322;
    public static double ScopeW = 0.55;

    // 背景图片 (可选): BgImage=文件名(相对皮肤)或绝对路径; BgMode 0=拉伸 1=平铺 2=居中
    public static string BaseDir = "";         // 当前皮肤所在目录 (解析相对路径用)
    public static string BgImage = "";
    public static int    BgMode  = 0;

    // 元素偏移: 名称 -> {dx,dy,dw,dh}
    public static Dictionary<string, int[]> Elem = new Dictionary<string, int[]>();

    static Color C(int r, int g, int b) { return Color.FromArgb(r, g, b); }
    public static Color P(string s, Color d) {   // 解析 #RRGGBB / #AARRGGBB
        try { if (string.IsNullOrEmpty(s)) return d; s = s.Trim().TrimStart('#');
              int v = int.Parse(s, NumberStyles.HexNumber);
              if (s.Length <= 6) return Color.FromArgb(255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
              return Color.FromArgb((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255); }
        catch { return d; }
    }
    public static string H(Color c) { return string.Format("#{0:X2}{1:X2}{2:X2}", c.R, c.G, c.B); }

    // ---- 读取 .303paint 皮肤 (键=值; # 注释) ----
    public static void Load(string path)
    {
        try { BaseDir = Path.GetDirectoryName(Path.GetFullPath(path)); } catch { BaseDir = ""; }
        foreach (string raw in File.ReadAllLines(path))
        {
            string l = raw.Trim(); if (l.Length == 0 || l[0] == '#') continue;
            int eq = l.IndexOf('='); if (eq <= 0) continue;
            string k = l.Substring(0, eq).Trim(), v = l.Substring(eq + 1).Trim();
            try
            {
                switch (k)
                {
                    case "name": Name = v; break;
                    case "font": FontName = v; break;
                    case "fontsize": FontSize = float.Parse(v, CultureInfo.InvariantCulture); break;
                    case "topH": TopH = int.Parse(v); if (TopH < 282) TopH = 282; break;   // 旧皮肤(topH=222)会裁掉录音区, 强制最小
                    case "stepH": StepH = int.Parse(v); break;
                    case "scopeW": ScopeW = double.Parse(v, CultureInfo.InvariantCulture); break;
                    case "bgimage": BgImage = v; break;
                    case "bgmode": BgMode = int.Parse(v); break;
                    case "c.formBg": FormBg = P(v, FormBg); break;
                    case "c.text": Text = P(v, Text); break;
                    case "c.textDim": TextDim = P(v, TextDim); break;
                    case "c.header": Header = P(v, Header); break;
                    case "c.status": Status = P(v, Status); break;
                    case "c.value": Value = P(v, Value); break;
                    case "c.warn": Warn = P(v, Warn); break;
                    case "c.stepNo": StepNo = P(v, StepNo); break;
                    case "c.topBg": TopBg = P(v, TopBg); break;
                    case "c.stepBg": StepBg = P(v, StepBg); break;
                    case "c.monBg": MonBg = P(v, MonBg); break;
                    case "c.cellBg": CellBg = P(v, CellBg); break;
                    case "c.cellBgOff": CellBgOff = P(v, CellBgOff); break;
                    case "c.divider": Divider = P(v, Divider); break;
                    case "c.btnBg": BtnBg = P(v, BtnBg); break;
                    case "c.btnFg": BtnFg = P(v, BtnFg); break;
                    case "c.btnBorder": BtnBorder = P(v, BtnBorder); break;
                    case "c.toggleBg": ToggleBg = P(v, ToggleBg); break;
                    case "c.accent": Accent = P(v, Accent); break;
                    case "c.accentFg": AccentFg = P(v, AccentFg); break;
                    case "c.playhead": Playhead = P(v, Playhead); break;
                    case "c.playFg": PlayFg = P(v, PlayFg); break;
                    case "c.onAcc": OnAcc = P(v, OnAcc); break;
                    case "c.onSlide": OnSlide = P(v, OnSlide); break;
                    case "c.onRest": OnRest = P(v, OnRest); break;
                    case "c.onTie": OnTie = P(v, OnTie); break;
                    case "c.noteTie": NoteTie = P(v, NoteTie); break;
                    case "c.scopeBg": ScopeBg = P(v, ScopeBg); break;
                    case "c.scopeGrid": ScopeGrid = P(v, ScopeGrid); break;
                    case "c.scopeMid": ScopeMid = P(v, ScopeMid); break;
                    case "c.scopeTrace": ScopeTrace = P(v, ScopeTrace); break;
                    case "c.barBg": BarBg = P(v, BarBg); break;
                    case "c.barFill": BarFill = P(v, BarFill); break;
                    case "c.knobBg": KnobBg = P(v, KnobBg); break;
                    case "c.knobFace": KnobFace = P(v, KnobFace); break;
                    case "c.knobRing": KnobRing = P(v, KnobRing); break;
                    case "c.knobPtr": KnobPtr = P(v, KnobPtr); break;
                    case "c.togOn": TogOn = P(v, TogOn); break;
                    case "c.togOff": TogOff = P(v, TogOff); break;
                    case "c.togKnob": TogKnob = P(v, TogKnob); break;
                    case "c.inputBg": InputBg = P(v, InputBg); break;
                    case "c.inputFg": InputFg = P(v, InputFg); break;
                    default:
                        if (k.StartsWith("elem."))
                        {
                            string nm = k.Substring(5); string[] a = v.Split(',');
                            if (a.Length >= 4) Elem[nm] = new int[] { int.Parse(a[0]), int.Parse(a[1]), int.Parse(a[2]), int.Parse(a[3]) };
                        }
                        break;
                }
            }
            catch { }
        }
    }

    // ---- 写出 .303paint 皮肤 ----
    public static void Save(string path)
    {
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("# 303paint skin  (OV-03 皮肤文件, 可用 303paint 编辑器修改)");
        sb.AppendLine("name=" + Name);
        sb.AppendLine("font=" + FontName);
        sb.AppendLine("fontsize=" + FontSize.ToString(CultureInfo.InvariantCulture));
        sb.AppendLine("topH=" + TopH);
        sb.AppendLine("stepH=" + StepH);
        sb.AppendLine("scopeW=" + ScopeW.ToString(CultureInfo.InvariantCulture));
        sb.AppendLine("bgimage=" + BgImage);
        sb.AppendLine("bgmode=" + BgMode);
        Action<string, Color> W = delegate(string k, Color c) { sb.AppendLine(k + "=" + H(c)); };
        W("c.formBg", FormBg); W("c.text", Text); W("c.textDim", TextDim); W("c.header", Header);
        W("c.status", Status); W("c.value", Value); W("c.warn", Warn); W("c.stepNo", StepNo);
        W("c.topBg", TopBg); W("c.stepBg", StepBg); W("c.monBg", MonBg); W("c.cellBg", CellBg);
        W("c.cellBgOff", CellBgOff); W("c.divider", Divider); W("c.btnBg", BtnBg); W("c.btnFg", BtnFg);
        W("c.btnBorder", BtnBorder); W("c.toggleBg", ToggleBg); W("c.accent", Accent); W("c.accentFg", AccentFg);
        W("c.playhead", Playhead); W("c.playFg", PlayFg); W("c.onAcc", OnAcc); W("c.onSlide", OnSlide);
        W("c.onRest", OnRest); W("c.onTie", OnTie); W("c.noteTie", NoteTie); W("c.scopeBg", ScopeBg);
        W("c.scopeGrid", ScopeGrid); W("c.scopeMid", ScopeMid); W("c.scopeTrace", ScopeTrace);
        W("c.barBg", BarBg); W("c.barFill", BarFill); W("c.knobBg", KnobBg); W("c.knobFace", KnobFace);
        W("c.knobRing", KnobRing); W("c.knobPtr", KnobPtr); W("c.togOn", TogOn); W("c.togOff", TogOff);
        W("c.togKnob", TogKnob); W("c.inputBg", InputBg); W("c.inputFg", InputFg);
        foreach (KeyValuePair<string, int[]> kv in Elem)
        { int[] a = kv.Value; sb.AppendLine("elem." + kv.Key + "=" + a[0] + "," + a[1] + "," + a[2] + "," + a[3]); }
        File.WriteAllText(path, sb.ToString(), Encoding.UTF8);
    }

    public const string DefaultText = "";
}



class ScopePanel : Panel
{
    int[] buf = new int[0];                // 复用缓冲 (避免每帧 new int[] -> GC/CPU)
    int dataLen = 0;
    public int spanSamples = 288;          // 显示窗口(采样数): 时基
    public ScopePanel() { DoubleBuffered = true; BackColor = Theme.ScopeBg; }
    public void SetData(int[] src, int n)  // 拷贝进内部缓冲后重绘 (调用方缓冲可复用)
    {
        if (n < 0) n = 0;
        if (buf.Length < n) buf = new int[n];
        if (n > 0) Array.Copy(src, buf, n);
        dataLen = n;
        Invalidate();
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        int w = Width, h = Height, mid = h / 2;
        g.Clear(BackColor);
        for (int x = 0; x < w; x += 48) { using (Pen gg = new Pen(Theme.ScopeGrid)) g.DrawLine(gg, x, 0, x, h); }
        using (Pen gp = new Pen(Theme.ScopeMid)) { g.DrawLine(gp, 0, mid, w, mid); }
        int n = dataLen; if (n < 8) return;
        long sum = 0; for (int i = 0; i < n; i++) sum += buf[i];
        int dc = (int)(sum / n);
        int mx = 1; for (int i = 0; i < n; i++) { int a = buf[i] - dc; if (a < 0) a = -a; if (a > mx) mx = a; }
        int hyst = mx / 4 + 30;
        int start = 0;                                    // 触发: 去直流后的上升过零 (自适应迟滞)
        for (int i = 1; i < n - 1; i++) { int a = buf[i - 1] - dc, b = buf[i] - dc; if (a < -hyst && b >= hyst) { start = i; break; } }
        int span = spanSamples; if (span > n - start) span = n - start; if (span < 16) { start = 0; span = n > spanSamples ? spanSamples : n; }
        using (Pen p = new Pen(Theme.ScopeTrace, 1.4f))
        {
            PointF prev = new PointF(0, mid);
            for (int x = 0; x < w; x++)
            {
                double fi = start + (double)x / (w - 1) * (span - 1);   // 线性插值 -> 高分辨率
                int i0 = (int)fi; if (i0 > n - 1) i0 = n - 1; int i1 = i0 + 1; if (i1 > n - 1) i1 = n - 1;
                double f = fi - i0;
                double v = (buf[i0] * (1 - f) + buf[i1] * f) - dc;
                float y = (float)(mid - v * (h * 0.45) / mx);
                PointF cur = new PointF(x, y);
                if (x > 0) g.DrawLine(p, prev, cur);
                prev = cur;
            }
        }
    }
}

// ---- Windows waveOut (直接输出 PCM 到指定播放设备; 无外部进程) ----
[StructLayout(LayoutKind.Sequential, Pack = 1)]
struct WAVEFORMATEX { public ushort wFormatTag, nChannels; public uint nSamplesPerSec, nAvgBytesPerSec; public ushort nBlockAlign, wBitsPerSample, cbSize; }
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
struct WAVEOUTCAPS { public ushort wMid, wPid; public uint vDriverVersion; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname; public uint dwFormats; public ushort wChannels, wReserved1; public uint dwSupport; }

class ParamBar : Control                       // 可拖动参数条 (电位器关时可鼠标设置)
{
    public double value = 0;                   // 0..1
    public bool interactive = false;
    public bool held = false;                   // 正在被拖动 (拖动期间 K 行不回写本)
    public Action<double> onSet;
    public double def = 0;                      // 双击复位默认值
    int lastX = 0; double startVal = 0;
    public ParamBar() { DoubleBuffered = true; BackColor = Theme.BarBg; }
    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics; g.Clear(BackColor);
        using (SolidBrush b = new SolidBrush(Theme.BarFill)) g.FillRectangle(b, 0, 0, (int)(Width * value), Height);
        using (Pen p = new Pen(Theme.BtnBorder)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
    }
    // 相对拖动: 按住后按鼠标位移改变 (不再"跳到光标位置" -> 不会乱跑)
    protected override void OnMouseDown(MouseEventArgs e) { if (interactive) { held = true; Capture = true; lastX = e.X; startVal = value; } base.OnMouseDown(e); }
    protected override void OnMouseMove(MouseEventArgs e) { if (interactive && held) { double v = startVal + (double)(e.X - lastX) / Math.Max(1, Width); if (v < 0) v = 0; if (v > 1) v = 1; value = v; Invalidate(); if (onSet != null) onSet(v); } base.OnMouseMove(e); }
    protected override void OnMouseUp(MouseEventArgs e) { if (held) { held = false; Capture = false; if (interactive && onSet != null) onSet(value); } base.OnMouseUp(e); }
    protected override void OnMouseDoubleClick(MouseEventArgs e) { if (!interactive) return; value = def; Invalidate(); if (onSet != null) onSet(value); base.OnMouseDoubleClick(e); }
}

class Knob : Control                            // 旋钮 (上下拖动改值; 双击归零)
{
    public double value = 0;                    // 0..1
    public Action<double> onSet;
    bool held = false; int lastY = 0; double startVal = 0;
    public Knob() { DoubleBuffered = true; SetStyle(ControlStyles.SupportsTransparentBackColor, true); BackColor = Color.Transparent; }
    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias;
        int w = Width, h = Height, cx = w / 2, cy = h / 2 + 1, R = Math.Min(w, h) / 2 - 4; if (R < 4) R = 4;
        using (SolidBrush b = new SolidBrush(Theme.KnobFace)) g.FillEllipse(b, cx - R, cy - R, 2 * R, 2 * R);
        using (Pen p = new Pen(Theme.KnobRing, 1.5f)) g.DrawEllipse(p, cx - R, cy - R, 2 * R, 2 * R);
        double ang = (-135.0 + 270.0 * value) * Math.PI / 180.0;
        int ix = cx + (int)(Math.Sin(ang) * (R - 4)), iy = cy - (int)(Math.Cos(ang) * (R - 4));
        using (Pen p = new Pen(Theme.KnobPtr, 2.2f)) g.DrawLine(p, cx, cy, ix, iy);
    }
    protected override void OnMouseDown(MouseEventArgs e) { held = true; Capture = true; lastY = e.Y; startVal = value; base.OnMouseDown(e); }
    protected override void OnMouseMove(MouseEventArgs e) { if (held) { double v = startVal + (lastY - e.Y) / 110.0; if (v < 0) v = 0; if (v > 1) v = 1; value = v; Invalidate(); if (onSet != null) onSet(value); } base.OnMouseMove(e); }
    protected override void OnMouseUp(MouseEventArgs e) { if (held) { held = false; Capture = false; if (onSet != null) onSet(value); } base.OnMouseUp(e); }
    protected override void OnMouseDoubleClick(MouseEventArgs e) { value = 0; Invalidate(); if (onSet != null) onSet(value); base.OnMouseDoubleClick(e); }
}

class ToggleSwitch : Control                  // 左右开关 (自绘 pill)
{
    public bool on = false;
    public Action<bool> onToggle;
    public ToggleSwitch() { DoubleBuffered = true; SetStyle(ControlStyles.SupportsTransparentBackColor, true); BackColor = Color.Transparent; Size = new Size(46, 22); Cursor = Cursors.Hand; }
    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias;
        int r = Height - 2, y = (Height - r) / 2, w = Width - 2;
        using (SolidBrush b = new SolidBrush(on ? Theme.TogOn : Theme.TogOff))
        { g.FillEllipse(b, 0, y, r, r); g.FillEllipse(b, w - r, y, r, r); g.FillRectangle(b, r / 2, y, w - r, r); }
        int kx = on ? (w - r + 2) : 2;
        using (SolidBrush kb = new SolidBrush(Theme.TogKnob)) g.FillEllipse(kb, kx, y + 2, r - 4, r - 4);
    }
    protected override void OnMouseDown(MouseEventArgs e) { on = !on; Invalidate(); if (onToggle != null) onToggle(on); base.OnMouseDown(e); }
}

class MainForm : Form
{
    SerialPort sp = new SerialPort();
    Timer tmr = new Timer();
    StringBuilder rxBuf = new StringBuilder();
    bool reading = false;
    List<string> dump = new List<string>();
    int dcCheck = 0;                            // 掉线检测计数
    volatile int uiTickMs = 0;                  // UI 心跳 (卡死检测用)
    double lastUp = -1;                         // Pico 运行毫秒 (检测复位)
    static string CrashPath = null;
    static System.Threading.Mutex _instMutex = null;   // 存在期间 -> 安装/卸载会提示先关闭程序 (AppMutex)
    Dictionary<string, Control> elems = new Dictionary<string, Control>();      // 可换肤元素
    Dictionary<string, Rectangle> baseRects = new Dictionary<string, Rectangle>();
    // 数据子目录 (pattern / log), 不存在则创建; 返回绝对路径
    static string DataDir(string sub) {
        string d = Path.Combine(Application.StartupPath, sub);
        try { Directory.CreateDirectory(d); } catch {}
        return d;
    }
    // 简单键值设置 (持久化到 log\ov03.cfg)
    static string CfgPath() { return Path.Combine(DataDir("log"), "ov03.cfg"); }
    static string CfgGet(string key, string def) {
        try { foreach (string l in File.ReadAllLines(CfgPath())) { int e = l.IndexOf('='); if (e > 0 && l.Substring(0, e).Trim() == key) return l.Substring(e + 1).Trim(); } } catch {}
        return def;
    }
    static void CfgSet(string key, string val) {
        try {
            var d = new System.Collections.Generic.Dictionary<string, string>();
            try { foreach (string l in File.ReadAllLines(CfgPath())) { int e = l.IndexOf('='); if (e > 0) d[l.Substring(0, e).Trim()] = l.Substring(e + 1).Trim(); } } catch {}
            d[key] = val;
            var sb = new StringBuilder();
            foreach (var kv in d) sb.AppendLine(kv.Key + "=" + kv.Value);
            File.WriteAllText(CfgPath(), sb.ToString(), Encoding.UTF8);
        } catch {}
    }
    static void Crash(string s) {
        try {
            if (CrashPath == null) CrashPath = Path.Combine(DataDir("log"), "OV-03_crash.txt");
            try {   // 日志滚动: 最多保留 5MB; 超出就丢最早的、存最新的 (仍 <=5MB)
                const long MAX = 5L * 1024 * 1024;
                if (File.Exists(CrashPath)) {
                    byte[] old = File.ReadAllBytes(CrashPath);
                    int add = Encoding.UTF8.GetByteCount(s);
                    if ((long)old.Length + add > MAX) {
                        int keep = (int)Math.Max(0, MAX - add);
                        int cut = old.Length > keep ? old.Length - keep : 0;
                        while (cut < old.Length && old[cut] != (byte)10) cut++;   // 对齐到整行, 不留半行
                        if (cut < old.Length) cut++;
                        byte[] tail = new byte[old.Length - cut];
                        Array.Copy(old, cut, tail, 0, tail.Length);
                        File.WriteAllBytes(CrashPath, tail);
                    }
                }
            } catch {}
            File.AppendAllText(CrashPath, s, Encoding.UTF8);
        } catch {}
    }
    System.Threading.Thread serialThr = null;   // 串口读取线程 (与 UI 解耦)
    System.Threading.Thread sendThr = null;     // 串口发送线程 (UI 不阻塞)
    System.Threading.Thread lostThr = null;     // 断开收尾线程 (sp.Close 可能阻塞, 放后台防冻 UI)
    volatile bool serialDead = false;           // SerialPump 检测到串口断开
    volatile string uiOp = "idle";              // UI 线程当前操作 (卡死时写入日志定位)
    volatile int lastAudioMs = 0;               // 最后一次收到音频帧的时刻 (检测音频流中断)
    volatile int cSerial = 0, cSend = 0, cAudio = 0;   // 各后台线程迭代计数 (抓 100% 空转)
    volatile bool serialRun = false;
    int reconnectPhase = 0;                     // 0=空闲 1=待关 2=等消失 3=等回来 4=重开
    int reconnectAt = 0;
    int connectTimeMs = 0;                       // 本次连接时刻 (用于检测"选错口")
    bool connectWarned = true;                   // 连上后是否已收到固件回复(文本口会回 T/Q)
    bool audioBeforeReconnect = false;
    string[] prePorts = new string[0];          // 复位前的端口列表 (用于发现复位后新出现的端口)
    Queue<string> textQ = new Queue<string>();  // 后台解析出的文本行, 交给 UI 处理
    object textLock = new object();
    Queue<string> sendQ = new Queue<string>();  // 待发送命令 (后台线程写, UI 不阻塞)
    object sendLock = new object();

    ComboBox cboPort = new ComboBox();
    ComboBox cboAudioPort = new ComboBox();     // 音频口 (第2路 CDC); 只在开 USB 音频时用
    Button btnRefresh = new Button(), btnConnect = new Button(), btnSkin = new Button();
    Label lblConn = new Label();
    ComboBox cboLen = new ComboBox();
    ComboBox cboTb = new ComboBox();
    TextBox numBpm = new TextBox();
    Button btnRead = new Button(), btnWrite = new Button(), btnPlay = new Button(), btnStop = new Button(), btnClear = new Button();
    Button btnSaveFile = new Button(), btnLoadFile = new Button();
    Button btnUploadImg = new Button();         // 上传图片到 Pico 屏幕
    Button btnClearImg = new Button();          // 清除图片 (恢复笑脸)
    CheckBox chkDither = new CheckBox();        // 上传图片时转成点阵图
    ToggleSwitch swWave = new ToggleSwitch();   // 波形左右开关 (锯齿<->方波)
    Label lblWaveState = new Label();           // 波形状态文字
    int waveSquare = 0;                         // 0=锯齿 1=方波 (从固件 K 行同步)
    Label lblStatus = new Label();
    Button[] btnPat = new Button[4];
    CheckBox[] chkEn = new CheckBox[4];
    int playMask = 0x0F;
    bool suppressChk = false;
    bool suppressPort = false;                  // 程序回填端口下拉时不要触发"换口重连"
    bool suppressAudioPort = false;
    int readStartMs = 0;
    int playPat = -1, playStep = -1;
    bool playRun = false;

    ScopePanel scope = new ScopePanel();
    ParamBar[] bars = new ParamBar[8];
    double[] barPending = new double[8];
    bool[] barDirty = new bool[8];
    double[] barDefault = { 0.5, 0.3, 0.5, 0.5, 0.5, 0.5, 0.0, 0.5 };   // 双击复位默认值 (CUT RES ENV DEC ACC TUNE DIST COLOR)
    Label[] barVal = new Label[8];
    string[] barName = new string[] { "CUT", "RES", "ENV", "DEC", "ACC", "TUNE", "DIST", "COLOR" };
    Knob kNoise = new Knob(), kVar = new Knob(), kTemp = new Knob();
    double[] knobPending = new double[3];
    bool[] knobDirty = new bool[3];
    Label[] knobVal = new Label[3];
    string[] knobPre = new string[] { "!", "%", "&" };

    // USB 音频回传开关 (读 Pico 的 PCM 流 -> Windows waveOut 直接输出到所选设备; 无外部进程)
    CheckBox chkAudio = new CheckBox();
    CheckBox chkPots = new CheckBox();          // 电位器开关: 开=硬件电位器, 关=软件拖动参数条
    CheckBox chkResBass = new CheckBox();       // 谐振低频补偿 (h bit6): 关=原版 res 逻辑, 开=按 res 抬低频
    CheckBox chkSelfOsc = new CheckBox();       // 自激开关 (S0/S1): 关=原机行为(不自激), 开=res 拉满可自激
    CheckBox chkDistPost = new CheckBox();      // 失真位置 (/1 或 /0): 开=VCA之后(TD-3), 关=VCA之前
    bool distPost = true;                       // 失真位置: true=VCA后(TD-3 默认)
    bool potsOn = true;                         // 默认开 (硬件电位器); 取消勾选才走软件参数条 (仅连接期间生效)
    const double CUT_MIN_HZ = 20.0, CUT_MAX_HZ = 4000.0;   // 与固件 Syrebas 一致 (真机实测下限 200Hz)
    ComboBox cboDev = new ComboBox();           // 输出设备
    bool audioOn = false;
    Queue<byte[]> audioQ = new Queue<byte[]>();
    object audioLock = new object();
    int audioBytes = 0;                         // 队列里待播字节数 (按字节限流, 别按块!)
    ParamBar volBar = new ParamBar();           // 监听音量 (自绘条, 避免 TrackBar 黑块)
    volatile float audioGain = 8.0f;            // 输出只有 ~3%, 监听需放大
    System.Threading.Thread audioThr = null;
    volatile bool audioRun = false;
    SerialPort spAudio = new SerialPort();      // 第 2 路 CDC: 专用音频 (裸 PCM)
    // ---- 录音 (录 USB 音频这一路) ----
    volatile bool recOn = false;                // 正在录音
    MemoryStream recMs = null;                  // 录到的裸 PCM (48k/16bit/mono)
    readonly object recLock = new object();
    long recBytes = 0;                          // 已录字节
    byte[] lastRecPcm = null;                   // 最近一次录音数据 (内存, 导出才落盘)
    string lastRecPath = "";                    // 最近一次导出的文件
    string audioPortName = null;                // 自动探测到的音频口
    System.Threading.Thread audioReadThr = null;
    [DllImport("winmm.dll")] static extern int waveOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)] static extern int waveOutGetDevCaps(IntPtr uDeviceID, ref WAVEOUTCAPS caps, int size);
    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr h, int id, ref WAVEFORMATEX wf, IntPtr cb, IntPtr inst, int flags);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr h, IntPtr wh, int size);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr h, IntPtr wh, int size);
    [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr h, IntPtr wh, int size);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutPause(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutRestart(IntPtr h);
    const int WAVE_MAPPER = -1, WHDR_DONE = 1, WAVHDR_SIZE = 48, AUD_BUFSZ = 1024, AUD_NBUF = 5;
    IntPtr hWave = IntPtr.Zero;
    IntPtr[] audHdr = new IntPtr[AUD_NBUF];
    IntPtr[] audBuf = new IntPtr[AUD_NBUF];
    bool[] audBusy = new bool[AUD_NBUF];

    Label lblCpu = new Label();                 // CPU 占用率 (core1/core0 + 主频/温度)
    Button btnCpuScan = new Button();
    Timer oneShotScan = new Timer();            // 连接后 5s 自动扫一次 (不常扫)
    double cpuC1 = -1, cpuC0 = -1, cpuMHz = -1, cpuTemp = -1;
    double cpuOcIdx = -1, cpuFails = -1;

    ComboBox cboOc = new ComboBox();            // 超频档位 (K<idx>)
    CheckBox chkWdt = new CheckBox();           // 看门狗开关 (u0/u1)
    CheckBox chkOcUnlock = new CheckBox();      // 超频解锁开关 (+0/+1; 关=最高375MHz)
    ComboBox cboFiltOS = new ComboBox();        // 滤波过采样倍数 (@4/8/12/16)
    Button btnReset = new Button();             // 板子复位 (z)
    bool suppressOc = false, suppressWdt = false, suppressFos = false, suppressSelfOsc = false, suppressDistPost = false;
    string ocSig = "";                          // 固件上报的档位表签名 (变了才重建下拉)
    string[] ocTable = null;                    // 固件上报的档位表 ("MHz/mV",...)
    bool ocUnlocked = false;                    // 解锁状态
    int ocMaxLock = 4;                          // 锁定时的最高档索引 (固件 ocmax=)
    int ocApplied = -1;                         // 已应用的超频档 (供"选好才应用")
    int fosApplied = -1;                        // 已应用的过采样 (同上)
    bool follow = false;                        // 跟随: 自动切到正在播放的 pattern
    Button btnFollow = new Button();

    Step[][] pat = new Step[4][];
    int[] patLen = new int[4];
    int curPat = 0;
    public bool argAudio = false, argPlay = false;     // 命令行自测: --audio / --play
    public int argDev = -1;                            // --dev=N 选输出设备序号(下拉序号)

        const int STEPW = 54, STEPH = 248;   // Theme.StepH=步进区总高; Theme.TopH=工具条高
    Button[] bAcc, bSlide, bRest, bTie, bNoteUp, bNoteDn, bOctDn, bOctUp;
    Label[] lblNote, lblNo;

    static readonly string[] NN = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static string NoteName(int n) { return NN[n % 12] + (n / 12 - 1).ToString(); }

    public MainForm()
    {
        Text = "OV-03 v1.2";
        try { Icon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch {}   // 标题栏/任务栏图标 = 内嵌图标
        // 默认尺寸按屏幕可用区自适应 (理想 1400x880): 保证音序器不被挤到出现滚动条
        {
            Rectangle wa = Screen.PrimaryScreen.WorkingArea;
            int w = Math.Min(1400, wa.Width - 40);
            int h = Math.Min(880, wa.Height - 40);
            if (w < 900) w = wa.Width;
            if (h < 740) h = wa.Height;
            Size = new Size(w, h);
        }
        MinimumSize = new Size(860, 660);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Theme.FormBg; ForeColor = Theme.Text;
        Font = new Font(Theme.FontName, Theme.FontSize);
        AutoScaleMode = AutoScaleMode.None;               // 关闭 DPI 自动缩放: 控件按字面像素摆放 (否则顶部被缩放挤掉)

        for (int i = 0; i < 4; i++) { pat[i] = new Step[16]; for (int s = 0; s < 16; s++) pat[i][s] = new Step(); }
        patLen[0] = 16;

        // -------- 工具条 --------
        int CW = ClientSize.Width, CH = ClientSize.Height;
        Panel top = new Panel(); top.BackColor = Theme.TopBg;
        top.SetBounds(0, 0, CW, Theme.TopH); top.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(top);
        int cA = 16, cB = 336, cC = 700, cD = 1076;

        // ================= 列A: 连接 / 音序操作 / 时基 =================
        Hdr(top, "连接", cA, 8);
        AddLabel(top, "文本串口", cA + 2, 29);
        cboPort.DropDownStyle = ComboBoxStyle.DropDownList; cboPort.SetBounds(cA + 60, 26, 86, 24); LightBox(cboPort); top.Controls.Add(cboPort);
        Btn(top, btnRefresh, "扫描", cA + 150, 26, 44, 24, delegate { RefreshPorts(); });
        Btn(top, btnConnect, "连接", cA + 198, 26, 52, 24, delegate { ToggleConnect(); });
        lblConn.SetBounds(cA + 2, 56, 100, 20); lblConn.BackColor = Color.Transparent; lblConn.Text = "未连接"; lblConn.ForeColor = Theme.Warn; top.Controls.Add(lblConn);
        lblStatus.SetBounds(cA, Theme.TopH - 24, cD - cA - 8, 20); lblStatus.BackColor = Color.Transparent; lblStatus.Text = "就绪"; lblStatus.ForeColor = Theme.Status; top.Controls.Add(lblStatus);

        Hdr(top, "音序操作", cA, 88);
        Btn(top, btnRead, "读回", cA + 2, 106, 54, 24, delegate { BeginRead(); });
        Btn(top, btnWrite, "写入", cA + 60, 106, 54, 24, delegate { WritePattern(); });
        Btn(top, btnPlay, "播放", cA + 118, 106, 50, 24, delegate { PlayPattern(); });
        Btn(top, btnStop, "停止", cA + 172, 106, 50, 24, delegate { StopSeq(); });
        Btn(top, btnClear, "清空", cA + 226, 106, 50, 24, delegate { ClearPico(); });
        AddLabel(top, "时基", cA + 2, 143);
        cboTb.DropDownStyle = ComboBoxStyle.DropDownList; cboTb.SetBounds(cA + 40, 140, 72, 24);
        cboTb.Items.AddRange(new object[] { "10ms", "20ms", "40ms", "80ms", "160ms", "320ms" }); cboTb.SelectedIndex = 0;
        LightBox(cboTb); top.Controls.Add(cboTb);
        cboTb.SelectedIndexChanged += delegate {
            int[] ms = new int[] { 10, 20, 40, 80, 160, 320 };
            int k = cboTb.SelectedIndex; if (k < 0 || k >= ms.Length) return;
            int decim = (int)Math.Round(ms[k] / 10.6667); if (decim < 1) decim = 1; if (decim > 64) decim = 64;
            Send("t" + decim);
            scope.spanSamples = 512;
            scope.Invalidate();
            SetStatus("示波器时基 " + ms[k] + "ms (抽点 x" + decim + ")");
        };

        // ================= 列B: 音序器 / 图片 =================
        Hdr(top, "音序器", cB, 8);
        AddLabel(top, "播放开关", cB + 2, 29);
        for (int i = 0; i < 4; i++)
        {
            int x = cB + 68 + i * 36;
            CheckBox ck = new CheckBox();
            ck.Text = ((char)('A' + i)).ToString(); ck.SetBounds(x, 26, 34, 20);
            ck.ForeColor = Theme.Text; ck.Checked = ((playMask >> i) & 1) != 0;
            int idx = i;
            ck.CheckedChanged += delegate {
                if (suppressChk) return;
                if (chkEn[idx].Checked) playMask |= (1 << idx); else playMask &= ~(1 << idx);
                Send("m" + playMask);
                SetStatus("播放开关 " + "ABCD"[idx] + (chkEn[idx].Checked ? " 开" : " 关") + " (点“写入”保存到 flash)");
            };
            top.Controls.Add(ck); chkEn[i] = ck;
        }
        AddLabel(top, "编辑", cB + 2, 61);
        for (int i = 0; i < 4; i++)
        {
            int x = cB + 40 + i * 36;
            Button pb = new Button(); pb.Text = ((char)('A' + i)).ToString();
            pb.SetBounds(x, 58, 34, 24); StyleToggle(pb);
            int idx2 = i;
            pb.Click += delegate {
                if (follow) { follow = false; UpdateFollowBtn(); }   // 手动切 pattern -> 关跟随
                curPat = idx2; HighlightPat(); cboLen.SelectedIndex = Math.Max(0, Math.Min(15, patLen[curPat] - 1)); RefreshGrid();
            };
            top.Controls.Add(pb); btnPat[i] = pb;
        }
        HighlightPat();
        AddLabel(top, "步数", cB + 2, 95);
        cboLen.DropDownStyle = ComboBoxStyle.DropDownList; cboLen.SetBounds(cB + 40, 92, 46, 24);
        for (int i = 1; i <= 16; i++) cboLen.Items.Add(i.ToString());
        cboLen.SelectedIndex = 15; LightBox(cboLen); top.Controls.Add(cboLen);
        cboLen.SelectedIndexChanged += delegate { patLen[curPat] = 1 + cboLen.SelectedIndex; RefreshGrid(); };
        AddLabel(top, "BPM", cB + 96, 95);
        numBpm.SetBounds(cB + 134, 92, 54, 24); numBpm.Text = "120.0"; numBpm.TextAlign = HorizontalAlignment.Center;
        LightBox(numBpm); top.Controls.Add(numBpm);
        Btn(top, btnFollow, "跟随", cB + 196, 92, 56, 24, delegate {
            follow = !follow; UpdateFollowBtn();
            SetStatus(follow ? "跟随: 自动切到正在播放的 pattern (手动切 pattern 会自动关)" : "跟随: 关");
        });
        UpdateFollowBtn();

        Hdr(top, "音序 / 皮肤", cB, 126);
        Btn(top, btnSaveFile, "存音序", cB + 2, 144, 60, 24, delegate { SaveToFile(); });
        Btn(top, btnLoadFile, "读音序", cB + 66, 144, 60, 24, delegate { LoadFromFile(); });
        Btn(top, btnSkin, "换皮肤", cB + 130, 144, 60, 24, delegate { PickSkin(); });



        // ================= 列C: 音色 / 音频 =================
        Hdr(top, "音色", cC, 8);
        AddLabel(top, "波形", cC + 2, 29);
        swWave.SetBounds(cC + 38, 26, 44, 22);
        swWave.onToggle = delegate(bool on)
        {
            waveSquare = on ? 1 : 0;
            if (sp.IsOpen) Send(on ? "w127" : "w0");
            UpdateWaveState();
            SetStatus("波形: " + (on ? "方波 (锯齿比较导出, 占空比随音高 45→70%)" : "锯齿"));
        };
        top.Controls.Add(swWave);
        lblWaveState.SetBounds(cC + 86, 29, 40, 18); lblWaveState.ForeColor = Theme.Text;
        lblWaveState.BackColor = Color.Transparent; top.Controls.Add(lblWaveState);
        UpdateWaveState();
        chkPots.Text = "电位器"; chkPots.SetBounds(cC + 132, 26, 72, 22); chkPots.ForeColor = Theme.Text; chkPots.Checked = potsOn;
        chkPots.CheckedChanged += delegate {
            potsOn = chkPots.Checked;
            if (sp.IsOpen) Send(potsOn ? "p1" : "p0");
            for (int i = 0; i < 8; i++) bars[i].interactive = !potsOn;
            SetStatus(potsOn ? "电位器开: 参数由硬件电位器控制" : "电位器关: 参数由软件控制 (拖动下面 CUT/RES/... 条)");
        };
        top.Controls.Add(chkPots);
        chkResBass.Text = "低频补偿"; chkResBass.SetBounds(cC + 212, 26, 88, 22); chkResBass.ForeColor = Theme.Text;
        chkResBass.CheckedChanged += delegate {
            if (sp.IsOpen) Send(chkResBass.Checked ? "h213" : "h149");
            SetStatus(chkResBass.Checked ? "谐振低频补偿: 开 (按 res 抬低频)" : "谐振低频补偿: 关 (原版 res 逻辑)");
        };
        top.Controls.Add(chkResBass);
        chkSelfOsc.Text = "自激"; chkSelfOsc.SetBounds(cC + 212, 48, 88, 22); chkSelfOsc.ForeColor = Theme.Text;
        chkSelfOsc.CheckedChanged += delegate {
            if (suppressSelfOsc) return;                        // 由固件回读同步, 不回写
            if (sp.IsOpen) Send(chkSelfOsc.Checked ? "$1" : "$0");
            SetStatus(chkSelfOsc.Checked ? "自激: 开 (res 拉满可自激)" : "自激: 关 (原机行为, 干净不自激)");
        };
        top.Controls.Add(chkSelfOsc);
        chkDistPost.Text = "失真后置"; chkDistPost.SetBounds(cC + 132, 48, 76, 17); chkDistPost.ForeColor = Theme.Text;
        chkDistPost.CheckedChanged += delegate {
            if (suppressDistPost) return;
            distPost = chkDistPost.Checked;
            if (sp.IsOpen) Send(distPost ? "/1" : "/0");
            CfgSet("distpost", distPost ? "1" : "0");
            SetStatus(distPost ? "失真位置: VCA 之后 (TD-3, 音头更咬)" : "失真位置: VCA 之前 (VCF→失真→VCA)");
        };
        top.Controls.Add(chkDistPost);

        Hdr(top, "音频", cC, 64);
        chkAudio.Text = "USB音频";                   // 内部状态, 不在界面显示
        chkAudio.CheckedChanged += delegate {
            if (chkAudio.Checked && !audioOn) StartAudio();
            else if (!chkAudio.Checked && audioOn) StopAudio();
        };
        AddLabel(top, "音频串口", cC + 2, 85);
        cboAudioPort.DropDownStyle = ComboBoxStyle.DropDownList; cboAudioPort.SetBounds(cC + 64, 82, 116, 24);
        LightBox(cboAudioPort); top.Controls.Add(cboAudioPort);
        cboAudioPort.SelectedIndexChanged += delegate {
            if (suppressAudioPort) return;
            chkAudio.Checked = cboAudioPort.SelectedIndex > 0;   // 选了音频口=开, "关闭"=关
        };
        AddLabel(top, "输出", cC + 2, 117);
        cboDev.DropDownStyle = ComboBoxStyle.DropDownList; cboDev.SetBounds(cC + 40, 114, 300, 24);
        LightBox(cboDev); top.Controls.Add(cboDev);
        RefreshDevices();
        AddLabel(top, "监听", cC + 2, 151);
        Label lblVol = new Label(); lblVol.Text = "x8"; lblVol.SetBounds(cC + 222, 151, 30, 18); lblVol.ForeColor = Theme.Value;
        lblVol.BackColor = Color.Transparent; top.Controls.Add(lblVol);
        volBar.SetBounds(cC + 40, 147, 176, 24); volBar.interactive = true; volBar.def = 0.4; volBar.value = 0.4;
        volBar.onSet = delegate(double v) { audioGain = (float)(v * 20.0); lblVol.Text = "x" + (int)Math.Round(v * 20.0); };
        top.Controls.Add(volBar);

        // ---- 录音 (录 USB 音频这一路) ----
        Hdr(top, "录音", cC, 182);
        btnRecStart = new Button();
        Btn(top, btnRecStart, "● 开始录音", cC + 2, 202, 100, 24, delegate { RecToggle(); });
        Btn(top, new Button(), "■ 停止录音", cC + 108, 202, 100, 24, delegate { RecStop(); });
        btnRecPrev = new Button();
        Btn(top, btnRecPrev, "▶ 试听", cC + 2, 230, 100, 24, delegate { RecPreview(); });
        Btn(top, new Button(), "导出 (WAV/MP3)", cC + 108, 230, 100, 24, delegate { RecExport(); });

        // ================= 列D: 硬件 / 旋钮 =================
        Hdr(top, "硬件", cD, 8);
        AddLabel(top, "超频", cD + 2, 29);
        cboOc.DropDownStyle = ComboBoxStyle.DropDownList; cboOc.SetBounds(cD + 38, 26, 100, 24);
        cboOc.Items.AddRange(new object[] { "150MHz/1.10V 原版", "250MHz/1.20V", "300MHz/1.20V", "360MHz/1.30V", "375MHz/1.30V" });
        cboOc.SelectedIndex = 0; LightBox(cboOc); top.Controls.Add(cboOc);
        cboOc.KeyDown += delegate(object s, KeyEventArgs e) {          // 关闭状态方向键不改档 (避免每按一下都复位)
            if (!cboOc.DroppedDown && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down || e.KeyCode == Keys.Left || e.KeyCode == Keys.Right)) e.Handled = true; };
        cboOc.DropDownClosed += delegate {                             // 打开下拉选好后(关闭)才应用 + 复位
            if (suppressOc || !sp.IsOpen) return;
            if (cboOc.SelectedIndex == ocApplied) return;              // 没改就不复位
            ocApplied = cboOc.SelectedIndex;
            Send("K" + cboOc.SelectedIndex);                  // 写入档位 + 复位
            SetStatus("超频档位 -> " + cboOc.Text + " (Pico 复位中, 自动重连...)");
            StartReconnect();
        };
        chkOcUnlock.Text = "解锁超频"; chkOcUnlock.SetBounds(cD + 146, 26, 86, 24); chkOcUnlock.ForeColor = Theme.Text;
        chkOcUnlock.CheckedChanged += delegate {
            ocUnlocked = chkOcUnlock.Checked;
            if (suppressOc) { RebuildOcItems(); return; }          // 程序回读设置时不要回写
            if (sp.IsOpen) Send("+" + (ocUnlocked ? "1" : "0"));
            RebuildOcItems();
            SetStatus(ocUnlocked ? "超频解锁: 开 (可选 >375MHz, 电压最高 1.35V)" : "超频解锁: 关 (最高 375MHz/1.30V)");
        };
        top.Controls.Add(chkOcUnlock);
        chkWdt.Text = "看门狗"; chkWdt.SetBounds(cD + 2, 58, 78, 24); chkWdt.ForeColor = Theme.Text; chkWdt.Checked = true;
        chkWdt.CheckedChanged += delegate {
            if (suppressWdt || !sp.IsOpen) return;
            Send("u" + (chkWdt.Checked ? "1" : "0"));          // 开/关看门狗
            SetStatus("看门狗 " + (chkWdt.Checked ? "开" : "关") + " (掉电后恢复默认开)");
        };
        top.Controls.Add(chkWdt);
        { Label lo = new Label(); lo.Text = "过采样"; lo.SetBounds(cD + 90, 61, 52, 18); lo.ForeColor = Theme.TextDim; lo.BackColor = Color.Transparent; top.Controls.Add(lo); }
        cboFiltOS.DropDownStyle = ComboBoxStyle.DropDownList; cboFiltOS.SetBounds(cD + 146, 58, 54, 24);
        cboFiltOS.Items.AddRange(new object[] { "4x", "8x", "12x", "16x" });
        cboFiltOS.SelectedIndex = 1; LightBox(cboFiltOS); top.Controls.Add(cboFiltOS);
        cboFiltOS.KeyDown += delegate(object s, KeyEventArgs e) {
            if (!cboFiltOS.DroppedDown && (e.KeyCode == Keys.Up || e.KeyCode == Keys.Down || e.KeyCode == Keys.Left || e.KeyCode == Keys.Right)) e.Handled = true; };
        cboFiltOS.DropDownClosed += delegate {
            if (suppressFos || !sp.IsOpen) return;
            if (cboFiltOS.SelectedIndex == fosApplied) return;         // 没改就不复位
            fosApplied = cboFiltOS.SelectedIndex;
            int[] osv = { 4, 8, 12, 16 };
            Send("@" + osv[cboFiltOS.SelectedIndex]);
            SetStatus("滤波过采样 -> " + cboFiltOS.Text + (cboFiltOS.SelectedIndex >= 2 ? " ⚠高负载(12x/16x 可能爆音)" : "") + " (Pico 复位中, 自动重连...)");
            StartReconnect();
        };
        Btn(top, btnReset, "复位", cD + 2, 92, 46, 24, delegate {
            if (!sp.IsOpen) { SetStatus("未连接"); return; }
            Send("z"); SetStatus("板子复位中... 稍后重新连接");
        });
        Btn(top, btnCpuScan, "扫CPU", cD + 54, 92, 58, 24, delegate { ScanCpu(); }); btnCpuScan.BringToFront();
        lblCpu.SetBounds(cD + 118, 95, 160, 18); lblCpu.Text = "CPU -"; lblCpu.ForeColor = Theme.Value;
        lblCpu.BackColor = Color.Transparent; lblCpu.AutoSize = false; lblCpu.TextAlign = ContentAlignment.MiddleLeft;
        lblCpu.Font = new Font("Consolas", 8f); top.Controls.Add(lblCpu); lblCpu.BringToFront();

        Hdr(top, "旋钮", cD, 126);
        {
            Knob[] kn = { kNoise, kVar, kTemp };
            string[] knName = { "噪声", "容差", "温度" };
            for (int i = 0; i < 3; i++)
            {
                int ii = i, kx = cD + 2 + i * 62, ky = 144;
                kn[i].SetBounds(kx, ky, 46, 46);
                kn[i].onSet = delegate(double v) { knobPending[ii] = v; knobDirty[ii] = true; knobVal[ii].Text = ((int)(v * 127)).ToString(); };
                top.Controls.Add(kn[i]);
                Label kl = new Label(); kl.Text = knName[i]; kl.SetBounds(kx - 4, ky + 45, 54, 15); kl.ForeColor = Theme.TextDim; kl.TextAlign = ContentAlignment.MiddleCenter; kl.BackColor = Color.Transparent; top.Controls.Add(kl);
                Label kv = new Label(); kv.Text = "0"; kv.SetBounds(kx - 4, ky + 59, 54, 13); kv.ForeColor = Theme.Value; kv.TextAlign = ContentAlignment.MiddleCenter; kv.BackColor = Color.Transparent; kv.Font = new Font("Consolas", 7f); top.Controls.Add(kv); knobVal[i] = kv;
            }
        }


        oneShotScan.Interval = 5000; oneShotScan.Tick += delegate { oneShotScan.Stop(); ScanCpu(); };

        // -------- 底部监视 (示波器 + 参数) --------
        int monY = Theme.TopH + Theme.StepH, monH = CH - monY;
        Panel mon = new Panel(); mon.BackColor = Theme.MonBg;
        mon.SetBounds(0, monY, CW, monH); mon.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(mon);
        int scopeW = (int)(CW * Theme.ScopeW); if (scopeW < 420) scopeW = 420;
        scope.SetBounds(10, 10, scopeW, monH - 20); mon.Controls.Add(scope); scope.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left;
        int px = 10 + scopeW + 30; if (px > CW - 420) px = CW - 420; if (px < 420) px = 420;
        for (int i = 0; i < 8; i++)
        {
            int yy = 8 + i * ((monH - 16) / 8);
            Label lb = new Label(); lb.Text = barName[i]; lb.SetBounds(px, yy, 44, 18); lb.ForeColor = Theme.TextDim; mon.Controls.Add(lb);
            ParamBar pb = new ParamBar(); pb.SetBounds(px + 46, yy, 246, 18); pb.interactive = !potsOn; pb.def = barDefault[i];
            int ii = i; pb.onSet = delegate(double v) { SetParamFromBar(ii, v); };
            mon.Controls.Add(pb); bars[i] = pb;
            Label vl = new Label(); vl.Text = "-"; vl.SetBounds(px + 300, yy, 120, 18); vl.ForeColor = Theme.Value; mon.Controls.Add(vl); barVal[i] = vl;
            vl.DoubleClick += delegate { if (!potsOn) { double d = barDefault[ii]; bars[ii].value = d; bars[ii].Invalidate(); SetParamFromBar(ii, d); SetStatus(barName[ii] + " 复位"); } };
        }

        // -------- 步进格 --------
        Panel stepPanel = new Panel(); stepPanel.AutoScroll = true; stepPanel.BackColor = Theme.StepBg;
        stepPanel.SetBounds(0, Theme.TopH, CW, Theme.StepH); stepPanel.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(stepPanel);
        BuildGrid(stepPanel);
        // 提示文字: 紧贴音序器(步进格)正下方, 不靠近监视区/示波器
        Label hint = new Label();
        hint.Text =
            "播放: 默认播放已勾选的 pattern, 按 A→B→C→D 顺序循环; 未勾选或空的 pattern 跳过, 直接播下一个可播的。\n" +
            "按钮: 读回 = 从 Pico flash 读回;  写入 = 存到 Pico flash(掉电保留);  播放 = 上传到 RAM 并开始播(不存盘);  停止 = 暂停(再按一次回到开头);  清空 = 清空并保存。\n" +
            "音序器: +/- = 半音,  ▲▼ = 八度(双击音名重置A2),  ACC = 重音,  SLIDE = 滑到下一步,  REST = 休止,  TIE = 延音。";
        hint.SetBounds(8, 6 + STEPH + 4, CW - 40, 54); hint.ForeColor = Theme.TextDim;
        hint.BackColor = Theme.StepBg; hint.AutoSize = false;
        stepPanel.Controls.Add(hint); hint.BringToFront();
        RefreshPorts(); RefreshGrid();
        cboPort.SelectedIndexChanged += delegate { if (!suppressPort && sp.IsOpen) SwitchMainPort(); };       // 用户换文本口 -> 断开重连

        tmr.Interval = 30; tmr.Tick += delegate { uiTickMs = Environment.TickCount; OnTick(); }; tmr.Start();   // 常开: 重连状态机也要跑
        FormClosing += delegate { try { if (audioOn) StopAudio(); Send("H0"); System.Threading.Thread.Sleep(90); serialRun = false; if (serialThr != null) { serialThr.Join(500); serialThr = null; } if (sendThr != null) { sendThr.Join(500); sendThr = null; } if (sp.IsOpen) sp.Close(); } catch {} };

        RegisterElems();                 // 注册可换肤元素 (皮肤可改其位置/大小)
        ApplyElem();                     // 应用皮肤里的元素偏移
        // 背景图片 (可选): 设置后三大区域面板透明, 图片透出来
        if (!string.IsNullOrEmpty(Theme.BgImage)) {
            string bg = Theme.BgImage;
            try { if (!Path.IsPathRooted(bg)) bg = Path.Combine(Theme.BaseDir, bg); } catch {}
            if (File.Exists(bg)) { try {
                BackgroundImage = Image.FromFile(bg);
                BackgroundImageLayout = Theme.BgMode == 1 ? ImageLayout.Tile : (Theme.BgMode == 2 ? ImageLayout.Center : ImageLayout.Stretch);
                top.BackColor = Color.Transparent; stepPanel.BackColor = Color.Transparent; mon.BackColor = Color.Transparent;
            } catch {} }
        }
    }

    void AddLabel(Control parent, string t, int x, int y) { Label l = new Label(); l.Text = t; l.AutoSize = true; l.BackColor = Color.Transparent; l.SetBounds(x, y, 20, 18); l.ForeColor = Theme.TextDim; parent.Controls.Add(l); }
    // 分组小标题 (工具条用)
    void Hdr(Control p, string t, int x, int y) { Label l = new Label(); l.Text = t; l.SetBounds(x, y, 220, 16); l.ForeColor = Theme.Header; l.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold); l.BackColor = Color.Transparent; p.Controls.Add(l); }

    // ============ 换肤 ============
    void Reg(string name, Control c) { if (c == null) return; elems[name] = c; baseRects[name] = new Rectangle(c.Left, c.Top, c.Width, c.Height); }
    void RegisterElems()
    {
        Reg("scope", scope); Reg("volBar", volBar); Reg("port", cboPort); Reg("timebase", cboTb);
        Reg("len", cboLen); Reg("bpm", numBpm); Reg("follow", btnFollow);
        Reg("read", btnRead); Reg("write", btnWrite); Reg("play", btnPlay); Reg("stop", btnStop); Reg("clear", btnClear);
        Reg("save", btnSaveFile); Reg("load", btnLoadFile);
        Reg("audioport", cboAudioPort); Reg("device", cboDev); Reg("pots", chkPots); Reg("resbass", chkResBass); Reg("wave", swWave);
        Reg("oc", cboOc); Reg("unlock", chkOcUnlock); Reg("wdt", chkWdt); Reg("filtos", cboFiltOS); Reg("reset", btnReset); Reg("cpuscan", btnCpuScan); Reg("cpu", lblCpu);
        Reg("knobNoise", kNoise); Reg("knobVar", kVar); Reg("knobTemp", kTemp);
        Reg("conn", lblConn); Reg("status", lblStatus);
        Reg("refresh", btnRefresh); Reg("connect", btnConnect); Reg("skin", btnSkin); Reg("distpost", chkDistPost);
        Reg("patA", btnPat[0]); Reg("patB", btnPat[1]); Reg("patC", btnPat[2]); Reg("patD", btnPat[3]);
        Reg("enA", chkEn[0]); Reg("enB", chkEn[1]); Reg("enC", chkEn[2]); Reg("enD", chkEn[3]);
        suppressDistPost = true;
        distPost = CfgGet("distpost", "1") != "0";   // 读回上次的失真位置 (持久化)
        chkDistPost.Checked = distPost;
        suppressDistPost = false;
    }
    void ApplyElem()
    {
        foreach (KeyValuePair<string, int[]> kv in Theme.Elem)
        {
            Control c; if (!elems.TryGetValue(kv.Key, out c)) continue;
            int[] a = kv.Value;                                        // elem.* = 绝对 位置/大小 (相对父容器)
            c.SetBounds(a[0], a[1], Math.Max(8, a[2]), Math.Max(8, a[3]));
        }
    }
    void PickSkin()
    {
        OpenFileDialog d = new OpenFileDialog();
        d.Filter = "303paint 皮肤 (*.303paint)|*.303paint|所有文件 (*.*)|*.*";
        string sk = Path.Combine(Application.StartupPath, "skins");
        try { Directory.CreateDirectory(sk); } catch { }
        d.InitialDirectory = sk;
        if (d.ShowDialog() != DialogResult.OK) return;
        try { File.WriteAllText(Path.Combine(DataDir("log"), "skin.txt"), d.FileName); } catch { }
        if (MessageBox.Show("皮肤已选择：\n" + d.FileName + "\n\n重启程序后生效。现在就重启？",
            "OV-03 皮肤", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
            Application.Restart();
        else SetStatus("已选择皮肤 " + Path.GetFileName(d.FileName) + " (重启后生效)");
    }
    // ComboBox 深色窗体上闭合显示区会被系统画成黑块; 用 OwnerDraw 强制白底黑字
    void LightBox(Control c) {
        c.BackColor = Theme.InputBg; c.ForeColor = Theme.InputFg;
        ComboBox cb = c as ComboBox;
        if (cb != null) { cb.FlatStyle = FlatStyle.Flat; cb.DrawMode = DrawMode.OwnerDrawFixed; cb.ItemHeight = 16; cb.DrawItem += ComboDraw; cb.KeyPress += delegate(object s, KeyPressEventArgs e) { e.Handled = true; }; }
        NumericUpDown nu = c as NumericUpDown; if (nu != null) nu.BorderStyle = BorderStyle.FixedSingle;
    }
    void ComboDraw(object sender, DrawItemEventArgs e) {
        ComboBox cb = sender as ComboBox; if (cb == null) return;
        bool sel = (e.State & DrawItemState.Selected) == DrawItemState.Selected;
        using (SolidBrush b = new SolidBrush(sel ? Theme.Accent : Theme.InputBg)) e.Graphics.FillRectangle(b, e.Bounds);
        string t = (e.Index >= 0 && e.Index < cb.Items.Count) ? cb.Items[e.Index].ToString() : cb.Text;
        if (!string.IsNullOrEmpty(t)) using (SolidBrush f = new SolidBrush(sel ? Theme.AccentFg : Theme.InputFg)) e.Graphics.DrawString(t, cb.Font, f, e.Bounds.X + 2, e.Bounds.Y + 1);
    }
    void Btn(Control parent, Button b, string t, int x, int y, int w, int h, EventHandler on)
    { b.Text = t; b.SetBounds(x, y, w, h); b.FlatStyle = FlatStyle.Flat; b.BackColor = Theme.BtnBg; b.ForeColor = Theme.Text; b.FlatAppearance.BorderColor = Theme.BtnBorder; b.Click += on; parent.Controls.Add(b); }
    void SetStatus(string s) { lblStatus.Text = s; }
    void StyleToggle(Button b) { b.FlatStyle = FlatStyle.Flat; b.FlatAppearance.BorderColor = Theme.BtnBorder; b.BackColor = Theme.ToggleBg; b.ForeColor = Theme.Text; }
    void HighlightPat() { for (int i = 0; i < 4; i++) { bool on = (i == curPat); btnPat[i].BackColor = on ? Theme.Accent : Theme.BtnBg; btnPat[i].ForeColor = on ? Theme.AccentFg : Theme.Text; } }
    void SetPlayhead(int pat, int step)
    {
        if (pat == playPat && step == playStep) return;
        playPat = pat; playStep = step;
        RefreshPlayhead();
    }
    void RefreshPlayhead()
    {
        for (int i = 0; i < 16; i++) { lblNo[i].BackColor = Color.Transparent; lblNo[i].ForeColor = Theme.StepNo; }
        if (playPat == curPat && playStep >= 0 && playStep < 16)
        { lblNo[playStep].BackColor = Theme.Playhead; lblNo[playStep].ForeColor = Theme.PlayFg; }
    }

    void BuildGrid(Panel stepPanel)
    {
        bAcc = new Button[16]; bSlide = new Button[16]; bRest = new Button[16]; bTie = new Button[16];
        bNoteUp = new Button[16]; bNoteDn = new Button[16]; lblNote = new Label[16]; lblNo = new Label[16];
        bOctDn = new Button[16]; bOctUp = new Button[16];
        // 按面板宽度均分 16 列 (窗口变大时格子自动加宽, 不出现横向滚动)
        int W = stepPanel.ClientSize.Width, gm = 6, gg = 4;
        int sw = (W - gm * 2 - 15 * gg) / 16; if (sw < 40) sw = 40;
        for (int i = 0; i < 16; i++)
        {
            int x = gm + i * (sw + gg);
            Panel pc = new Panel(); pc.SetBounds(x, 6, sw, STEPH); pc.BackColor = Theme.CellBg; stepPanel.Controls.Add(pc);
            Label no = new Label(); no.Text = (i + 1).ToString(); no.SetBounds(0, 2, sw, 18); no.TextAlign = ContentAlignment.MiddleCenter; no.ForeColor = Theme.StepNo; pc.Controls.Add(no); lblNo[i] = no;
            Button up = new Button(); up.Text = "+"; up.SetBounds(4, 24, sw - 8, 24); StyleToggle(up); pc.Controls.Add(up); bNoteUp[i] = up;
            Label nl = new Label(); nl.Text = "A2"; nl.SetBounds(0, 51, sw, 24); nl.TextAlign = ContentAlignment.MiddleCenter; nl.Font = new Font("Segoe UI", 11f, FontStyle.Bold); nl.ForeColor = Theme.Value; pc.Controls.Add(nl); lblNote[i] = nl;
            Button dn = new Button(); dn.Text = "-"; dn.SetBounds(4, 78, sw - 8, 24); StyleToggle(dn); pc.Controls.Add(dn); bNoteDn[i] = dn;
            Button a = new Button(); a.Text = "ACC"; a.SetBounds(4, 110, sw - 8, 20); pc.Controls.Add(a); bAcc[i] = a;
            Button sl = new Button(); sl.Text = "SLIDE"; sl.SetBounds(4, 132, sw - 8, 20); pc.Controls.Add(sl); bSlide[i] = sl;
            Button rr = new Button(); rr.Text = "REST"; rr.SetBounds(4, 154, sw - 8, 20); pc.Controls.Add(rr); bRest[i] = rr;
            Button t = new Button(); t.Text = "TIE"; t.SetBounds(4, 176, sw - 8, 20); pc.Controls.Add(t); bTie[i] = t;
            Button ou = new Button(); ou.Text = "\u25B2"; ou.Font = new Font("Segoe UI", 10f, FontStyle.Bold); ou.SetBounds(4, 198, sw - 8, 20); StyleToggle(ou); pc.Controls.Add(ou); bOctUp[i] = ou;
            Button od = new Button(); od.Text = "\u25BC"; od.Font = new Font("Segoe UI", 10f, FontStyle.Bold); od.SetBounds(4, 220, sw - 8, 20); StyleToggle(od); pc.Controls.Add(od); bOctDn[i] = od;
            StyleToggle(a); StyleToggle(sl); StyleToggle(rr); StyleToggle(t);
            int idx = i;
            nl.DoubleClick += delegate { pat[curPat][idx].note = 45; RefreshGrid(); };   // 双击音名 -> 重置 A2
            up.Click += delegate { pat[curPat][idx].note = Math.Min(96, pat[curPat][idx].note + 1); RefreshGrid(); };
            dn.Click += delegate { pat[curPat][idx].note = Math.Max(21, pat[curPat][idx].note - 1); RefreshGrid(); };
            od.Click += delegate { pat[curPat][idx].note = Math.Max(21, pat[curPat][idx].note - 12); RefreshGrid(); };   // 降一个八度
            ou.Click += delegate { pat[curPat][idx].note = Math.Min(96, pat[curPat][idx].note + 12); RefreshGrid(); };   // 升一个八度
            a.Click  += delegate { Step s = pat[curPat][idx]; s.acc = !s.acc; RefreshGrid(); };
            sl.Click += delegate { Step s = pat[curPat][idx]; s.slide = !s.slide; RefreshGrid(); };
            rr.Click += delegate { Step s = pat[curPat][idx]; s.rest = !s.rest; RefreshGrid(); };
            t.Click  += delegate { Step s = pat[curPat][idx]; s.tie = !s.tie; RefreshGrid(); };
        }
        // 第 8 步后的竖向分隔线 (视觉引导前 8 / 后 8 步)
        Panel div = new Panel(); div.SetBounds(gm + 8 * (sw + gg) - gg / 2 - 1, 6, 2, STEPH);
        div.BackColor = Theme.Divider;
        stepPanel.Controls.Add(div); div.BringToFront();
    }

    void RefreshGrid()
    {
        int len = patLen[curPat]; if (len < 1) len = 1;
        for (int i = 0; i < 16; i++)
        {
            bool on = i < len;
            ((Panel)bNoteUp[i].Parent).BackColor = on ? Theme.CellBg : Theme.CellBgOff;
            bNoteUp[i].Enabled = bNoteDn[i].Enabled = on;
            bOctDn[i].Enabled = bOctUp[i].Enabled = on;
            Step s = pat[curPat][i];
            lblNote[i].Text = s.rest ? "--" : (s.tie ? "TIE" : NoteName(s.note));   // TIE=延长上一音(音高不变)
            lblNote[i].ForeColor = s.rest ? Theme.TextDim : (s.tie ? Theme.NoteTie : Theme.Value);
            SetTgl(bAcc[i], on && s.acc, Theme.OnAcc);
            SetTgl(bSlide[i], on && s.slide, Theme.OnSlide);
            SetTgl(bRest[i], on && s.rest, Theme.OnRest);
            SetTgl(bTie[i], on && s.tie, Theme.OnTie);
            bAcc[i].Enabled = bSlide[i].Enabled = bRest[i].Enabled = bTie[i].Enabled = on;
        }
        RefreshPlayhead();
    }
    void SetTgl(Button b, bool on, Color c) { b.BackColor = on ? c : Theme.ToggleBg; b.ForeColor = on ? Theme.PlayFg : Theme.Text; }

    bool ProbeIsText(string pn)                     // 开一会这个口发 T, 回 "temp=" 就是文本口
    {
        try {
            SerialPort t = new SerialPort(pn, 115200);
            t.ReadTimeout = 300; t.WriteTimeout = 300; t.DtrEnable = true; t.RtsEnable = true;
            t.Open(); t.DiscardInBuffer();
            try { t.Write("T\n"); } catch {}
            System.Threading.Thread.Sleep(450);
            string s = ""; try { s = t.ReadExisting(); } catch {}
            t.Close();
            return s.IndexOf("temp=") >= 0;
        } catch { return false; }
    }
    // 扫描可用串口, 自动识别出"文本口"和"音频口"; 不给用户瞎选 (端口由 Windows 分配)
    void RefreshPorts()
    {
        suppressPort = true; suppressAudioPort = true;
        string held = sp.IsOpen ? sp.PortName : null;
        string[] ps = SerialPort.GetPortNames(); Array.Sort(ps);
        string textPort = held, audioPort = null;
        foreach (string pn in ps) {
            if (pn == held) continue;
            if (textPort == null && ProbeIsText(pn)) { textPort = pn; continue; }   // 未连接才探测
            if (audioPort == null) audioPort = pn;
        }
        cboPort.Items.Clear();
        if (textPort != null) cboPort.Items.Add(textPort); else cboPort.Items.AddRange(ps);
        if (cboPort.Items.Count > 0) cboPort.SelectedIndex = 0;
        int keep = cboAudioPort.SelectedIndex;
        cboAudioPort.Items.Clear(); cboAudioPort.Items.Add("关闭");
        if (audioPort != null) cboAudioPort.Items.Add(audioPort);
        cboAudioPort.SelectedIndex = (keep > 0 && keep < cboAudioPort.Items.Count) ? keep : 0;
        suppressPort = false; suppressAudioPort = false;
        SetStatus("扫描: 文本口=" + (textPort == null ? "无" : textPort) + "   音频口=" + (audioPort == null ? "无" : audioPort) + "   (端口由 Windows 分配)");
    }

    void ToggleConnect()
    {
        if (sp.IsOpen) { Send("H0"); System.Threading.Thread.Sleep(90); serialRun = false; if (serialThr != null) { try { serialThr.Join(500); } catch {} serialThr = null; } if (sendThr != null) { try { sendThr.Join(500); } catch {} sendThr = null; } if (audioOn) StopAudio(); try { sp.Close(); } catch {} lblConn.Text = "未连接"; lblConn.ForeColor = Theme.Warn; SetStatus("已断开 (Pico 恢复本地界面)"); return; }
        try { sp.PortName = cboPort.Text.Trim(); OpenPortAndInit(); SetStatus("PC 模式: Pico 屏幕已停刷 -> 笑脸; 正在读回 pattern..."); }
        catch (Exception ex) { lblConn.Text = "连接失败"; lblConn.ForeColor = Theme.OnAcc; SetStatus(cboPort.Text + " 打不开 (可能被其它程序占用): " + ex.Message); }
    }
    void SwitchMainPort()   // 用户换了文本串口 -> 断开重连
    {
        string want = cboPort.Text.Trim();
        if (sp.IsOpen && sp.PortName == want) return;
        try {
            if (sp.IsOpen) {
                Send("H0"); System.Threading.Thread.Sleep(80);
                serialRun = false;
                if (serialThr != null) { try { serialThr.Join(500); } catch {} serialThr = null; }
                if (sendThr != null) { try { sendThr.Join(500); } catch {} sendThr = null; }
                if (audioOn) StopAudio();
                try { sp.Close(); } catch {}
            }
            reconnectPhase = 0;
            sp.PortName = want;
            OpenPortAndInit();
            SetStatus("已换口并连接 " + sp.PortName);
        } catch (Exception ex) {
            lblConn.Text = "连接失败"; lblConn.ForeColor = Theme.OnAcc;
            SetStatus(want + " 打不开 (可能被其它程序占用): " + ex.Message);
        }
    }

    // 打开端口 + 初始化 (首次连接与自动重连共用)
    bool OpenPortAndInit()
    {
        sp.BaudRate = 115200; sp.Parity = Parity.None; sp.DataBits = 8; sp.StopBits = StopBits.One;
        sp.DtrEnable = true; sp.RtsEnable = true; sp.ReadTimeout = 300; sp.WriteTimeout = 300; sp.NewLine = "\n";
        sp.ReadBufferSize = 262144;
        sp.Open(); tmr.Start();
        connectTimeMs = Environment.TickCount; connectWarned = false;   // 2s 内没收到回复 -> 提示可能选错口
        serialRun = true;
        serialThr = new System.Threading.Thread(SerialPump); serialThr.IsBackground = true; serialThr.Start();
        sendThr = new System.Threading.Thread(SendPump); sendThr.IsBackground = true; sendThr.Start();
        lblConn.Text = "已连接 " + sp.PortName; lblConn.ForeColor = Theme.Status;
        System.Threading.Thread.Sleep(200);
        Send("j0"); System.Threading.Thread.Sleep(80);   // 先清掉上次崩溃/被杀残留的 USB 音频流
        Send("H1"); System.Threading.Thread.Sleep(150);
        Send(potsOn ? "p1" : "p0");
        for (int i = 0; i < 8; i++) bars[i].interactive = !potsOn;
        Send("S0"); System.Threading.Thread.Sleep(120);
        Send(distPost ? "/1" : "/0");       // 失真位置 (持久化设置)
        BeginRead();
        Send("T"); Send("Q");          // 连上立刻读回 设置(超频档/解锁/过采样/三旋钮) + CPU
        RefreshPorts();                // 顺便识别出音频口 (供"音频串口"下拉)
        oneShotScan.Stop(); oneShotScan.Start();
        return true;
    }

    // 复位档位/过采样后: 用 UI 定时器状态机自动重连 (无后台线程/竞态)
    void StartReconnect()
    {
        if (reconnectPhase != 0) return;
        audioBeforeReconnect = audioOn;
        try { prePorts = SerialPort.GetPortNames(); } catch { prePorts = new string[0]; }
        reconnectPhase = 1;
        reconnectAt = Environment.TickCount + 250;
        SetStatus("Pico 复位中 -> 自动重连...");
    }
    bool PortPresent()
    {
        try { foreach (string pn in SerialPort.GetPortNames()) if (pn == sp.PortName) return true; } catch {}
        return false;
    }
    void ReconnectTick()
    {
        if (reconnectPhase == 1 && Environment.TickCount >= reconnectAt)
        {
            // 关掉现有连接 (停音频/串口线程 + 关端口)
            try { audioRun = false; if (audioThr != null) { audioThr.Join(600); audioThr = null; } } catch {}
            try { if (audioReadThr != null) { audioReadThr.Join(600); audioReadThr = null; } } catch {}
            try { if (spAudio.IsOpen) spAudio.Close(); } catch {}
            try { serialRun = false; if (serialThr != null) { serialThr.Join(400); serialThr = null; } if (sendThr != null) { sendThr.Join(400); sendThr = null; } } catch {}
            try { CloseWave(); } catch {}
            try { if (sp.IsOpen) sp.Close(); } catch {}
            audioOn = false; serialDead = false;
            lblConn.Text = "重连中..."; lblConn.ForeColor = Theme.Playhead;
            reconnectPhase = 2; reconnectAt = Environment.TickCount + 6000;    // 最多等端口消失 6s
        }
        else if (reconnectPhase == 2)
        {
            // 先等端口消失 (Pico 正在重启)
            if (!PortPresent()) { reconnectPhase = 3; reconnectAt = Environment.TickCount + 12000; }
            else if (Environment.TickCount >= reconnectAt) { reconnectPhase = 3; reconnectAt = Environment.TickCount + 12000; }
        }
        else if (reconnectPhase == 3)
        {
            // 再等端口回来 (原端口, 或复位后新出现的端口)
            string found = null;
            try
            {
                string[] now = SerialPort.GetPortNames();
                foreach (string pn in now) if (pn == sp.PortName) { found = pn; break; }
                if (found == null) foreach (string pn in now)
                {
                    bool was = false; foreach (string q in prePorts) if (q == pn) { was = true; break; }
                    if (!was) { found = pn; break; }
                }
            }
            catch {}
            if (found != null) { sp.PortName = found; reconnectPhase = 4; reconnectAt = Environment.TickCount + 500; }
            else if (Environment.TickCount >= reconnectAt) { reconnectPhase = 0; OnSerialLost(); SetStatus("自动重连失败, 请手动点【连接】"); }
        }
        else if (reconnectPhase == 4 && Environment.TickCount >= reconnectAt)
        {
            reconnectPhase = 0;
            try {
                OpenPortAndInit();
                if (audioBeforeReconnect || chkAudio.Checked) {          // 重连后把 USB 音频自动重开
                    if (!chkAudio.Checked) chkAudio.Checked = true;       // 勾选已掉 -> 勾上(触发 StartAudio)
                    else StartAudio();                                    // 勾选还在 -> 直接重开
                }
                SetStatus("已自动重连 " + sp.PortName + (audioOn ? " (USB音频已重开)" : ""));
            }
            catch (Exception ex) { OnSerialLost(); SetStatus("重连失败: " + ex.Message); }
        }
    }

    void Send(string s) { lock (sendLock) { if (sendQ.Count < 512) sendQ.Enqueue(s); } }   // 非阻塞: 交给后台发送线程

    void SendPump()                                  // 后台: 从队列取命令写串口 (写阻塞也只堵本线程)
    {
        while (serialRun)
        {
            cSend++;
            string s = null;
            lock (sendLock) { if (sendQ.Count > 0) s = sendQ.Dequeue(); }
            if (s == null) { System.Threading.Thread.Sleep(3); continue; }
            try { if (sp.IsOpen) sp.Write(s + "\n"); } catch {}
        }
    }

    void OnSerialLost()
    {
        if (lostThr != null && lostThr.IsAlive) return;
        audioOn = false;
        lostThr = new System.Threading.Thread(delegate () {         // 后台收尾: sp.Close()/waveOut 可能阻塞, 不能冻 UI
            try { audioRun = false; if (audioThr != null) { audioThr.Join(1000); audioThr = null; } } catch {}
            try { if (audioReadThr != null) { audioReadThr.Join(600); audioReadThr = null; } } catch {}
            try { if (spAudio.IsOpen) spAudio.Close(); } catch {}
            try { serialRun = false; if (serialThr != null) { serialThr.Join(800); serialThr = null; } if (sendThr != null) { sendThr.Join(800); sendThr = null; } } catch {}
            try { CloseWave(); } catch {}
            try { if (sp.IsOpen) sp.Close(); } catch {}
            audioOn = false;
            try { this.BeginInvoke((MethodInvoker)delegate {
                lblConn.Text = "未连接"; lblConn.ForeColor = Theme.Warn;
                SetStatus("串口断开 (Pico 复位/拔线), 点连接重连");
            }); } catch {}
        });
        lostThr.IsBackground = true; lostThr.Start();
    }

    // ---- 输出设备枚举 ----
    void RefreshDevices()
    {
        int cur = cboDev.SelectedIndex;
        cboDev.Items.Clear();
        cboDev.Items.Add("系统默认");
        int nd = waveOutGetNumDevs();
        for (int i = 0; i < nd; i++)
        {
            WAVEOUTCAPS c = new WAVEOUTCAPS();
            if (waveOutGetDevCaps((IntPtr)i, ref c, Marshal.SizeOf(typeof(WAVEOUTCAPS))) == 0)
                cboDev.Items.Add((i + 1) + ": " + c.szPname);
        }
        cboDev.SelectedIndex = (cur >= 0 && cur < cboDev.Items.Count) ? cur : 0;
    }
    int SelectedDevId() { int i = cboDev.SelectedIndex; return i <= 0 ? WAVE_MAPPER : (i - 1); }   // 0=系统默认

    // ---- 打开/关闭 waveOut (48k/16bit/mono, 8 个轮转缓冲, 无回调靠 dwFlags 轮询) ----
    bool OpenWave(int devId)
    {
        WAVEFORMATEX wf = new WAVEFORMATEX();
        wf.wFormatTag = 1; wf.nChannels = 1; wf.nSamplesPerSec = 48000;
        wf.nAvgBytesPerSec = 96000; wf.nBlockAlign = 2; wf.wBitsPerSample = 16; wf.cbSize = 0;
        if (waveOutOpen(out hWave, devId, ref wf, IntPtr.Zero, IntPtr.Zero, 0) != 0) { hWave = IntPtr.Zero; return false; }
        for (int i = 0; i < AUD_NBUF; i++)
        {
            audHdr[i] = Marshal.AllocHGlobal(WAVHDR_SIZE);
            audBuf[i] = Marshal.AllocHGlobal(AUD_BUFSZ);
            for (int k = 0; k < WAVHDR_SIZE; k++) Marshal.WriteByte(audHdr[i], k, 0);
            Marshal.WriteIntPtr(audHdr[i], 0, audBuf[i]);      // lpData
            waveOutPrepareHeader(hWave, audHdr[i], WAVHDR_SIZE);
            audBusy[i] = false;
        }
        return true;
    }
    void CloseWave()
    {
        if (hWave == IntPtr.Zero) return;
        try { waveOutReset(hWave); } catch {}
        for (int i = 0; i < AUD_NBUF; i++)
        {
            if (audHdr[i] != IntPtr.Zero) { try { waveOutUnprepareHeader(hWave, audHdr[i], WAVHDR_SIZE); } catch {} Marshal.FreeHGlobal(audHdr[i]); audHdr[i] = IntPtr.Zero; }
            if (audBuf[i] != IntPtr.Zero) { Marshal.FreeHGlobal(audBuf[i]); audBuf[i] = IntPtr.Zero; }
        }
        try { waveOutClose(hWave); } catch {}
        hWave = IntPtr.Zero;
    }

    // ---- USB 音频回传: 音频走独立第 2 路 CDC (裸 PCM), 与文本/示波器物理分开 ----
    void StartAudio()
    {
        if (audioRun && audioThr != null && audioThr.IsAlive) return;   // 已在运行: 防重复起线程(会叠加/交织成噪声)
        if (!sp.IsOpen) { SetStatus("未连接"); chkAudio.Checked = false; return; }
        if (!OpenWave(SelectedDevId())) { SetStatus("打开音频输出设备失败 (换一个设备试试)"); chkAudio.Checked = false; return; }
        if (!OpenAudioPort()) { SetStatus("找不到音频口 (设备应有第 2 个 COM 口)"); chkAudio.Checked = false; CloseWave(); try { Send("j0"); } catch {} return; }
        audioOn = true; audioRun = true;
        lock (audioLock) audioQ.Clear();
        audioThr = new System.Threading.Thread(AudioPump); audioThr.IsBackground = true; audioThr.Start();
        audioReadThr = new System.Threading.Thread(AudioReadPump); audioReadThr.IsBackground = true; audioReadThr.Start();
        SetStatus("USB 音频开 -> " + cboDev.Text + "  (音频口 " + audioPortName + ")");
    }
    // 打开第 2 路 CDC (音频): 排除主口; 逐口 开(拉DTR) -> 发 j1 -> 读到数据才发现
    bool OpenAudioPort()
    {
        try { if (spAudio.IsOpen) spAudio.Close(); } catch {}
        List<string> cands = new List<string>();
        string want = (cboAudioPort.SelectedIndex > 0) ? cboAudioPort.Text : null;   // "自动"则留空
        if (want != null && want != sp.PortName) cands.Add(want);
        string[] ports = SerialPort.GetPortNames(); Array.Sort(ports);
        foreach (string pn in ports) if (pn != sp.PortName && pn != want) cands.Add(pn);
        foreach (string pn in cands)
        {
            try
            {
                spAudio.PortName = pn; spAudio.BaudRate = 115200; spAudio.ReadTimeout = 300;
                spAudio.ReadBufferSize = 65536; spAudio.DtrEnable = true; spAudio.RtsEnable = true;
                spAudio.Open();
                Send("j1"); System.Threading.Thread.Sleep(400);     // 开音频, 等第 2 路开始喷 PCM
                int got = 0; byte[] t = new byte[8192];
                try { got = spAudio.Read(t, 0, t.Length); } catch {}
                if ((got & 1) != 0) { try { spAudio.Read(t, 0, 1); } catch {} }   // 丢弃偶数个字节, 保证 16-bit 采样对齐
                if (got > 200) { audioPortName = pn; return true; }
                Send("j0"); System.Threading.Thread.Sleep(60);
                spAudio.Close();
            }
            catch (Exception ex)
            {
                try { spAudio.Close(); } catch {}
                SetStatus(pn + " 打不开 (可能被其它程序占用): " + ex.Message);
            }
        }
        return false;
    }
    void AudioReadPump()                          // 独立线程: 从音频口读裸 PCM -> 队列
    {
        byte[] tmp = new byte[8192];
        while (audioRun)
        {
            if (!spAudio.IsOpen) { System.Threading.Thread.Sleep(5); continue; }
            int n = 0;
            try { n = spAudio.Read(tmp, 0, tmp.Length); } catch { }
            if (n > 0) FeedAudio(tmp, n);
            else System.Threading.Thread.Sleep(1);
        }
    }
    // 独立线程: 把队列里的 PCM 填进 waveOut 缓冲轮转输出 (音频设备慢也不会卡 UI)
    void AudioPump()
    {
        byte[] tmp = new byte[AUD_BUFSZ];
        byte[] carry = null; int carryOff = 0;
        byte alignByte = 0; bool alignHas = false;   // 跨缓冲保留半个采样: 监听增益必须按 16-bit 对齐, 否则相邻字节错配成噪声
        while (audioRun)
        {
            cAudio++;
            if (hWave == IntPtr.Zero) break;
            try {
            for (int i = 0; i < AUD_NBUF; i++)
                if (audBusy[i] && (Marshal.ReadInt32(audHdr[i], 24) & WHDR_DONE) != 0) audBusy[i] = false;
            int bi = -1; for (int i = 0; i < AUD_NBUF; i++) if (!audBusy[i]) { bi = i; break; }
            if (bi < 0) { System.Threading.Thread.Sleep(2); continue; }
            int got = 0;
            if (alignHas) { tmp[got++] = alignByte; alignHas = false; }   // 补回上一块留下的半个采样
            while (got < AUD_BUFSZ)
            {
                if (carry == null) { lock (audioLock) { if (audioQ.Count > 0) { carry = audioQ.Dequeue(); audioBytes -= carry.Length; } } if (carry == null) break; }
                int cp = Math.Min(carry.Length - carryOff, AUD_BUFSZ - got);
                Buffer.BlockCopy(carry, carryOff, tmp, got, cp); carryOff += cp; got += cp;
                if (carryOff >= carry.Length) { carry = null; carryOff = 0; }
            }
            if ((got & 1) != 0) { alignByte = tmp[got - 1]; alignHas = true; got--; }   // 缓冲长度保持偶数: 不把半个采样切给下一块(否则错位)
            if (got == 0) { Array.Clear(tmp, 0, AUD_BUFSZ); got = AUD_BUFSZ; }   // 队列空: 补静音保持 waveOut 连续 (避免欠载"拉链/方波"噪声)
            float g = audioGain;                       // 监听增益 (带限幅)
            if (g != 1.0f)
            {
                for (int k = 0; k + 1 < got; k += 2)
                {
                    int v = (short)(tmp[k] | (tmp[k + 1] << 8));
                    int s = (int)(v * g);
                    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                    tmp[k] = (byte)(s & 0xFF); tmp[k + 1] = (byte)((s >> 8) & 0xFF);
                }
            }
            Marshal.Copy(tmp, 0, audBuf[bi], got);
            Marshal.WriteInt32(audHdr[bi], 8, got);             // dwBufferLength
            int fl = Marshal.ReadInt32(audHdr[bi], 24); Marshal.WriteInt32(audHdr[bi], 24, fl & ~WHDR_DONE);
            waveOutWrite(hWave, audHdr[bi], WAVHDR_SIZE);
            audBusy[bi] = true;
            } catch { System.Threading.Thread.Sleep(5); }
        }
    }
    void StopAudio()
    {
        audioOn = false; audioRun = false;
        if (sp.IsOpen) { Send("j0"); System.Threading.Thread.Sleep(80); }
        bool stopped = true;
        if (audioThr != null) { try { stopped = audioThr.Join(1000); } catch {} audioThr = null; }
        if (audioReadThr != null) { try { audioReadThr.Join(600); } catch {} audioReadThr = null; }
        try { if (spAudio.IsOpen) spAudio.Close(); } catch {}
        lock (audioLock) audioQ.Clear();
        if (stopped) CloseWave();   // 音频线程未退出 -> 不释放缓冲(防闪退)
        try { sp.DiscardInBuffer(); } catch {}
        SetStatus("USB 音频关");
    }
    void EnsureAudioOff() { if (audioOn || chkAudio.Checked) chkAudio.Checked = false; }  // 需要文本回复的操作前先关音频
    void FeedAudio(byte[] b, int n)
    {
        lastAudioMs = Environment.TickCount;
        if (recOn) { lock (recLock) { if (recMs != null) { recMs.Write(b, 0, n); recBytes += n; } } }   // 录音: 存原始 PCM
        byte[] c = new byte[n]; Buffer.BlockCopy(b, 0, c, 0, n);
        lock (audioLock)
        {
            audioQ.Enqueue(c); audioBytes += n;
            while (audioBytes > 32768 && audioQ.Count > 1) { audioBytes -= audioQ.Dequeue().Length; }   // 上限 32KB (~0.34s), 满了丢最旧
        }
    }
    // ================= 录音 =================
    static void WriteWav(string path, byte[] pcm)
    {
        int dataLen = pcm.Length;
        using (FileStream fs = new FileStream(path, FileMode.Create, FileAccess.Write))
        using (BinaryWriter w = new BinaryWriter(fs))
        {
            w.Write(new char[] { 'R', 'I', 'F', 'F' }); w.Write(36 + dataLen);
            w.Write(new char[] { 'W', 'A', 'V', 'E' });
            w.Write(new char[] { 'f', 'm', 't', ' ' }); w.Write(16); w.Write((short)1); w.Write((short)1);
            w.Write(48000); w.Write(48000 * 2); w.Write((short)2); w.Write((short)16);
            w.Write(new char[] { 'd', 'a', 't', 'a' }); w.Write(dataLen); w.Write(pcm);
        }
    }
    Button btnRecStart = null, btnRecPrev = null;   // 录音按钮(变色) / 试听按钮
    static byte[] NormalizePcm(byte[] pcm)         // 归一化到 ~ -1dBFS (不削波)
    {
        int n = pcm.Length / 2; int peak = 0;
        for (int i = 0; i < n; i++) { int v = (short)(pcm[i * 2] | (pcm[i * 2 + 1] << 8)); int a = v < 0 ? -v : v; if (a > peak) peak = a; }
        if (peak <= 0) return pcm;
        double g = (32767.0 * 0.89) / peak;        // 目标 ~ -1dBFS
        if (g > 6.0) g = 6.0;                       // 增益上限 +15.6dB (别过度放大底噪)
        byte[] o = new byte[pcm.Length];
        for (int i = 0; i < n; i++) { int v = (short)(pcm[i * 2] | (pcm[i * 2 + 1] << 8)); int s = (int)(v * g); if (s > 32767) s = 32767; else if (s < -32768) s = -32768; o[i * 2] = (byte)(s & 0xFF); o[i * 2 + 1] = (byte)((s >> 8) & 0xFF); }
        return o;
    }
    static string NextRecName(string dir)          // OV-03 demo 01.wav -> 02 -> ... 100
    {
        int max = 0;
        try { foreach (string f in Directory.GetFiles(dir, "OV-03 demo *.wav")) { string b = Path.GetFileNameWithoutExtension(f); int sp = b.LastIndexOf(' '); int v; if (sp >= 0 && int.TryParse(b.Substring(sp + 1), out v) && v > max) max = v; } } catch {}
        return Path.Combine(dir, "OV-03 demo " + (max + 1).ToString("D2") + ".wav");
    }
    void SetRecButton(bool on)
    {
        if (btnRecStart == null) return;
        btnRecStart.Text = on ? "● 录音中" : "● 开始录音";
        btnRecStart.BackColor = on ? Color.FromArgb(200, 40, 40) : Theme.BtnBg;
        btnRecStart.ForeColor = on ? Color.White : Theme.Text;
    }
    void RecStart()
    {
        if (!audioOn) { SetStatus("先开 USB 音频(音频栏)才能录"); return; }
        PvStop();
        lock (recLock) { recMs = new MemoryStream(); recBytes = 0; }
        recOn = true; SetRecButton(true); SetStatus("● 录音中... (再点一次=停止)");
    }
    void RecStop()
    {
        if (!recOn) return;
        recOn = false; SetRecButton(false);
        byte[] pcm;
        lock (recLock) { if (recMs == null) return; pcm = recMs.ToArray(); recMs.Dispose(); recMs = null; }
        if (pcm.Length < 1600) { SetStatus("录音太短, 已丢弃"); return; }
        lastRecPcm = NormalizePcm(pcm);
        SetStatus("■ 已录制 " + (lastRecPcm.Length / (48000 * 2)) + "s (可试听 / 导出)");
    }
    void RecToggle() { if (recOn) RecStop(); else RecStart(); }
    // ---- 试听播放器 (waveOut: 播放 / 暂停 / 回到开头) ----
    byte[] pvPcm = null; IntPtr pvH = IntPtr.Zero; int pvPos = 0;
    volatile bool pvGo = false, pvPaused = false;
    long pvLastClick = 0;                        // 自己判定双击 (400ms 内两次)
    IntPtr[] pvHdr = new IntPtr[AUD_NBUF]; IntPtr[] pvBuf = new IntPtr[AUD_NBUF]; bool[] pvBusy = new bool[AUD_NBUF];
    System.Threading.Thread pvThr = null;
    static byte[] ReadWavPcm(string path)
    {
        byte[] all = File.ReadAllBytes(path); int i = 12;
        while (i + 8 <= all.Length) { int id = BitConverter.ToInt32(all, i); int sz = BitConverter.ToInt32(all, i + 4);
            if (id == 0x61746164) { byte[] d = new byte[sz]; Buffer.BlockCopy(all, i + 8, d, 0, sz); return d; } i += 8 + sz + (sz & 1); }
        return null;
    }
    void PvStop()
    {
        pvGo = false;
        if (pvH != IntPtr.Zero) { try { waveOutReset(pvH); } catch {} }
        if (pvThr != null) { try { pvThr.Join(400); } catch {} pvThr = null; }
        for (int i = 0; i < AUD_NBUF; i++) { if (pvHdr[i] != IntPtr.Zero) { try { if (pvH != IntPtr.Zero) waveOutUnprepareHeader(pvH, pvHdr[i], WAVHDR_SIZE); } catch {} Marshal.FreeHGlobal(pvHdr[i]); pvHdr[i] = IntPtr.Zero; } if (pvBuf[i] != IntPtr.Zero) { Marshal.FreeHGlobal(pvBuf[i]); pvBuf[i] = IntPtr.Zero; } pvBusy[i] = false; }
        if (pvH != IntPtr.Zero) { try { waveOutClose(pvH); } catch {} pvH = IntPtr.Zero; }
    }
    void RecPreview()                                  // 单击: 播放 / 暂停(停在当前) / 播完重播; 双击(400ms内): 回开头
    {
        long nowMs = Environment.TickCount;
        if (nowMs - pvLastClick < 200) { pvLastClick = 0; RecPreviewRewind(); return; }   // 双击(200ms内) = 回开头
        pvLastClick = nowMs;
        if (lastRecPcm == null || lastRecPcm.Length < 2) { SetStatus("还没有录音"); return; }
        if (pvH != IntPtr.Zero)
        {
            if (pvPcm != null && pvPos >= pvPcm.Length) { try { waveOutReset(pvH); pvPos = 0; pvPaused = false; waveOutRestart(pvH); } catch {} SetStatus("▶ 重放"); return; }
            try { if (pvPaused) { waveOutRestart(pvH); pvPaused = false; SetStatus("▶ 继续"); } else { waveOutPause(pvH); pvPaused = true; SetStatus("⏸ 暂停 (双击=回开头)"); } } catch {}
            return;
        }
        pvPcm = lastRecPcm;
        WAVEFORMATEX wf = new WAVEFORMATEX(); wf.wFormatTag = 1; wf.nChannels = 1; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 96000; wf.cbSize = 0;
        if (waveOutOpen(out pvH, WAVE_MAPPER, ref wf, IntPtr.Zero, IntPtr.Zero, 0) != 0) { pvH = IntPtr.Zero; SetStatus("打开音频失败"); return; }
        for (int i = 0; i < AUD_NBUF; i++) { pvBuf[i] = Marshal.AllocHGlobal(AUD_BUFSZ); pvHdr[i] = Marshal.AllocHGlobal(WAVHDR_SIZE); for (int z = 0; z < WAVHDR_SIZE; z++) Marshal.WriteByte(pvHdr[i], z, 0); Marshal.WriteIntPtr(pvHdr[i], 0, pvBuf[i]); waveOutPrepareHeader(pvH, pvHdr[i], WAVHDR_SIZE); pvBusy[i] = false; }
        pvPos = 0; pvGo = true; pvPaused = false;
        pvThr = new System.Threading.Thread(PvFeeder); pvThr.IsBackground = true; pvThr.Start();
        SetStatus("▶ 试听 (单击暂停, 双击回开头)");
    }
    void RecPreviewRewind()                            // 双击: 回到开头
    {
        if (pvH == IntPtr.Zero) return;
        try { waveOutReset(pvH); pvPos = 0; pvPaused = false; waveOutRestart(pvH); } catch {}
        SetStatus("⏮ 回到开头");
    }
    void PvFeeder()
    {
        while (pvGo)
        {
            if (pvH == IntPtr.Zero) break;
            try {
                if (pvPaused) { System.Threading.Thread.Sleep(5); continue; }
                for (int i = 0; i < AUD_NBUF; i++) if (pvBusy[i] && (Marshal.ReadInt32(pvHdr[i], 24) & WHDR_DONE) != 0) pvBusy[i] = false;
                int bi = -1; for (int i = 0; i < AUD_NBUF; i++) if (!pvBusy[i]) { bi = i; break; }
                if (bi < 0) { System.Threading.Thread.Sleep(2); continue; }
                int n = Math.Min(AUD_BUFSZ, pvPcm.Length - pvPos);
                if (n <= 0) { System.Threading.Thread.Sleep(20); continue; }
                Marshal.Copy(pvPcm, pvPos, pvBuf[bi], n); pvPos += n;
                Marshal.WriteInt32(pvHdr[bi], 8, n);
                int fl = Marshal.ReadInt32(pvHdr[bi], 24); Marshal.WriteInt32(pvHdr[bi], 24, fl & ~WHDR_DONE);
                waveOutWrite(pvH, pvHdr[bi], WAVHDR_SIZE); pvBusy[bi] = true;
            } catch { System.Threading.Thread.Sleep(5); }
        }
    }
    static string FfmpegPath()
    {
        string[] cand = { Path.Combine(Application.StartupPath, "tools", "ffmpeg.exe") };
        foreach (string c in cand) if (File.Exists(c)) return c;
        return null;
    }
    void RecExport()
    {
        if (lastRecPcm == null || lastRecPcm.Length < 2) { SetStatus("还没有录音"); return; }
        string dir = DataDir("records");
        SaveFileDialog d = new SaveFileDialog();
        d.Filter = "WAV (*.wav)|*.wav|MP3 (*.mp3)|*.mp3";
        d.FileName = Path.GetFileNameWithoutExtension(NextRecName(dir));
        d.InitialDirectory = dir;
        if (d.ShowDialog() != DialogResult.OK) return;
        try
        {
            if (d.FileName.ToLower().EndsWith(".mp3"))
            {
                string ff = FfmpegPath();
                if (ff == null) { SetStatus("找不到 ffmpeg, 只能导出 WAV"); return; }
                string tmp = Path.Combine(Path.GetTempPath(), "ov03_rec.wav");
                WriteWav(tmp, lastRecPcm);
                var psi = new System.Diagnostics.ProcessStartInfo(ff, "-y -i \"" + tmp + "\" -codec:a libmp3lame -qscale:a 2 \"" + d.FileName + "\"");
                psi.UseShellExecute = false; psi.CreateNoWindow = true;
                System.Diagnostics.Process pr = System.Diagnostics.Process.Start(psi); pr.WaitForExit();
                try { File.Delete(tmp); } catch {}
            }
            else WriteWav(d.FileName, lastRecPcm);
            lastRecPath = d.FileName;
            SetStatus("已导出: " + Path.GetFileName(d.FileName));
        }
        catch (Exception ex) { SetStatus("导出失败: " + ex.Message); }
    }

    void BeginRead()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        if (reconnectPhase != 0) { SetStatus("正在重连, 请稍候再读回"); return; }
        // 注意: 不要在这里关 USB 音频 (之前会误关且不恢复); 文本与音频帧解析器已能共存
        reading = true; dump.Clear(); readStartMs = Environment.TickCount; Send("U");
    }

    void OnTick()
    {
        try {
        uiOp = "OnTick";
        if (reconnectPhase != 0) { uiOp = "ReconnectTick"; ReconnectTick(); uiOp = "idle"; return; }
        if (!sp.IsOpen) { uiOp = "idle"; return; }
        if (serialDead) { serialDead = false; uiOp = "OnSerialLost"; OnSerialLost(); uiOp = "idle"; return; }
        if (!connectWarned && Environment.TickCount - connectTimeMs > 2000)
        { connectWarned = true; SetStatus("该串口无响应: 可能选成了“音频串口”! 请把文本串口改成另一个 COM (通常更小的那个)"); }
        FlushBars();                                   // 合并发送拖动的参数
        FlushKnobs();                                  // 合并发送三个旋钮
        uiOp = "drain";
        for (int dn = 0; dn < 300; dn++)               // 每 tick 最多 300 行, 防文本积压冻 UI
        {
            string line = null;
            lock (textLock) { if (textQ.Count > 0) line = textQ.Dequeue(); }
            if (line == null) break;
            try { ProcessLine(line); } catch {}
        }
        lock (textLock) { if (textQ.Count > 20000) textQ.Clear(); }   // 积压过大直接丢弃
        uiOp = "readtimeout";
        if (reading && Environment.TickCount - readStartMs > 3000) { reading = false; ParseDump(); }   // 3s 超时 (原 2s 偶发不够)
        uiOp = "idle";
        } catch (Exception ex) { Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " OnTick\n" + ex.ToString() + "\n\n"); }
    }
    // 独立线程: 持续读串口 -> 解析音频帧(喂 waveOut) / 文本行(入队给 UI)。与 UI 拖动/点击解耦, 不再因 UI 忙而丢帧。
    void SerialPump()
    {
        while (serialRun)
        {
            cSerial++;
            if (!sp.IsOpen) { serialDead = true; System.Threading.Thread.Sleep(10); continue; }
            int avail = 0; try { avail = sp.BytesToRead; } catch { serialDead = true; }
            if (avail <= 0) { System.Threading.Thread.Sleep(2); continue; }
            int len = Math.Min(avail, 32768);
            byte[] tmp = new byte[len];
            int n = 0; try { n = sp.Read(tmp, 0, len); } catch { serialDead = true; }
            if (n <= 0) continue;
            try { AppendRaw(tmp, n); ParseRaw(); } catch { }   // 解析异常不能杀死读取线程 (否则永久收不到数据)
        }
    }
    // 混合协议解析: 音频帧 FF AA <len:u16> <PCM>  与  ASCII 文本行 共存
    byte[] rxRaw = new byte[65536];
    int rxRawLen = 0;
    void AppendRaw(byte[] b, int n)
    {
        if (rxRawLen + n > rxRaw.Length)
        {
            int keep = Math.Min(rxRawLen, 8192);
            Buffer.BlockCopy(rxRaw, rxRawLen - keep, rxRaw, 0, keep);
            rxRawLen = keep;
        }
        Buffer.BlockCopy(b, 0, rxRaw, rxRawLen, n); rxRawLen += n;
    }
    void DropRaw(int k)
    {
        if (k <= 0) return;
        if (k >= rxRawLen) { rxRawLen = 0; return; }
        Buffer.BlockCopy(rxRaw, k, rxRaw, 0, rxRawLen - k); rxRawLen -= k;
    }
    void ParseRaw()
    {
        while (rxRawLen > 0)
        {
            if (rxRaw[0] == 0xFF)
            {
                if (rxRawLen < 2) return;
                if (rxRaw[1] != 0xAA) { DropRaw(1); continue; }   // 失步, 丢一字节
                if (rxRawLen < 4) return;
                int len = rxRaw[2] | (rxRaw[3] << 8);
                if (len <= 0 || len > 512) { DropRaw(1); continue; }
                if (rxRawLen < 4 + len) return;
                if (audioOn)
                {
                    byte[] pcm = new byte[len];
                    Buffer.BlockCopy(rxRaw, 4, pcm, 0, len);
                    FeedAudio(pcm, len);
                }
                DropRaw(4 + len);
                continue;
            }
            int nl = -1;
            for (int i = 0; i < rxRawLen; i++) if (rxRaw[i] == (byte)'\n') { nl = i; break; }   // W 示波器行很长, 必须等到 '\n'
            if (nl < 0) return;
            string line = Encoding.ASCII.GetString(rxRaw, 0, nl).Replace("\r", "").Trim();
            DropRaw(nl + 1);
            lock (textLock) { textQ.Enqueue(line); }    // 交给 UI 线程 ProcessLine
        }
    }

    void ProcessLine(string l)
    {
        if (l.Length == 0) return;
        if (l[0] == 'K' && l.IndexOf(',') > 0) { ParseK(l.Substring(1)); return; }
        if (l[0] == 'W' && l.IndexOf(',') > 0) { ParseW(l); return; }
        if (l.StartsWith("core1=")) { ParseCpuLine(l); return; }   // Q 命令回复
        if (l.StartsWith("temp="))  { ParseTempLine(l); return; }  // T 命令回复
        if (reading)
        {
            dump.Add(l);
            if (l.StartsWith("#END")) { reading = false; ParseDump(); }
        }
        if (l == "#SAVED") SetStatus("已写入 flash");
        else if (l == "#CLEARED") SetStatus("已清空 Pico pattern");
    }

    void ParseK(string s)
    {
        string[] p = s.Split(',');
        if (p.Length < 6) return;
        double cut, res, env, dec, acc, tune;
        double.TryParse(p[0], NumberStyles.Any, CultureInfo.InvariantCulture, out cut);
        double.TryParse(p[1], NumberStyles.Any, CultureInfo.InvariantCulture, out res);
        double.TryParse(p[2], NumberStyles.Any, CultureInfo.InvariantCulture, out env);
        double.TryParse(p[3], NumberStyles.Any, CultureInfo.InvariantCulture, out dec);
        double.TryParse(p[4], NumberStyles.Any, CultureInfo.InvariantCulture, out acc);
        double.TryParse(p[5], NumberStyles.Any, CultureInfo.InvariantCulture, out tune);
        SetBar(0, CutToU(cut) * 100.0, ((int)cut).ToString() + " Hz");
        SetBar(1, res, ((int)res).ToString() + " %");
        SetBar(2, clamp01(env / 100.0) * 100.0, ((int)env).ToString() + " %");
        SetBar(3, clamp01((dec - 200.0) / 2300.0) * 100.0, ((int)dec).ToString() + " ms");
        SetBar(4, acc / 255.0 * 100.0, ((int)acc).ToString());
        SetBar(5, (tune + 12) / 24.0 * 100.0, tune.ToString("0.0", CultureInfo.InvariantCulture) + " st");
        if (p.Length >= 10)
        {
            double dist;
            double.TryParse(p[9], NumberStyles.Any, CultureInfo.InvariantCulture, out dist);
            SetBar(6, dist / 255.0 * 100.0, ((int)dist).ToString());
        }
        if (p.Length >= 11)
        {
            int col;   // 固件 color 0..255 -> COLOR 显示条
            if (int.TryParse(p[10], out col)) SetBar(7, col / 255.0 * 100.0, col.ToString());
        }
        if (p.Length >= 12)
        {
            int wv;    // 固件 wave 0..255 -> 波形开关状态
            if (int.TryParse(p[11], out wv)) { waveSquare = wv >= 128 ? 1 : 0; UpdateWaveState(); }
        }
        if (p.Length >= 13)
        {
            int so;    // 固件 selfOsc 0/1 -> 自激勾选框状态 (含 V1.2 SW2 物理开关回读)
            if (int.TryParse(p[12], out so)) { suppressSelfOsc = true; chkSelfOsc.Checked = (so != 0); suppressSelfOsc = false; }
        }
        if (p.Length >= 9)
        {
            int pp, ps, pr;
            if (int.TryParse(p[6], out pp) && int.TryParse(p[7], out ps) && int.TryParse(p[8], out pr))
            {
                playRun = pr != 0;
                SetPlayhead(pp, ps);
                if (follow && pp != curPat) {                         // 跟随: 切到正在播放的 pattern
                    curPat = pp; HighlightPat();
                    cboLen.SelectedIndex = Math.Max(0, Math.Min(15, patLen[curPat] - 1));
                    RefreshGrid();
                }
            }
        }
    }
    void SetBar(int i, double pct, string txt)
    {
        if (bars[i].held) return;                  // 拖动中: 不让 K 行回写, 免得打架乱跳
        double v = pct / 100.0; if (v < 0) v = 0; if (v > 1) v = 1;
        bars[i].value = v; bars[i].Invalidate(); barVal[i].Text = txt;
    }
    static double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    static double AudioTaper(double u) { return (Math.Pow(10.0, 2.0 * u) - 1.0) / 99.0; }            // B->A 对数锥 (与固件 audioTaper 一致)
    static double AudioTaperInv(double t) { double x = 99.0 * t + 1.0; if (x < 1e-9) x = 1e-9; return Math.Log10(x) / 2.0; }
    static double CutToU(double hz) { if (hz < 1) return 0; return clamp01(Math.Log(hz / CUT_MIN_HZ) / Math.Log(CUT_MAX_HZ / CUT_MIN_HZ)); }
    string BarValueText(int i, double v)
    {
        switch (i)
        {
            case 0: return (CUT_MIN_HZ * Math.Pow(CUT_MAX_HZ / CUT_MIN_HZ, v)).ToString("0", CultureInfo.InvariantCulture) + " Hz";
            case 1: return (v * 100).ToString("0", CultureInfo.InvariantCulture) + " %";
            case 2: return (v * 100).ToString("0", CultureInfo.InvariantCulture) + " %";
            case 3: return (200.0 + 2300.0 * v).ToString("0", CultureInfo.InvariantCulture) + " ms";
            case 4: return ((int)(v * 127)).ToString();
            case 5: return (v * 24 - 12).ToString("0.0", CultureInfo.InvariantCulture) + " st";
            case 6: return ((int)(v * 127)).ToString();
            case 7: return ((int)(v * 127)).ToString();
        }
        return "";
    }
    void SetParamFromBar(int i, double v)          // 拖动: 只更新显示 + 打标记, 由 30ms 定时器合并发送 (避免刷串口卡死)
    {
        if (potsOn) return;
        barPending[i] = v; barDirty[i] = true;
        barVal[i].Text = BarValueText(i, v);
        SetStatus(barName[i] + " = " + BarValueText(i, v));
    }
    void FlushBars()                               // 每个 30ms 最多各发一条, 合流
    {
        if (potsOn || !sp.IsOpen) return;
        for (int i = 0; i < 8; i++)
        {
            if (!barDirty[i]) continue;
            barDirty[i] = false;
            double v = barPending[i];
            switch (i)
            {
                case 0: Send("c" + (CUT_MIN_HZ * Math.Pow(CUT_MAX_HZ / CUT_MIN_HZ, v)).ToString("0", CultureInfo.InvariantCulture)); break;
                case 1: Send("r" + (v * 100).ToString("0", CultureInfo.InvariantCulture)); break;
                case 2: Send("e" + (v * 100).ToString("0", CultureInfo.InvariantCulture)); break;
                case 3: Send("d" + (200.0 + 2300.0 * v).ToString("0", CultureInfo.InvariantCulture)); break;
                case 4: Send("x" + (v * 127).ToString("0", CultureInfo.InvariantCulture)); break;
                case 5: Send("o" + (v * 24 - 12).ToString("0.0", CultureInfo.InvariantCulture)); break;
                case 6: Send("a" + (v * 127).ToString("0", CultureInfo.InvariantCulture)); break;
                case 7: Send("q" + (v * 127).ToString("0", CultureInfo.InvariantCulture)); break;
            }
        }
    }

    void FlushKnobs()                              // 合并发送三个旋钮 (噪声/容差/温度)
    {
        if (!sp.IsOpen) return;
        for (int i = 0; i < 3; i++)
        {
            if (!knobDirty[i]) continue;
            knobDirty[i] = false;
            Send(knobPre[i] + ((int)(knobPending[i] * 127)).ToString(CultureInfo.InvariantCulture));
        }
    }

    void RebuildOcItems()                          // 按解锁状态/固件表重建超频下拉 (含电压)
    {
        string[] raw = ocTable;
        if (raw == null) raw = new string[] { "150/1100", "250/1200", "300/1200", "360/1300", "375/1300", "396/1350", "408/1350", "432/1350", "456/1350" };
        int maxIdx = ocUnlocked ? raw.Length - 1 : Math.Min(ocMaxLock, raw.Length - 1);
        if (maxIdx < 0) maxIdx = 0;
        int keep = cboOc.SelectedIndex;
        suppressOc = true;
        cboOc.Items.Clear();
        for (int i = 0; i <= maxIdx; i++)
        {
            string[] mv = raw[i].Split('/');
            string lbl = mv[0] + "MHz";
            if (mv.Length >= 2) { double mV = 0; double.TryParse(mv[1], NumberStyles.Any, CultureInfo.InvariantCulture, out mV); if (mV > 0) lbl += "/" + (mV / 1000.0).ToString("0.00", CultureInfo.InvariantCulture) + "V"; }
            if (i == 0) lbl += " 原版";
            cboOc.Items.Add(lbl);
        }
        if (keep < 0 || keep > maxIdx) keep = 0;
        cboOc.SelectedIndex = keep;
        suppressOc = false;
    }

    // ---- CPU 占用率显示 (不常扫: 连接后 5s 扫一次 + 按钮手动扫一次) ----
    void ScanCpu()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        Send("Q"); System.Threading.Thread.Sleep(120);   // Q -> core1/core0
        Send("T"); System.Threading.Thread.Sleep(40);    // T -> 主频/温度/超频档
        SetStatus("读取 Pico CPU 占用...");
    }
    static double Tok(string s, string key)
    {
        int i = s.IndexOf(key); if (i < 0) return -1;
        i += key.Length; int j = i;
        while (j < s.Length && (char.IsDigit(s[j]) || s[j] == '.' || s[j] == '-' || s[j] == '+')) j++;
        double v; return double.TryParse(s.Substring(i, j - i), NumberStyles.Any, CultureInfo.InvariantCulture, out v) ? v : -1;
    }
    void ParseCpuLine(string l)
    {
        double c1 = Tok(l, "core1="), c0 = Tok(l, "core0=");
        if (c1 >= 0) cpuC1 = c1; if (c0 >= 0) cpuC0 = c0;
        UpdateCpuLabel();
    }
    void ParseTempLine(string l)
    {
        connectWarned = true;                        // 收到固件回复 -> 文本口正确
        double t = Tok(l, "temp="), m = Tok(l, "cpuMHz="), o = Tok(l, "ocIdx="), f = Tok(l, "fails="), w = Tok(l, "wdt=");
        if (t >= 0) cpuTemp = t; if (m >= 0) cpuMHz = m; if (o >= 0) cpuOcIdx = o; if (f >= 0) cpuFails = f;
        // 固件上报本机超频档位表 "oc=<MHz>/<mV>,..." -> 生成下拉标签 (RP2350/RP2040 自适应)
        int ci = l.IndexOf("oc=");
        if (ci >= 0)
        {
            string list = l.Substring(ci + 3).Trim().Split(' ')[0];
            if (list.Length > 0 && list != ocSig) { ocSig = list; ocTable = list.Split(','); RebuildOcItems(); }
        }
        double ul = Tok(l, "ocunlock="), om = Tok(l, "ocmax="), fl = Tok(l, "flash=");
        if (om >= 0) ocMaxLock = (int)om;
        if (ul >= 0)
        {
            ocUnlocked = (ul != 0);
            suppressOc = true; chkOcUnlock.Checked = ocUnlocked; suppressOc = false;
            RebuildOcItems();
        }
        double fo = Tok(l, "filtOS=");
        if (fo >= 0) { suppressFos = true; cboFiltOS.SelectedIndex = fo <= 4 ? 0 : fo <= 8 ? 1 : fo <= 12 ? 2 : 3; suppressFos = false; fosApplied = cboFiltOS.SelectedIndex; }
        double up = Tok(l, "up=");
        if (up >= 0) { if (lastUp >= 0 && up < lastUp) Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " Pico复位 (up " + (long)lastUp + " -> " + (long)up + ")\n"); lastUp = up; }
        // 读回 Pico 里持久保存的 噪声/容差/温度 旋钮值 (只显示, 不回写)
        double nz = Tok(l, "noise="), vr = Tok(l, "var="), tp = Tok(l, "tempk=");
        if (nz >= 0) { kNoise.value = nz; kNoise.Invalidate(); knobVal[0].Text = ((int)(nz * 127)).ToString(); }
        if (vr >= 0) { kVar.value = vr; kVar.Invalidate(); knobVal[1].Text = ((int)(vr * 127)).ToString(); }
        if (tp >= 0) { kTemp.value = tp; kTemp.Invalidate(); knobVal[2].Text = ((int)(tp * 127)).ToString(); }
        if (o >= 0 && o < cboOc.Items.Count) { suppressOc = true; cboOc.SelectedIndex = (int)o; suppressOc = false; ocApplied = (int)o; }
        if (w >= 0) { suppressWdt = true; chkWdt.Checked = (w != 0); suppressWdt = false; }
        UpdateCpuLabel();
        SetStatus(string.Format(CultureInfo.InvariantCulture,
            "Pico CPU: core1={0:0.#}% core0={1:0.#}%  {2:0}MHz  ocIdx={3:0} fails={4:0}  wdt={5}  temp={6:0.#}C  flash={7:0.0}MB",
            cpuC1, cpuC0, cpuMHz, cpuOcIdx, cpuFails, (w != 0 ? "on" : "off"), cpuTemp, fl >= 0 ? fl / 1048576.0 : 0.0));
    }
    void UpdateCpuLabel()
    {
        if (cpuC1 < 0 && cpuC0 < 0) { lblCpu.Text = "CPU -"; return; }
        lblCpu.Text = "C1 " + (cpuC1 >= 0 ? cpuC1.ToString("0") : "?") + "%  C0 " + (cpuC0 >= 0 ? cpuC0.ToString("0") : "?") + "%";
    }
    void UpdateFollowBtn()
    {
        btnFollow.BackColor = follow ? Color.MediumSeaGreen : Theme.BtnBg;
        btnFollow.ForeColor = follow ? Theme.PlayFg : Theme.Text;
    }
    void UpdateWaveState()
    {
        swWave.on = waveSquare != 0; swWave.Invalidate();
        lblWaveState.Text = waveSquare != 0 ? "方波" : "锯齿";
    }

    int[] wparse = new int[1024];              // 复用: 直接解析数字, 不 Split (省 512 字符串/帧 的 GC)
    void ParseW(string s)
    {
        int n = 0, val = 0; bool neg = false, inNum = false;
        for (int i = 1; i < s.Length; i++)     // 跳过首字符 'W'
        {
            char ch = s[i];
            if (ch == ',') { if (inNum) { if (n < wparse.Length) wparse[n++] = neg ? -val : val; inNum = false; val = 0; neg = false; } }
            else if (ch == '-') { neg = true; inNum = true; }
            else if (ch >= '0' && ch <= '9') { inNum = true; val = val * 10 + (ch - '0'); if (val > 100000000) val = 100000000; }
        }
        if (inNum && n < wparse.Length) wparse[n++] = neg ? -val : val;
        if (n < 2) return;
        scope.SetData(wparse, n);
    }

    void ParseDump()
    {
        for (int i = 0; i < 4; i++) patLen[i] = 0;
        bool got = false;
        foreach (string ln in dump)
        {
            string l = ln;
            if (l.StartsWith("#303PAT"))
            {
                got = true;
                int bi = l.IndexOf("bpm=");
                if (bi >= 0) { string sub = l.Substring(bi + 4).Split(' ')[0]; double b; if (double.TryParse(sub, NumberStyles.Any, CultureInfo.InvariantCulture, out b) && b > 0) numBpm.Text = b.ToString("0.0", CultureInfo.InvariantCulture); }
                int mi = l.IndexOf("mask=");
                if (mi >= 0) { string sub = l.Substring(mi + 5).Split(' ')[0]; int mk; if (int.TryParse(sub, out mk) && mk >= 0 && mk <= 15) playMask = mk; }
            }
            else if (l.StartsWith("P "))
            {
                string[] p = l.Split(' ');
                if (p.Length >= 3) { int pi; if (int.TryParse(p[1], out pi) && pi >= 0 && pi < 4) { int L; int.TryParse(p[2], out L); patLen[pi] = L; } }
            }
            else if (l.StartsWith("S "))
            {
                string[] p = l.Split(' ');
                if (p.Length >= 5)
                {
                    int pi, si, note, fl;
                    if (int.TryParse(p[1], out pi) && int.TryParse(p[2], out si) && int.TryParse(p[3], out note) && int.TryParse(p[4], out fl))
                        if (pi >= 0 && pi < 4 && si >= 0 && si < 16 && patLen[pi] >= 1)   // 空 pattern 不覆盖默认
                        {
                            Step st = pat[pi][si]; st.note = note;
                            st.acc = (fl & 1) != 0; st.slide = (fl & 2) != 0; st.rest = (fl & 4) != 0; st.tie = (fl & 8) != 0;
                        }
                }
            }
        }
        suppressChk = true;
        for (int i = 0; i < 4; i++) chkEn[i].Checked = ((playMask >> i) & 1) != 0;
        suppressChk = false;
        cboLen.SelectedIndex = Math.Max(0, Math.Min(15, patLen[curPat] - 1));
        HighlightPat();
        RefreshGrid();
        Send("S1");                                  // 恢复示波器流
        SetStatus(got ? "已读回 flash 里的 pattern (读回即验证)" : "读回失败/超时");
    }

    void SyncPattern(bool persist)
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        Send("S0"); System.Threading.Thread.Sleep(160);          // 暂停示波器流, 命令才不会被淹没
        Send("C"); System.Threading.Thread.Sleep(80);            // 清空 (仅 RAM)
        for (int pi = 0; pi < 4; pi++)
        {
            if (patLen[pi] < 1) continue;
            Send("l" + pi + "," + patLen[pi]);
            for (int si = 0; si < patLen[pi]; si++)
            {
                Step s = pat[pi][si];
                int fl = (s.acc ? 1 : 0) | (s.slide ? 2 : 0) | (s.rest ? 4 : 0) | (s.tie ? 8 : 0);
                Send("P" + pi + "," + si + "," + s.note + "," + fl);
                System.Threading.Thread.Sleep(3);
            }
        }
        double bpm = 120.0; double.TryParse(numBpm.Text, NumberStyles.Any, CultureInfo.InvariantCulture, out bpm);
        Send("G" + bpm.ToString("0.0", CultureInfo.InvariantCulture));
        Send("m" + playMask);
        System.Threading.Thread.Sleep(80);
        if (persist) { Send("W"); System.Threading.Thread.Sleep(400); }   // 唯一的 flash 写 (保持在 S0, 由读回流程收尾)
        else { Send("S1"); }                                            // 仅上传 RAM: 恢复示波器流
        Send("X0");                                                     // 上传/写入后保持停止 (不自动播放)
    }

    void PlayPattern()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        SyncPattern(false);                                      // 把当前编辑上传到 Pico RAM (不写 flash)
        Send("X1");                                              // 从当前步(播放头)开始
        playRun = true;
        SetStatus("播放: 从当前步开始 (已上传当前编辑)");
    }

    void StopSeq()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        if (playRun) { Send("X0"); playRun = false; SetStatus("暂停: 停在该步 (再按一次“停止”回到开头)"); }
        else         { Send("X2"); playPat = 0; playStep = 0; RefreshPlayhead(); SetStatus("已回到开头 (第 1 步)"); }
    }

    void WritePattern()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        SetStatus("写入中...");
        SyncPattern(true);
        SetStatus("已写入, 正在读回验证...");
        BeginRead();                                             // 读回验证 (ParseDump 结束时会恢复 S1)
    }

    void ClearPico()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        Send("S0"); System.Threading.Thread.Sleep(160);
        Send("C"); System.Threading.Thread.Sleep(80);
        Send("m" + playMask);
        Send("W"); System.Threading.Thread.Sleep(400);
        Send("S1");
        for (int i = 0; i < 4; i++) { patLen[i] = 0; for (int s = 0; s < 16; s++) { Step st = pat[i][s]; st.acc = st.slide = st.rest = st.tie = false; st.note = 45; } }
        patLen[curPat] = 16; cboLen.SelectedIndex = 15;
        RefreshGrid(); SetStatus("已清空 Pico pattern");
    }

    // ---- 存文件 / 读文件 (把 4 个 pattern 存到电脑, 免手写) ----
    void SaveToFile()
    {
        SaveFileDialog d = new SaveFileDialog();
        d.Filter = "303 pattern (*.303)|*.303|文本 (*.txt)|*.txt|所有文件 (*.*)|*.*";
        d.FileName = "pattern.303"; d.InitialDirectory = DataDir("pattern");
        if (d.ShowDialog() != DialogResult.OK) return;
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("#303SEQ 1");
        double bpm; double.TryParse(numBpm.Text, NumberStyles.Any, CultureInfo.InvariantCulture, out bpm);
        sb.AppendLine("bpm " + bpm.ToString("0.0", CultureInfo.InvariantCulture));
        sb.AppendLine("mask " + playMask);
        for (int pi = 0; pi < 4; pi++)
        {
            sb.AppendLine("len " + pi + " " + patLen[pi]);
            for (int si = 0; si < 16; si++)
            {
                Step s = pat[pi][si];
                int fl = (s.acc ? 1 : 0) | (s.slide ? 2 : 0) | (s.rest ? 4 : 0) | (s.tie ? 8 : 0);
                sb.AppendLine("P " + pi + " " + si + " " + s.note + " " + fl);
            }
        }
        try { File.WriteAllText(d.FileName, sb.ToString(), Encoding.UTF8); SetStatus("已保存音序: " + d.FileName); }
        catch (Exception ex) { SetStatus("保存失败: " + ex.Message); }
    }

    void LoadFromFile()
    {
        OpenFileDialog d = new OpenFileDialog();
        d.Filter = "303 pattern (*.303)|*.303|文本 (*.txt)|*.txt|所有文件 (*.*)|*.*";
        d.InitialDirectory = DataDir("pattern");
        if (d.ShowDialog() != DialogResult.OK) return;
        LoadPatternFile(d.FileName);
    }

    // 从指定文件加载音序 (「读音序」按钮 / 首次启动自动加载 都用这个)
    void LoadPatternFile(string path)
    {
        for (int i = 0; i < 4; i++) { patLen[i] = 0; for (int s = 0; s < 16; s++) { Step st = pat[i][s]; st.note = 45; st.acc = st.slide = st.rest = st.tie = false; } }
        int loadedMask = playMask;
        try
        {
            char[] ws = new char[] { ' ', '\t' };
            foreach (string raw in File.ReadAllLines(path))
            {
                string l = raw.Trim(); if (l.Length == 0 || l[0] == '#') continue;
                string[] t = l.Split(ws, StringSplitOptions.RemoveEmptyEntries);
                if (t.Length == 0) continue;
                if (t[0] == "bpm" && t.Length >= 2) numBpm.Text = t[1];
                else if (t[0] == "mask" && t.Length >= 2) { int mk; if (int.TryParse(t[1], out mk)) loadedMask = mk & 15; }
                else if (t[0] == "len" && t.Length >= 3) { int pi, ln; if (int.TryParse(t[1], out pi) && int.TryParse(t[2], out ln) && pi >= 0 && pi < 4) patLen[pi] = Math.Max(0, Math.Min(16, ln)); }
                else if (t[0] == "P" && t.Length >= 5)
                {
                    int pi, si, nt, fl;
                    if (int.TryParse(t[1], out pi) && int.TryParse(t[2], out si) && int.TryParse(t[3], out nt) && int.TryParse(t[4], out fl))
                        if (pi >= 0 && pi < 4 && si >= 0 && si < 16)
                        {
                            Step s = pat[pi][si]; s.note = (byte)nt;
                            s.acc = (fl & 1) != 0; s.slide = (fl & 2) != 0; s.rest = (fl & 4) != 0; s.tie = (fl & 8) != 0;
                        }
                }
            }
            playMask = loadedMask;
            suppressChk = true;
            for (int i = 0; i < 4; i++) chkEn[i].Checked = ((playMask >> i) & 1) != 0;
            suppressChk = false;
            cboLen.SelectedIndex = Math.Max(0, Math.Min(15, patLen[curPat] - 1));
            RefreshGrid();
            SetStatus("已加载音序: " + path + "  (再按「播放」或「写入」送到 Pico)");
        }
        catch (Exception ex) { SetStatus("加载失败: " + ex.Message); }
    }

    // 首次启动: 自动加载内置音序 Emotinium.303; 之后不再自动加载 (靠 log\firstrun.done 标记)
    void FirstRunLoad()
    {
        string flag = Path.Combine(DataDir("log"), "firstrun.done");
        if (File.Exists(flag)) return;
        try { File.WriteAllText(flag, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")); } catch {}
        string p = Path.Combine(DataDir("pattern"), "Emotinium.303");
        if (File.Exists(p)) LoadPatternFile(p);
    }

    // ---- 上传图片到 Pico 屏幕 (存 flash, PC 模式显示) ----
    void PickAndUploadImage()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        if (reconnectPhase != 0) { SetStatus("正在重连, 请稍候"); return; }
        OpenFileDialog d = new OpenFileDialog();
        d.Filter = "图片 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)|*.png;*.jpg;*.jpeg;*.bmp;*.gif|所有文件 (*.*)|*.*";
        d.InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        if (d.ShowDialog() != DialogResult.OK) return;
        byte[] data; int mode; string err;
        if (!BuildImageData(d.FileName, chkDither.Checked, out data, out mode, out err)) { SetStatus("处理图片失败: " + err); return; }
        SetStatus("上传图片中... (" + (mode == 0 ? "点阵图" : "彩色") + ", " + data.Length + " 字节)");
        bool wasAudio = audioOn;
        System.Threading.Thread t = new System.Threading.Thread(delegate () { UploadWorker(data, mode, wasAudio); });
        t.IsBackground = true; t.Start();
    }

    // 任意图片缩放到 240x240: 点阵(彩色像素化) 或 平滑(彩色)。都发 RGB565。
    bool BuildImageData(string path, bool dither, out byte[] data, out int mode, out string err)
    {
        data = null; mode = 1; err = "";
        try
        {
            Bitmap dst = new Bitmap(240, 240);
            using (Bitmap src = new Bitmap(path))
            {
                if (dither)
                {
                    // 点阵/像素画: 先缩到 120x120, 再最近邻放大到 240x240 (保留原色, 每格 2x2)
                    int G = 120;
                    Bitmap small = new Bitmap(G, G);
                    using (Graphics gs = Graphics.FromImage(small))
                    {
                        gs.Clear(Color.Black);
                        gs.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        double s = Math.Min((double)G / src.Width, (double)G / src.Height);
                        int w = Math.Max(1, (int)(src.Width * s)), h = Math.Max(1, (int)(src.Height * s));
                        gs.DrawImage(src, (G - w) / 2, (G - h) / 2, w, h);
                    }
                    using (Graphics g = Graphics.FromImage(dst))
                    {
                        g.Clear(Color.Black);
                        g.InterpolationMode = InterpolationMode.NearestNeighbor;
                        g.PixelOffsetMode = PixelOffsetMode.Half;
                        g.DrawImage(small, 0, 0, 240, 240);
                    }
                    small.Dispose();
                }
                else
                {
                    using (Graphics g = Graphics.FromImage(dst))
                    {
                        g.Clear(Color.Black);
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        double s = Math.Min(240.0 / src.Width, 240.0 / src.Height);
                        int w = Math.Max(1, (int)(src.Width * s)), h = Math.Max(1, (int)(src.Height * s));
                        g.DrawImage(src, (240 - w) / 2, (240 - h) / 2, w, h);
                    }
                }
            }
            data = new byte[240 * 240 * 2];
            int i = 0;
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 240; x++)
                {
                    Color c = dst.GetPixel(x, y);
                    int v = ((c.R >> 3) << 11) | ((c.G >> 2) << 5) | (c.B >> 3);
                    data[i++] = (byte)(v & 0xFF); data[i++] = (byte)(v >> 8);
                }
            mode = 1;
            dst.Dispose();
        }
        catch (Exception ex) { err = ex.Message; return false; }
        return true;
    }

    void WriteLineRaw(string s) { try { sp.Write(s + "\n"); } catch {} }
    bool WaitForMarker(string marker, int ms)
    {
        int deadline = Environment.TickCount + ms;
        StringBuilder sb = new StringBuilder();
        byte[] buf = new byte[2048];
        while (Environment.TickCount < deadline)
        {
            int avail = 0; try { avail = sp.BytesToRead; } catch {}
            if (avail > 0)
            {
                int n = 0; try { n = sp.Read(buf, 0, Math.Min(avail, buf.Length)); } catch {}
                if (n > 0)
                {
                    for (int i = 0; i < n; i++) sb.Append((char)buf[i]);
                    if (sb.ToString().IndexOf(marker) >= 0) return true;
                    if (sb.Length > 8192) sb.Remove(0, sb.Length - 4096);
                }
            }
            else System.Threading.Thread.Sleep(2);
        }
        return false;
    }

    void UploadWorker(byte[] data, int mode, bool wasAudio)
    {
        try
        {
            if (audioOn)
            {
                try { BeginInvoke((MethodInvoker)delegate { if (chkAudio.Checked) chkAudio.Checked = false; }); } catch {}
                for (int i = 0; i < 30 && audioOn; i++) System.Threading.Thread.Sleep(50);
            }
            serialRun = false;
            try { if (serialThr != null) { serialThr.Join(1000); serialThr = null; } } catch {}
            try { if (sendThr != null) { sendThr.Join(600); sendThr = null; } } catch {}
            try { sp.DiscardInBuffer(); } catch {}
            int oldWT = sp.WriteTimeout; sp.WriteTimeout = 5000;
            WriteLineRaw("H1"); System.Threading.Thread.Sleep(150);
            WriteLineRaw("S0"); System.Threading.Thread.Sleep(80);
            WriteLineRaw("I0"); System.Threading.Thread.Sleep(80);
            try { sp.DiscardInBuffer(); } catch {}
            WriteLineRaw("^" + mode + "," + data.Length);
            if (!WaitForMarker("#IMGRDY", 12000)) throw new Exception("无 #IMGRDY");
            int off = 0;
            while (off < data.Length)
            {
                int n = Math.Min(4096, data.Length - off);
                sp.Write(data, off, n); off += n;
                if (off >= data.Length) { if (!WaitForMarker("#IMGOK", 20000)) throw new Exception("末尾无 #IMGOK"); }
                else { if (!WaitForMarker("#IMGC", 12000)) throw new Exception("分块超时"); }
            }
            sp.WriteTimeout = oldWT;
            try { BeginInvoke((MethodInvoker)delegate { SetStatus("图片已上传 (" + (mode == 0 ? "点阵图" : "彩色") + "), 重连中..."); StartReconnect(); }); } catch {}
        }
        catch (Exception ex)
        {
            try { if (sp.IsOpen) sp.DiscardInBuffer(); } catch {}
            try { BeginInvoke((MethodInvoker)delegate { SetStatus("上传失败: " + ex.Message); }); } catch {}
        }
        finally
        {
            serialRun = true;
            serialThr = new System.Threading.Thread(SerialPump); serialThr.IsBackground = true; serialThr.Start();
            if (wasAudio) { try { BeginInvoke((MethodInvoker)delegate { if (!chkAudio.Checked) chkAudio.Checked = true; }); } catch {} }
        }
    }

    public void AutoConnect(string port)   // 命令行参数: 启动后自动连接该串口并读回
    {
        if (sp.IsOpen) return;
        RefreshPorts();
        int idx = cboPort.FindStringExact(port);
        if (idx >= 0) cboPort.SelectedIndex = idx;
        ToggleConnect();
    }

    [STAThread]
    static void Main(string[] args)
    {
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += delegate(object s, System.Threading.ThreadExceptionEventArgs e) {
            Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " UI异常\n" + e.Exception.ToString() + "\n\n"); };
        AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e) {
            Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " 未处理\n" + (e.ExceptionObject != null ? e.ExceptionObject.ToString() : "null") + "\n\n"); };
        Crash("===== 启动 " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " =====\n");
        // 单实例: 换皮肤用 Application.Restart(), 旧进程可能还没退 -> 等一下(最多 5s) 再判定, 避免误报
        _instMutex = new System.Threading.Mutex(false, "OV-03_App");   // 供安装程序检测; 同时和 303paint 互斥(串口冲突)
        bool madeOurs = false;
        try { madeOurs = _instMutex.WaitOne(0); if (!madeOurs) madeOurs = _instMutex.WaitOne(5000); }
        catch (System.Threading.AbandonedMutexException) { madeOurs = true; }   // 旧进程被强杀 -> 视为可用
        if (!madeOurs) { MessageBox.Show("OV-03 已经打开了。", "OV-03", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        try { System.Threading.Mutex o = System.Threading.Mutex.OpenExisting("303paint_App"); o.Close();
              MessageBox.Show("303paint 正在运行。两者不能同时打开（串口会冲突），请先关闭 303paint。", "OV-03", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        catch { }
        // 载入皮肤: log\skin.txt 里记的用户皮肤优先, 否则 skins\default.303paint, 否则内置默认值
        try {
            string appDir = Application.StartupPath, userSkin = null;
            try { string sf = Path.Combine(appDir, "log", "skin.txt"); if (File.Exists(sf)) userSkin = File.ReadAllText(sf).Trim(); } catch {}
            if (userSkin != null && userSkin.Length > 0 && File.Exists(userSkin)) Theme.Load(userSkin);
            else { string def = Path.Combine(appDir, "skins", "default.303paint"); if (File.Exists(def)) Theme.Load(def); }
        } catch {}
        Application.EnableVisualStyles();
        MainForm f = new MainForm();
        f.FirstRunLoad();               // 首次启动自动加载内置音序 Emotinium.303 (之后不再自动加载)
        string port = null;
        foreach (string a in args)
        {
            if (a == "--audio") f.argAudio = true;
            else if (a == "--play") f.argPlay = true;
            else if (a.StartsWith("--dev=")) { int dv; if (int.TryParse(a.Substring(6), out dv)) f.argDev = dv; }
            else port = a;
        }
        if (port != null)
        {
            string pp = port;
            f.Shown += delegate
            {
                f.AutoConnect(pp);
                if (f.argDev >= 0 && f.argDev < f.cboDev.Items.Count) f.cboDev.SelectedIndex = f.argDev;
                if (f.argAudio)
                {
                    Timer t = new Timer(); t.Interval = 1500;
                    t.Tick += delegate
                    {
                        t.Stop(); f.chkAudio.Checked = true;
                        if (f.argPlay)
                        {
                            Timer t2 = new Timer(); t2.Interval = 1500;
                            t2.Tick += delegate { t2.Stop(); f.PlayPattern(); };
                            t2.Start();
                        }
                    };
                    t.Start();
                }
            };
        }
        // UI 卡死检测 (后台线程): UI 定时器 >3s 没刷新 -> 记日志
        System.Threading.Thread wd = new System.Threading.Thread(delegate () {
            bool audioLogged = false;
            int pS = 0, pD = 0, pA = 0, tick = 0;
            while (true) {
                System.Threading.Thread.Sleep(1000); tick++;
                int last = f.uiTickMs;
                if (last != 0 && Environment.TickCount - last > 3000)
                    Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " UI卡死(>3s) op=" + f.uiOp + "\n");
                bool aStop = f.audioOn && f.lastAudioMs > 0 && Environment.TickCount - f.lastAudioMs > 2500;
                if (aStop && !audioLogged) { audioLogged = true; Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " 音频流停(>2.5s) op=" + f.uiOp + "\n"); }
                else if (!aStop) audioLogged = false;
                int cs = f.cSerial, cd = f.cSend, ca = f.cAudio;
                int ds = cs - pS, dd = cd - pD, da = ca - pA; pS = cs; pD = cd; pA = ca;
                if (tick % 5 == 0 || Math.Max(ds, Math.Max(dd, da)) > 20000)
                    Crash(DateTime.Now.ToString("HH:mm:ss.fff") + " 线程速率/秒 serial=" + ds + " send=" + dd + " audio=" + da + "\n");
            }
        });
        wd.IsBackground = true; wd.Start();
        Application.Run(f);
    }
}
