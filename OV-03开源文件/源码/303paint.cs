// 303paint.cs  —  OV-03 皮肤编辑器 (303paint)  v1.2
// 编译: csc /target:winexe /win32icon:303paint.ico /out:303paint.exe /r:System.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll 303paint.cs
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;

// ---- 程序集信息 (与安装包版本同步) ----
[assembly: System.Reflection.AssemblyTitle("303paint")]
[assembly: System.Reflection.AssemblyProduct("OV-03")]
[assembly: System.Reflection.AssemblyCompany("OV-03")]
[assembly: System.Reflection.AssemblyVersion("1.2.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("1.2.0.0")]
[assembly: System.Reflection.AssemblyInformationalVersion("OV-03 v1.2")]

// ===== 皮肤字段 (与 OV-03 一致) =====
static class Sk
{
    public static string Name = "默认";
    public static string FontName = "Segoe UI";
    public static float  FontSize = 9f;
    public static int TopH = 282, StepH = 322;
    public static double ScopeW = 0.55;
    public static string BaseDir = "", BgImage = ""; public static int BgMode = 0;

    public static Color formBg=C(24,24,28), text=Color.Gainsboro, textDim=Color.Gray, header=C(130,150,190),
        status=Color.LightGreen, value=Color.LightSkyBlue, warn=Color.Orange, stepNo=Color.SlateGray,
        topBg=C(32,32,38), stepBg=C(18,18,22), monBg=C(20,20,24), cellBg=C(28,28,34), cellBgOff=C(18,18,20),
        divider=C(120,120,145), btnBg=C(52,52,60), btnFg=Color.Gainsboro, btnBorder=C(80,80,90),
        toggleBg=C(44,44,52), accent=Color.DodgerBlue, accentFg=Color.White, playhead=Color.Gold, playFg=Color.Black,
        onAcc=Color.OrangeRed, onSlide=Color.DeepSkyBlue, onRest=Color.MediumPurple, onTie=Color.SeaGreen, noteTie=Color.Khaki,
        scopeBg=C(8,12,8), scopeGrid=C(18,34,18), scopeMid=C(28,55,28), scopeTrace=Color.Lime,
        barBg=C(36,36,42), barFill=C(46,90,150), knobBg=C(32,32,38), knobFace=C(48,48,56), knobRing=C(95,95,108), knobPtr=C(120,200,255),
        togOn=C(60,170,80), togOff=C(72,72,84), togKnob=Color.White, inputBg=Color.White, inputFg=Color.Black;

    // 颜色字段 + 中文说明 (一一对应)
    public static readonly string[] ColorFields = {
        "formBg","text","textDim","header","status","value","warn","stepNo","topBg","stepBg","monBg","cellBg","cellBgOff",
        "divider","btnBg","btnFg","btnBorder","toggleBg","accent","accentFg","playhead","playFg","onAcc","onSlide","onRest",
        "onTie","noteTie","scopeBg","scopeGrid","scopeMid","scopeTrace","barBg","barFill","knobBg","knobFace","knobRing",
        "knobPtr","togOn","togOff","togKnob","inputBg","inputFg" };
    public static readonly string[] ColorNames = {
        "窗体背景","主文字","次要文字","分组标题","状态栏文字","数值/音名","警告/未连接","步号",
        "工具条背景","步进区背景","监视区背景","格子(启用)","格子(禁用)","分隔线",
        "按钮背景","按钮文字","按钮边框","开关(未按下)","选中高亮","高亮文字",
        "播放头","播放头文字","ACC亮","SLIDE亮","REST亮","TIE亮","延音音名",
        "示波器底","示波器网格","示波器中线","示波器波形","参数条底","参数条填充",
        "旋钮背景","旋钮盘面","旋钮边圈","旋钮指针","开关(开)","开关(关)","开关滑块","输入框底","输入框文字" };

    public static Dictionary<string,int[]> Elem = new Dictionary<string,int[]>();

    static Color C(int r,int g,int b){ return Color.FromArgb(r,g,b); }
    public static Color Get(string n){ return (Color)typeof(Sk).GetField(n).GetValue(null); }
    public static void  Set(string n, Color c){ typeof(Sk).GetField(n).SetValue(null, c); }
    public static string Hex(Color c){ return string.Format("#{0:X2}{1:X2}{2:X2}", c.R, c.G, c.B); }
    public static Color Parse(string s, Color d){
        try{ if(string.IsNullOrEmpty(s))return d; s=s.Trim().TrimStart('#');
             int v=int.Parse(s,NumberStyles.HexNumber);
             if(s.Length<=6) return Color.FromArgb(255,(v>>16)&255,(v>>8)&255,v&255);
             return Color.FromArgb((v>>24)&255,(v>>16)&255,(v>>8)&255,v&255);}catch{return d;}
    }
    public static void Load(string path)
    {
        foreach (string raw in File.ReadAllLines(path))
        {
            string l = raw.Trim(); if (l.Length == 0 || l[0] == '#') continue;
            int eq = l.IndexOf('='); if (eq <= 0) continue;
            string k = l.Substring(0, eq).Trim(), v = l.Substring(eq + 1).Trim();
            try {
                if (k == "name") Name = v;
                else if (k == "font") FontName = v;
                else if (k == "fontsize") FontSize = float.Parse(v, CultureInfo.InvariantCulture);
                else if (k == "topH") { TopH = int.Parse(v); if (TopH < 282) TopH = 282; }
                else if (k == "stepH") StepH = int.Parse(v);
                else if (k == "scopeW") ScopeW = double.Parse(v, CultureInfo.InvariantCulture);
                else if (k == "bgimage") BgImage = v;
                else if (k == "bgmode") BgMode = int.Parse(v);
                else if (k.StartsWith("c.")) { string f = k.Substring(2); if (Array.IndexOf(ColorFields, f) >= 0) Set(f, Parse(v, Get(f))); }
                else if (k.StartsWith("elem.")) { string nm = k.Substring(5); string[] a = v.Split(','); if (a.Length >= 4) Elem[nm] = new int[]{ int.Parse(a[0]), int.Parse(a[1]), int.Parse(a[2]), int.Parse(a[3]) }; }
                else if (k.StartsWith("scr.")) {
                    string f = k.Substring(4);
                    if (f == "scopeH") Scr.ScopeH = int.Parse(v);
                    else if (f == "cols") Scr.Cols = int.Parse(v);
                    else if (f == "rows") Scr.Rows = int.Parse(v);
                    else if (f == "cellW") Scr.CellW = int.Parse(v);
                    else if (f == "cellH") Scr.CellH = int.Parse(v);
                    else if (f == "knobR") Scr.KnobR = int.Parse(v);
                    else if (f == "cellFn") { string[] a = v.Split(','); for (int i = 0; i < 8 && i < a.Length; i++) { int z; if (int.TryParse(a[i], out z)) Scr.CellFn[i] = z & 7; } }
                    else if (Array.IndexOf(Scr.ColorKeys, f) >= 0) Scr.Set(f, Scr.Parse(v, Scr.Get(f)));
                }
            } catch {}
        }
    }
    public static void Save(string path)
    {
        StringBuilder sb = new StringBuilder();
        sb.AppendLine("# 303paint skin  (OV-03 皮肤文件)");
        sb.AppendLine("name=" + Name);
        sb.AppendLine("font=" + FontName);
        sb.AppendLine("fontsize=" + FontSize.ToString(CultureInfo.InvariantCulture));
        sb.AppendLine("topH=" + TopH);
        sb.AppendLine("stepH=" + StepH);
        sb.AppendLine("scopeW=" + ScopeW.ToString(CultureInfo.InvariantCulture));
        sb.AppendLine("bgimage=" + BgImage);
        sb.AppendLine("bgmode=" + BgMode);
        foreach (string f in ColorFields) sb.AppendLine("c." + f + "=" + Hex(Get(f)));
        foreach (KeyValuePair<string,int[]> kv in Elem) { int[] a = kv.Value; sb.AppendLine("elem." + kv.Key + "=" + a[0] + "," + a[1] + "," + a[2] + "," + a[3]); }
        sb.AppendLine("scr.scopeH=" + Scr.ScopeH);
        sb.AppendLine("scr.cols=" + Scr.Cols);
        sb.AppendLine("scr.rows=" + Scr.Rows);
        sb.AppendLine("scr.cellW=" + Scr.CellW);
        sb.AppendLine("scr.cellH=" + Scr.CellH);
        sb.AppendLine("scr.knobR=" + Scr.KnobR);
        sb.AppendLine("scr.cellFn=" + string.Join(",", Array.ConvertAll(Scr.CellFn, delegate(int x) { return x.ToString(); })));
        foreach (string k in Scr.ColorKeys) sb.AppendLine("scr." + k + "=" + Scr.Hex(Scr.Get(k)));
        File.WriteAllText(path, sb.ToString(), Encoding.UTF8);
    }
}

// 元素基准位置 + 中文名
static class Elems
{
    public static readonly string[] Keys = {
        "port","timebase","read","write","play","stop","clear","len","bpm","follow",
        "save","load","audioport","device","volBar",
        "wave","pots","resbass","oc","unlock","wdt","filtos","reset","cpuscan","cpu",
        "knobNoise","knobVar","knobTemp","conn","status","scope",
        "selfosc","recStart","recStop","recPrev","recExport",
        "refresh","connect","skin","distpost","patA","patB","patC","patD","enA","enB","enC","enD" };
    public static readonly string[] Names = {
        "串口下拉","时基下拉","读回按钮","写入按钮","播放按钮","停止按钮","清空按钮","步数下拉","BPM输入","跟随按钮",
        "存音序按钮","读音序按钮","音频口下拉","输出设备下拉","监听音量",
        "波形开关","电位器","低频补偿","超频下拉","解锁超频","看门狗","过采样下拉","复位按钮","扫CPU按钮","CPU文字",
        "噪声旋钮","容差旋钮","温度旋钮","连接状态","状态栏","示波器",
        "自激开关","开始录音","停止录音","试听","导出",
        "刷新按钮","连接按钮","换皮肤按钮","失真后置","图A按钮","图B按钮","图C按钮","图D按钮","开关A","开关B","开关C","开关D" };
    public static int[] Base(string n)
    {
        switch (n) {
            case "port": return new int[]{76,26,86,24};
            case "timebase": return new int[]{56,140,72,24};
            case "read": return new int[]{18,106,54,24};
            case "write": return new int[]{76,106,54,24};
            case "play": return new int[]{134,106,50,24};
            case "stop": return new int[]{188,106,50,24};
            case "clear": return new int[]{242,106,50,24};
            case "len": return new int[]{376,92,46,24};
            case "bpm": return new int[]{470,92,54,24};
            case "follow": return new int[]{532,92,56,24};
            case "save": return new int[]{338,144,60,24};
            case "load": return new int[]{402,144,60,24};
            case "audioport": return new int[]{764,82,116,24};
            case "device": return new int[]{740,114,300,24};
            case "volBar": return new int[]{740,147,176,24};
            case "wave": return new int[]{738,26,44,22};
            case "pots": return new int[]{832,26,72,22};
            case "resbass": return new int[]{912,26,88,22};
            case "oc": return new int[]{1114,26,100,24};
            case "unlock": return new int[]{1222,26,86,24};
            case "wdt": return new int[]{1078,58,78,24};
            case "filtos": return new int[]{1222,58,54,24};
            case "reset": return new int[]{1078,92,46,24};
            case "cpuscan": return new int[]{1130,92,58,24};
            case "cpu": return new int[]{1194,95,160,18};
            case "knobNoise": return new int[]{1078,144,46,46};
            case "knobVar": return new int[]{1140,144,46,46};
            case "knobTemp": return new int[]{1202,144,46,46};
            case "recStart": return new int[]{702,202,100,24};
            case "selfosc": return new int[]{912,48,88,22};
            case "recStop": return new int[]{808,202,100,24};
            case "recPrev": return new int[]{702,230,100,24};
            case "recExport": return new int[]{808,230,100,24};
            case "conn": return new int[]{18,56,100,20};
            case "status": return new int[]{16,258,1052,20};
            case "scope": return new int[]{10,10,760,260};
            case "refresh": return new int[]{166,26,44,24};
            case "connect": return new int[]{214,26,52,24};
            case "skin": return new int[]{466,144,60,24};
            case "distpost": return new int[]{832,48,76,17};
            case "patA": return new int[]{376,58,34,24};
            case "patB": return new int[]{412,58,34,24};
            case "patC": return new int[]{448,58,34,24};
            case "patD": return new int[]{484,58,34,24};
            case "enA": return new int[]{404,26,34,20};
            case "enB": return new int[]{440,26,34,20};
            case "enC": return new int[]{476,26,34,20};
            case "enD": return new int[]{512,26,34,20};
            default: return new int[]{0,0,40,20};
        }
    }
}

// ===== Pico 小屏 (240x240) 皮肤 (与固件 ScreenSkin 对应) =====
static class Scr
{
    public static int Ver = 1, ScopeH = 141, Cols = 4, Rows = 2, CellW = 60, CellH = 49, KnobR = 10;
    public static int[] CellFn = new int[]{ 0,1,2,3,4,5,6,7 };   // 每格功能: 0TUNE 1CUT 2RES 3DIST 4ENV 5DEC 6ACC 7WAVE
    public static Color cScopeBg = C(0,0,35), cScopeGrid = C(0,45,0), cScopeTrace = C(0,255,120), cDivider = C(70,70,70),
        cCellBg = C(8,8,14), cRing = C(0,170,255), cKnobFill = C(20,24,34), cPtr = C(255,220,0), cLabel = C(150,150,150), cValue = C(255,255,255);
    public static readonly string[] ColorKeys = { "cScopeBg","cScopeGrid","cScopeTrace","cDivider","cCellBg","cRing","cKnobFill","cPtr","cLabel","cValue" };
    public static readonly string[] ColorNames = { "示波器底","示波器网格","示波器波形","分隔线","格子底","旋钮边","旋钮底","旋钮指针","标签文字","数值文字" };
    static Color C(int r,int g,int b){ return Color.FromArgb(r,g,b); }
    public static Color Get(string k){ return (Color)typeof(Scr).GetField(k).GetValue(null); }
    public static void  Set(string k, Color c){ typeof(Scr).GetField(k).SetValue(null, c); }
    public static string Hex(Color c){ return string.Format("#{0:X2}{1:X2}{2:X2}", c.R, c.G, c.B); }
    public static Color Parse(string s, Color d){ return Sk.Parse(s, d); }
    // 组成固件 '=' 命令的 hex (28 字节: 8 头 + 10×RGB565 LE)
    public static string Wire()
    {
        List<byte> b = new List<byte>();
        b.Add((byte)Ver); b.Add((byte)ScopeH); b.Add((byte)Cols); b.Add((byte)Rows);
        b.Add((byte)CellW); b.Add((byte)CellH); b.Add((byte)KnobR); b.Add(0);
        for (int i = 0; i < 8; i++) b.Add((byte)(CellFn[i] & 7));
        foreach (string k in ColorKeys) { Color c = Get(k); int v = ((c.R>>3)<<11)|((c.G>>2)<<5)|(c.B>>3); b.Add((byte)(v & 0xFF)); b.Add((byte)((v >> 8) & 0xFF)); }
        StringBuilder sb = new StringBuilder(); foreach (byte x in b) sb.Append(x.ToString("X2"));
        return sb.ToString();
    }
}

class Preview : Panel
{
    public int Hi = -1;                     // 高亮的颜色字段索引 (-1=不高亮)
    public int HiElem = -1;                 // 高亮的元素索引 (-1=不高亮)
    public Preview() { DoubleBuffered = true; ResizeRedraw = true; BackColor = Color.FromArgb(10,10,12); }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias;
        int VW = 1400, VH = 880;
        double s = Math.Min((double)Width / VW, (double)Height / VH);
        int ox = (int)((Width - VW * s) / 2), oy = (int)((Height - VH * s) / 2);
        Func<int,int> X = delegate(int v){ return ox + (int)(v*s); };
        Func<int,int> Y = delegate(int v){ return oy + (int)(v*s); };
        Func<int,int> S = delegate(int v){ return (int)(v*s); };
        using (SolidBrush b = new SolidBrush(Sk.formBg)) g.FillRectangle(b, X(0), Y(0), S(VW), S(VH));
        if (!string.IsNullOrEmpty(Sk.BgImage)) {
            string bgp = Sk.BgImage;
            try { if (!Path.IsPathRooted(bgp) && !string.IsNullOrEmpty(Sk.BaseDir)) bgp = Path.Combine(Sk.BaseDir, bgp); } catch {}
            try { if (File.Exists(bgp)) { using (Image im = Image.FromFile(bgp)) g.DrawImage(im, 0, 0, S(VW), S(VH)); } } catch {}
        }
        int topH = Sk.TopH, stepH = Sk.StepH, monY = topH + stepH, monH = VH - monY;
        using (SolidBrush b = new SolidBrush(Sk.topBg)) g.FillRectangle(b, X(0), Y(0), S(VW), S(topH));
        using (SolidBrush b = new SolidBrush(Sk.stepBg)) g.FillRectangle(b, X(0), Y(topH), S(VW), S(stepH));
        int cw = (int)((VW - 72) / 16.0), gap = 4;
        for (int i = 0; i < 16; i++) { using (SolidBrush b = new SolidBrush(Sk.cellBg)) g.FillRectangle(b, X(6 + i*(cw+gap)), Y(topH+6), S(cw), S(248)); }
        using (SolidBrush b = new SolidBrush(Sk.divider)) g.FillRectangle(b, X(6 + 8*(cw+gap) - 3), Y(topH+6), 2, S(248));
        using (SolidBrush b = new SolidBrush(Sk.monBg)) g.FillRectangle(b, X(0), Y(monY), S(VW), S(monH));
        int scW = (int)(VW * Sk.ScopeW);
        using (SolidBrush b = new SolidBrush(Sk.scopeBg)) g.FillRectangle(b, X(10), Y(monY+10), S(scW), S(monH-20));
        using (Pen p = new Pen(Sk.scopeGrid)) { for (int x = 0; x < scW; x += 48) g.DrawLine(p, X(10+x), Y(monY+10), X(10+x), Y(monY+monH-10)); }
        using (Pen p = new Pen(Sk.scopeMid)) g.DrawLine(p, X(10), Y(monY+monH/2), X(10+scW), Y(monY+monH/2));
        using (Pen p = new Pen(Sk.scopeTrace, 1.4f))
        { PointF[] pts = new PointF[64]; for (int i = 0; i < 64; i++){ double ph = i/64.0; float yv = (float)(monY+monH/2 - Math.Sin(ph*Math.PI*4)*(monH*0.35)); pts[i] = new PointF(X(10 + (int)(scW*ph)), Y((int)yv)); } g.DrawLines(p, pts); }
        int px = 10 + scW + 30;
        for (int i = 0; i < 8; i++) { int yy = monY + 8 + i*((monH-16)/8);
            using (SolidBrush b = new SolidBrush(Sk.barBg)) g.FillRectangle(b, X(px), Y(yy), S(246), S(18));
            using (SolidBrush b = new SolidBrush(Sk.barFill)) g.FillRectangle(b, X(px), Y(yy), S(120), S(18)); }
        // 元素 (含偏移)
        for (int k = 0; k < Elems.Keys.Length; k++)
        {
            string nm = Elems.Keys[k]; int[] br = Elems.Base(nm);
            int x=br[0], y=br[1], w=br[2], h=br[3];
            if (Sk.Elem.ContainsKey(nm)) { int[] a = Sk.Elem[nm]; x=a[0]; y=a[1]; w=a[2]; h=a[3]; }   // elem.* = 绝对 位置/大小
            w = Math.Max(6, w); h = Math.Max(6, h);
            bool isCbo = (nm=="port"||nm=="timebase"||nm=="len"||nm=="audioport"||nm=="device"||nm=="oc"||nm=="filtos");
            bool isChk = (nm=="pots"||nm=="resbass"||nm=="selfosc"||nm=="distpost"||nm=="unlock"||nm=="wdt"||nm=="enA"||nm=="enB"||nm=="enC"||nm=="enD");
            Color fill;
            if (nm == "scope") { y = monY + y; fill = Sk.scopeBg; }
            else { fill = isCbo ? Sk.inputBg : (isChk ? Sk.topBg : ((nm=="volBar"||nm=="cpu"||nm=="conn"||nm=="status") ? Sk.toggleBg : Sk.btnBg)); }
            using (SolidBrush b = new SolidBrush(Color.FromArgb(215, fill))) g.FillRectangle(b, X(x), Y(y), S(w), S(h));
            using (Pen p = new Pen(Sk.btnBorder)) g.DrawRectangle(p, X(x), Y(y), S(w), S(h));
            if (isCbo && S(w) > 20) { float ar = Math.Max(2f, S(h) * 0.16f), ax = X(x+w) - S(6), ay = Y(y+h/2);
                using (SolidBrush ab = new SolidBrush(Sk.btnFg)) g.FillPolygon(ab, new PointF[] { new PointF(ax-ar, ay-ar), new PointF(ax+ar, ay-ar), new PointF(ax, ay+ar) }); }
            if (isChk) { int q = Math.Max(4, (int)(S(h)*0.58)); using (Pen p2 = new Pen(Sk.btnFg)) g.DrawRectangle(p2, X(x)+S(3), Y(y)+S(3), q, q); }
            if (S(w) > 26) using (SolidBrush tb = new SolidBrush(Sk.btnFg))
                g.DrawString(Elems.Names[k], new Font("Microsoft YaHei", Math.Max(5f, (float)(6.5*s))), tb, X(x)+S(isChk?10:3), Y(y)+1);
        }
        // 选中元素/颜色 -> 预览里高亮 (二选一)
        if (HiElem >= 0 && HiElem < Elems.Keys.Length)
        {
            int[] er = Elems.Base(Elems.Keys[HiElem]); int ex=er[0], ey=er[1], ew=er[2], eh=er[3];
            if (Sk.Elem.ContainsKey(Elems.Keys[HiElem])) { int[] a = Sk.Elem[Elems.Keys[HiElem]]; ex=a[0]; ey=a[1]; ew=a[2]; eh=a[3]; }
            if (Elems.Keys[HiElem] == "scope") ey += monY;
            using (Pen hp = new Pen(Color.FromArgb(255, 225, 40), Math.Max(2.4f, (float)(1.8 * s))))
                g.DrawRectangle(hp, X(ex), Y(ey), S(Math.Max(6, ew)), S(Math.Max(6, eh)));
        }
        else if (Hi >= 0 && Hi < Sk.ColorFields.Length)
        {
            string cf = Sk.ColorFields[Hi];
            System.Collections.Generic.List<int[]> hs = new System.Collections.Generic.List<int[]>();
            switch (cf)
            {
                case "formBg": hs.Add(new int[] { 0, 0, VW, VH }); break;
                case "topBg": hs.Add(new int[] { 0, 0, VW, topH }); break;
                case "stepBg": hs.Add(new int[] { 0, topH, VW, stepH }); break;
                case "monBg": hs.Add(new int[] { 0, monY, VW, monH }); break;
                case "cellBg": case "cellBgOff": for (int i = 0; i < 16; i++) hs.Add(new int[] { 6 + i * (cw + gap), topH + 6, cw, 248 }); break;
                case "divider": hs.Add(new int[] { 6 + 8 * (cw + gap) - 3, topH + 6, 2, 248 }); break;
                case "scopeBg": case "scopeGrid": case "scopeMid": case "scopeTrace": hs.Add(new int[] { 10, monY + 10, scW, monH - 20 }); break;
                case "barBg": case "barFill": for (int i = 0; i < 8; i++) hs.Add(new int[] { px, monY + 8 + i * ((monH - 16) / 8), 246, 18 }); break;
                case "status": hs.Add(new int[] { 16, topH - 24, 1052, 20 }); break;
                case "knobBg": case "knobFace": case "knobRing": case "knobPtr":
                    hs.Add(Elems.Base("knobNoise")); hs.Add(Elems.Base("knobVar")); hs.Add(Elems.Base("knobTemp")); break;
                default: foreach (string kk in Elems.Keys) hs.Add(Elems.Base(kk)); break;
            }
            using (Pen hp = new Pen(Color.FromArgb(255, 225, 40), Math.Max(2.4f, (float)(1.8 * s))))
                foreach (int[] r in hs) g.DrawRectangle(hp, X(r[0]), Y(r[1]), S(r[2]), S(r[3]));
        }
    }
}

class PreviewForm : Form          // 放大预览 (只读, 不可操作)
{
    static PreviewForm inst = null;
    public static void ShowPreview()
    {
        if (inst == null || inst.IsDisposed) inst = new PreviewForm();
        inst.Show(); inst.BringToFront(); inst.Activate();
    }
    public PreviewForm()
    {
        Text = "预览 (只读, 不可操作)  —  OV-03";
        try { Icon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch {}
        ClientSize = new Size(1400, 880);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(10, 10, 12);
        Font = new Font("Microsoft YaHei", 9f);
        Preview p = new Preview(); p.Dock = DockStyle.Fill; p.Hi = -1;
        Controls.Add(p);
    }
}

class ScrPrev : Panel
{
    public ScrPrev() { DoubleBuffered = true; ResizeRedraw = true; BackColor = Color.FromArgb(10,10,12); }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        int s = Math.Min(Width, Height); if (s < 20) return;
        int ox = (Width - s) / 2, oy = (Height - s) / 2;
        Func<int,int> X = delegate(int v){ return ox + v*s/240; };
        Func<int,int> Y = delegate(int v){ return oy + v*s/240; };
        Func<int,int> Wd = delegate(int v){ return Math.Max(1, v*s/240); };
        int SH = Scr.ScopeH; if (SH < 10) SH = 10; if (SH > 239) SH = 239;
        using (SolidBrush b = new SolidBrush(Scr.cScopeBg)) g.FillRectangle(b, X(0), Y(0), Wd(240), Wd(SH));
        using (Pen p = new Pen(Scr.cScopeGrid)) g.DrawLine(p, X(0), Y(SH/2), X(240), Y(SH/2));
        using (Pen p = new Pen(Scr.cScopeTrace, 1.4f)) { PointF[] pts = new PointF[48]; for (int i = 0; i < 48; i++){ double ph = i/47.0; float yv = (float)(SH/2 - Math.Sin(ph*Math.PI*4)*(SH*0.4)); pts[i] = new PointF(X((int)(240*ph)), Y((int)yv)); } g.DrawLines(p, pts); }
        using (SolidBrush b = new SolidBrush(Scr.cDivider)) g.FillRectangle(b, X(0), Y(SH-1), Wd(240), Wd(1));
        using (SolidBrush b = new SolidBrush(Scr.cCellBg)) g.FillRectangle(b, X(0), Y(SH), Wd(240), Wd(240-SH));
        int cols = Scr.Cols, rows = Scr.Rows; if (cols < 1) cols = 1; if (rows < 1) rows = 1;
        for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
            int cx = c*Scr.CellW + Scr.CellW/2, cy = SH + r*Scr.CellH + 14, rr = Scr.KnobR; if (rr < 2) rr = 2;
            using (Pen p = new Pen(Scr.cRing)) g.DrawEllipse(p, X(cx-rr), Y(cy-rr), Wd(rr*2), Wd(rr*2));
            using (SolidBrush b = new SolidBrush(Scr.cLabel)) g.FillRectangle(b, X(c*Scr.CellW+6), Y(SH+r*Scr.CellH+30), Wd(Scr.CellW-12), Wd(3));
            using (SolidBrush b = new SolidBrush(Scr.cValue)) g.FillRectangle(b, X(c*Scr.CellW+10), Y(SH+r*Scr.CellH+36), Wd(Scr.CellW-20), Wd(3));
        }
        using (Pen p = new Pen(Color.FromArgb(90,90,90))) g.DrawRectangle(p, X(0), Y(0), Wd(240)-1, Wd(240)-1);
    }
}

class FlashBar : Panel
{
    public long total = 16777216, prog = 175000, cfg = 12288, media = 0;
    public FlashBar() { DoubleBuffered = true; ResizeRedraw = true; BackColor = Color.FromArgb(18,18,22); }
    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        Graphics g = e.Graphics;
        int x0 = 2, y0 = 2, w = Width - 4, h = Height - 4; if (w < 4 || h < 4) return;
        using (SolidBrush b = new SolidBrush(Color.FromArgb(40,40,48))) g.FillRectangle(b, x0, y0, w, h);           // 空闲
        int wp = (int)(w * (double)(prog + cfg) / total);
        using (SolidBrush b = new SolidBrush(Color.FromArgb(90,100,120))) g.FillRectangle(b, x0, y0, wp, h);       // 程序/配置
        int wm = (int)(w * (double)media / total);
        using (SolidBrush b = new SolidBrush(Color.FromArgb(230,150,40))) g.FillRectangle(b, x0 + wp, y0, wm, h);  // 媒体=橙
        using (Pen p = new Pen(Color.FromArgb(90,90,100))) g.DrawRectangle(p, x0, y0, w, h);
    }
}

class Editor : Form
{
    ListBox lstColor = new ListBox(), lstElem = new ListBox();
    ListBox lstScrColor = new ListBox(); ScrPrev prevScr = new ScrPrev();     Label lblScrStatus = new Label(); Label lblFlash = new Label(); ComboBox[] cellCb = new ComboBox[8]; long imgUsed = 0, flashTotal = 16777216, imgCapBytes = 0;
    // 媒体序列 (多图/GIF/视频 -> 多帧)
    ListBox lstMedia = new ListBox();
    CheckBox chkZip = new CheckBox();
    NumericUpDown numFps = new NumericUpDown(), numHold = new NumericUpDown();
    Label lblMediaInfo = new Label(), lblMediaStatus = new Label();
    List<byte[]> mFrames = new List<byte[]>();
    List<int> mDelays = new List<int>();
    int mW = 240, mH = 240; Button btnMediaUp = new Button(); bool mediaBusy = false;
    PictureBox picPrev = new PictureBox(); NumericUpDown numFrameDur = new NumericUpDown(); Timer mediaTimer = new Timer(); int mediaPlayIdx = -1, mediaNextAt = 0;
    List<byte> rxLeft = new List<byte>(); FlashBar fb1 = new FlashBar();
    NumericUpDown numSH, numSC, numSR, numSW, numSHt, numSKR;
    Preview prev = new Preview();
    TextBox txtName = new TextBox(), txtFont = new TextBox(), txtBg = new TextBox();
    NumericUpDown numFont, numTop, numStep, numScope, numDx, numDy, numDw, numDh;
    ComboBox cboPort = new ComboBox(); Button btnConn = new Button();
    CheckBox chkDither = new CheckBox();
    Label lblStatus = new Label();
    SerialPort sp = new SerialPort();
    string curPath = null; bool loading = false;

    public Editor()
    {
        Text = "303paint  —  OV-03 皮肤编辑器";
        Size = new Size(2270, 1000); MinimumSize = new Size(1100, 680);
        BackColor = Color.FromArgb(28, 28, 32); ForeColor = Color.Gainsboro;
        Font = new Font("Microsoft YaHei", 9f);
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch {}
        FormClosing += delegate { try { if (sp.IsOpen) { sp.Write("H0\n"); System.Threading.Thread.Sleep(80); sp.Close(); } } catch {} };   // 关闭时退出 PC 模式: 停媒体播放 + 板子恢复小屏

        Button b;
        b = Mk("新建", 8, 60, delegate { NewSkin(); });
        b = Mk("打开", 72, 60, delegate { Open(); });
        b = Mk("保存", 136, 60, delegate { Save(false); });
        b = Mk("另存为", 200, 74, delegate { Save(true); });
        Mk("进入 BOOT 烧录", 2050, 200, delegate { EnterBoot(); });   // 免按 BOOTSEL: 发 'b' 让 Pico 复位进 U 盘
        Label ln = new Label(); ln.Text = "皮肤名"; ln.SetBounds(288, 13, 50, 18); ln.ForeColor = Color.Gray; Controls.Add(ln);
        txtName.SetBounds(340, 10, 200, 24); Controls.Add(txtName);
        lblStatus.SetBounds(560, 12, 620, 20); lblStatus.ForeColor = Color.LightGreen; Controls.Add(lblStatus);

        // ① 颜色
        GroupBox gc = Gb("① 颜色（双击改色）", 8, 44, 300, 318);
        lstColor.SetBounds(10, 22, 280, 286); lstColor.BackColor = Color.FromArgb(20,20,24); lstColor.ForeColor = Color.Gainsboro;
        lstColor.DrawMode = DrawMode.OwnerDrawFixed; lstColor.ItemHeight = 19; lstColor.IntegralHeight = false;
        lstColor.DrawItem += ColorDraw; lstColor.DoubleClick += delegate { PickColor(); }; lstColor.SelectedIndexChanged += delegate { prev.Hi = lstColor.SelectedIndex; prev.HiElem = -1; prev.Invalidate(); };
        gc.Controls.Add(lstColor);

        // ② 布局与字体
        GroupBox gl = Gb("② 布局 / 字体 / 背景图", 8, 366, 300, 190);
        AddNum(gl, "工具条高", 10, 24, out numTop, 100, 500);
        AddNum(gl, "步进区高", 10, 52, out numStep, 100, 800);
        Label ls = new Label(); ls.Text = "示波器宽占比"; ls.SetBounds(10, 83, 90, 18); ls.ForeColor = Color.Gray; gl.Controls.Add(ls);
        numScope = new NumericUpDown(); numScope.SetBounds(102, 80, 60, 24); numScope.DecimalPlaces = 2; numScope.Increment = 0.05m; numScope.Minimum = 0.2m; numScope.Maximum = 0.9m; gl.Controls.Add(numScope);
        Label lf = new Label(); lf.Text = "字体"; lf.SetBounds(10, 113, 40, 18); lf.ForeColor = Color.Gray; gl.Controls.Add(lf);
        txtFont.SetBounds(52, 110, 130, 24); gl.Controls.Add(txtFont);
        numFont = new NumericUpDown(); numFont.SetBounds(188, 110, 50, 24); numFont.DecimalPlaces = 1; numFont.Increment = 0.5m; numFont.Minimum = 5; numFont.Maximum = 24; numFont.TextAlign = HorizontalAlignment.Center; gl.Controls.Add(numFont);
        Label lb2 = new Label(); lb2.Text = "背景图"; lb2.SetBounds(10, 143, 44, 18); lb2.ForeColor = Color.Gray; gl.Controls.Add(lb2);
        txtBg.SetBounds(56, 140, 172, 24); gl.Controls.Add(txtBg);
        Button bb2 = new Button(); bb2.Text = "浏览"; bb2.SetBounds(232, 140, 58, 24); Flat(bb2); bb2.Click += delegate { PickBg(); }; gl.Controls.Add(bb2);
        txtBg.TextChanged += delegate { Sk.BgImage = txtBg.Text; prev.Invalidate(); };
        numTop.ValueChanged += delegate { Sk.TopH = (int)numTop.Value; prev.Invalidate(); };
        numStep.ValueChanged += delegate { Sk.StepH = (int)numStep.Value; prev.Invalidate(); };
        numScope.ValueChanged += delegate { Sk.ScopeW = (double)numScope.Value; prev.Invalidate(); };
        txtFont.TextChanged += delegate { Sk.FontName = txtFont.Text; prev.Invalidate(); };
        numFont.ValueChanged += delegate { Sk.FontSize = (float)numFont.Value; prev.Invalidate(); };

        // ③ 串口 + 屏幕图片
        GroupBox gs = Gb("③ 串口 / 擦除 / flash 占用", 8, 560, 300, 214);
        Label lp = new Label(); lp.Text = "串口"; lp.SetBounds(10, 26, 36, 18); lp.ForeColor = Color.Gray; gs.Controls.Add(lp);
        cboPort.DropDownStyle = ComboBoxStyle.DropDownList; cboPort.SetBounds(50, 23, 120, 24); Light(cboPort); gs.Controls.Add(cboPort);
        Button bs = new Button(); bs.Text = "扫描"; bs.SetBounds(176, 23, 50, 24); Flat(bs); bs.Click += delegate { ScanPorts(); }; gs.Controls.Add(bs);
        btnConn.Text = "连接"; btnConn.SetBounds(230, 23, 60, 24); Flat(btnConn); btnConn.Click += delegate { ToggleConnect(); }; gs.Controls.Add(btnConn);
        // (单图上传已并入 ⑦ 媒体序列; 此处不再放 传图片/清图)
        Label lhint = new Label(); lhint.Text = "连接后读/写小屏图片；开着 OV-03 会占用串口，请先关。"; 
        fb1.SetBounds(10, 56, 280, 26); gs.Controls.Add(fb1);
        lblFlash.SetBounds(10, 88, 280, 18); lblFlash.ForeColor = Color.LightSkyBlue; gs.Controls.Add(lblFlash);
        Button be1 = new Button(); be1.Text = "擦除图片/视频"; be1.SetBounds(10, 118, 136, 28); Flat(be1); be1.Click += delegate { EraseEditorData(1); }; gs.Controls.Add(be1);
        Button be2 = new Button(); be2.Text = "擦除屏幕皮肤/旋钮"; be2.SetBounds(152, 118, 148, 28); Flat(be2); be2.Click += delegate { EraseEditorData(2); }; gs.Controls.Add(be2);
        lhint.SetBounds(10, 152, 280, 34); lhint.ForeColor = Color.SlateGray; gs.Controls.Add(lhint);

        // ④ 元素
        GroupBox ge = Gb("④ 元素 位置/大小（点列表选）", 316, 44, 250, 712);
        lstElem.SetBounds(10, 22, 228, 470); lstElem.BackColor = Color.FromArgb(20,20,24); lstElem.ForeColor = Color.Gainsboro;
        lstElem.IntegralHeight = false; lstElem.SelectedIndexChanged += delegate { LoadElem(); prev.HiElem = lstElem.SelectedIndex; prev.Hi = -1; prev.Invalidate(); }; ge.Controls.Add(lstElem);
        Label le = new Label(); le.Text = "选中元素偏移："; le.SetBounds(10, 500, 200, 18); le.ForeColor = Color.Gray; ge.Controls.Add(le);
        AddNum2(ge, "左右", 10, 522, out numDx);
        AddNum2(ge, "上下", 10, 552, out numDy);
        AddNum2(ge, "宽",   10, 582, out numDw);
        AddNum2(ge, "高",   10, 612, out numDh);
        Button br = new Button(); br.Text = "复位此元素"; br.SetBounds(10, 646, 228, 30); Flat(br);
        br.Click += delegate { if (lstElem.SelectedIndex >= 0) { Sk.Elem.Remove(Elems.Keys[lstElem.SelectedIndex]); LoadElem(); prev.Invalidate(); } }; ge.Controls.Add(br);
        numDx.ValueChanged += delegate { SetElem(); }; numDy.ValueChanged += delegate { SetElem(); };
        numDw.ValueChanged += delegate { SetElem(); }; numDh.ValueChanged += delegate { SetElem(); };

        // ⑤ 预览 (填满剩余)
        GroupBox gp = Gb("⑤ 预览", 574, 42, 600, 714); gp.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        Button bz = new Button(); bz.Text = "放大预览"; bz.SetBounds(12, 20, 90, 24); Flat(bz);
        bz.Click += delegate { PreviewForm.ShowPreview(); }; gp.Controls.Add(bz);
        Label lzh = new Label(); lzh.Text = "点左边颜色 -> 预览里对应元素高亮"; lzh.SetBounds(110, 24, 320, 18); lzh.ForeColor = Color.SlateGray; gp.Controls.Add(lzh);
        prev.SetBounds(12, 50, 576, 652); prev.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        gp.Controls.Add(prev);

        // ⑥ Pico 小屏 (240x240)
        GroupBox g6 = Gb("⑥ Pico 小屏 240×240（可烧录到板子）", 1184, 42, 640, 714);
        AddNum(g6, "示波器高", 10, 22, out numSH, 40, 239);
        AddNum(g6, "格列数", 10, 50, out numSC, 1, 6);
        AddNum(g6, "格行数", 10, 78, out numSR, 1, 4);
        AddNum(g6, "格宽", 10, 106, out numSW, 10, 240);
        AddNum(g6, "格高", 10, 134, out numSHt, 20, 120);
        AddNum(g6, "旋钮半径", 10, 162, out numSKR, 2, 40);
        Label lsc = new Label(); lsc.Text = "小屏颜色 (双击改):"; lsc.SetBounds(10, 194, 220, 16); lsc.ForeColor = Color.FromArgb(150,170,210); g6.Controls.Add(lsc);
        lstScrColor.SetBounds(10, 212, 374, 200); lstScrColor.BackColor = Color.FromArgb(20,20,24); lstScrColor.ForeColor = Color.Gainsboro;
        lstScrColor.DrawMode = DrawMode.OwnerDrawFixed; lstScrColor.ItemHeight = 18; lstScrColor.IntegralHeight = false;
        lstScrColor.DrawItem += ScrColorDraw; lstScrColor.DoubleClick += delegate { PickScrColor(); }; lstScrColor.SelectedIndexChanged += delegate { prevScr.Invalidate(); };
        g6.Controls.Add(lstScrColor);
        Label lcf = new Label(); lcf.Text = "格子功能 (第1→第8格, 可指定每格显示哪个参数):"; lcf.SetBounds(398, 22, 236, 16); lcf.ForeColor = Color.FromArgb(150,170,210); g6.Controls.Add(lcf);
        string[] fnn = { "TUNE(调音)", "CUT(截止)", "RES(谐振)", "DIST(失真)", "ENV(包络量)", "DEC(衰减)", "ACC(重音)", "WAVE(波形)" };
        for (int i = 0; i < 8; i++) {
            ComboBox cb = new ComboBox(); cb.DropDownStyle = ComboBoxStyle.DropDownList; cb.SetBounds(398, 42 + i*28, 236, 24);
            for (int k = 0; k < 8; k++) cb.Items.Add((i+1) + "格: " + fnn[k]);
            int ii = i; cb.SelectedIndexChanged += delegate { Scr.CellFn[ii] = cb.SelectedIndex; prevScr.Invalidate(); };
            g6.Controls.Add(cb); cellCb[i] = cb;
        }
        Button bclear = new Button(); bclear.Text = "重置格子功能"; bclear.SetBounds(398, 272, 236, 26); Flat(bclear);
        bclear.Click += delegate { for (int i = 0; i < 8; i++) Scr.CellFn[i] = i; RefreshScr(); }; g6.Controls.Add(bclear);
        prevScr.SetBounds(78, 420, 240, 240); g6.Controls.Add(prevScr);
        Button bf = new Button(); bf.Text = "烧录到 Pico"; bf.SetBounds(10, 668, 160, 30); Flat(bf); bf.Click += delegate { FlashScreen(); }; g6.Controls.Add(bf);
        lblScrStatus.SetBounds(176, 672, 212, 20); lblScrStatus.ForeColor = Color.LightGreen; g6.Controls.Add(lblScrStatus);
        numSH.ValueChanged += delegate { Scr.ScopeH = (int)numSH.Value; prevScr.Invalidate(); };
        numSC.ValueChanged += delegate { Scr.Cols = (int)numSC.Value; prevScr.Invalidate(); };
        numSR.ValueChanged += delegate { Scr.Rows = (int)numSR.Value; prevScr.Invalidate(); };
        numSW.ValueChanged += delegate { Scr.CellW = (int)numSW.Value; prevScr.Invalidate(); };
        numSHt.ValueChanged += delegate { Scr.CellH = (int)numSHt.Value; prevScr.Invalidate(); };
        numSKR.ValueChanged += delegate { Scr.KnobR = (int)numSKR.Value; prevScr.Invalidate(); };

        // ⑦ 媒体序列 (连接电脑时小屏播放: 多图/GIF/视频)
        GroupBox g7 = Gb("⑦ 媒体序列（小屏循环播放：多图 / GIF / 视频）", 1840, 42, 416, 916);
        lstMedia.SetBounds(10, 24, 396, 300); lstMedia.BackColor = Color.FromArgb(20,20,24); lstMedia.ForeColor = Color.Gainsboro; lstMedia.IntegralHeight = false; g7.Controls.Add(lstMedia);
        Button a1 = new Button(); a1.Text = "添加图片(可多选)"; a1.SetBounds(10, 330, 198, 30); Flat(a1); a1.Click += delegate { MediaAddImages(); }; g7.Controls.Add(a1);
        Button a2 = new Button(); a2.Text = "添加 GIF/视频"; a2.SetBounds(214, 330, 192, 30); Flat(a2); a2.Click += delegate { MediaAddVideo(); }; g7.Controls.Add(a2);
        Button d1 = new Button(); d1.Text = "上移"; d1.SetBounds(10, 366, 92, 28); Flat(d1); d1.Click += delegate { MediaMove(-1); }; g7.Controls.Add(d1);
        Button d2 = new Button(); d2.Text = "下移"; d2.SetBounds(108, 366, 92, 28); Flat(d2); d2.Click += delegate { MediaMove(1); }; g7.Controls.Add(d2);
        Button d3 = new Button(); d3.Text = "删除"; d3.SetBounds(206, 366, 92, 28); Flat(d3); d3.Click += delegate { MediaDel(); }; g7.Controls.Add(d3);
        Button d4 = new Button(); d4.Text = "清空"; d4.SetBounds(304, 366, 102, 28); Flat(d4); d4.Click += delegate { mFrames.Clear(); mDelays.Clear(); RefreshMedia(); }; g7.Controls.Add(d4);
        chkZip.Text = "压缩 (120×120, 能放约 4 倍时长)"; chkZip.SetBounds(10, 404, 320, 22); chkZip.ForeColor = Color.Gainsboro; g7.Controls.Add(chkZip);
        chkZip.CheckedChanged += delegate { int nw = chkZip.Checked ? 120 : 240; if (nw != mW) MediaReScale(nw, nw); };
        Label lfz = new Label(); lfz.Text = "视频/GIF 帧率"; lfz.SetBounds(10, 434, 100, 18); lfz.ForeColor = Color.Gray; g7.Controls.Add(lfz);
        numFps.SetBounds(112, 431, 60, 24); numFps.Minimum = 1; numFps.Maximum = 60; numFps.Value = 10; g7.Controls.Add(numFps);
        Label lfz2 = new Label(); lfz2.Text = "fps"; lfz2.SetBounds(176, 434, 30, 18); lfz2.ForeColor = Color.Gray; g7.Controls.Add(lfz2);
        Label lhd = new Label(); lhd.Text = "静图停留(ms)"; lhd.SetBounds(210, 434, 92, 18); lhd.ForeColor = Color.Gray; g7.Controls.Add(lhd);
        numHold.SetBounds(306, 431, 100, 24); numHold.Minimum = 100; numHold.Maximum = 10000; numHold.Increment = 100; numHold.Value = 1000; g7.Controls.Add(numHold);
        lblMediaInfo.SetBounds(10, 462, 396, 66); lblMediaInfo.ForeColor = Color.LightSkyBlue; g7.Controls.Add(lblMediaInfo);
        btnMediaUp.Text = "上传到 Pico"; btnMediaUp.SetBounds(10, 532, 148, 32); Flat(btnMediaUp); btnMediaUp.Click += delegate { UploadMedia(); }; g7.Controls.Add(btnMediaUp);
        Button bread = new Button(); bread.Text = "从 Pico 读取"; bread.SetBounds(164, 532, 120, 32); Flat(bread); bread.Click += delegate { ReadMedia(); }; g7.Controls.Add(bread);
        Button bexp = new Button(); bexp.Text = "导出 PNG"; bexp.SetBounds(290, 532, 116, 32); Flat(bexp); bexp.Click += delegate { ExportPng(); }; g7.Controls.Add(bexp);
        lblMediaStatus.SetBounds(10, 570, 396, 40); lblMediaStatus.ForeColor = Color.LightGreen; g7.Controls.Add(lblMediaStatus);
        Label lmh = new Label(); lmh.Text = "帧 = 240×240×2=112.5KB（压缩时 120×120×2=28.1KB）;\n视频用自带 ffmpeg 抽帧, GIF 自动拆帧; 按顺序循环。\n上限 ≈ 芯片−0.3MB（≈15.7MB）。";
        lmh.SetBounds(10, 618, 396, 80); lmh.ForeColor = Color.SlateGray; g7.Controls.Add(lmh);
        Label lpv = new Label(); lpv.Text = "预览（选中帧 / 播放）"; lpv.SetBounds(10, 700, 200, 16); lpv.ForeColor = Color.FromArgb(150,170,210); g7.Controls.Add(lpv);
        picPrev.SetBounds(10, 718, 150, 150); picPrev.SizeMode = PictureBoxSizeMode.Zoom; picPrev.BackColor = Color.Black; picPrev.BorderStyle = BorderStyle.FixedSingle; g7.Controls.Add(picPrev);
        Button bp = new Button(); bp.Text = "▶ 播放"; bp.SetBounds(170, 718, 92, 28); Flat(bp); bp.Click += delegate { MediaPlay(); }; g7.Controls.Add(bp);
        Button bsx = new Button(); bsx.Text = "■ 停止"; bsx.SetBounds(268, 718, 88, 28); Flat(bsx); bsx.Click += delegate { mediaTimer.Stop(); }; g7.Controls.Add(bsx);
        Label lfd = new Label(); lfd.Text = "本帧时长(ms)"; lfd.SetBounds(170, 754, 90, 18); lfd.ForeColor = Color.Gray; g7.Controls.Add(lfd);
        numFrameDur.SetBounds(262, 751, 94, 24); numFrameDur.Minimum = 20; numFrameDur.Maximum = 10000; numFrameDur.Value = 100; g7.Controls.Add(numFrameDur);
        Button bap = new Button(); bap.Text = "应用到本帧"; bap.SetBounds(170, 782, 186, 28); Flat(bap);
        bap.Click += delegate { int i = lstMedia.SelectedIndex; if (i >= 0 && i < mDelays.Count) { mDelays[i] = (int)numFrameDur.Value; RefreshMedia(); lstMedia.SelectedIndex = i; } }; g7.Controls.Add(bap);
        lstMedia.SelectedIndexChanged += delegate { ShowFrame(); };
        mediaTimer.Interval = 100; mediaTimer.Tick += delegate { MediaTick(); };

        NewSkin();
        ScanPorts();
    }

    Button Mk(string t, int x, int w, EventHandler on){ Button b=new Button(); b.Text=t; b.SetBounds(x,8,w,30); Flat(b); b.Click+=on; Controls.Add(b); return b; }
    static void Flat(Button b){ b.FlatStyle=FlatStyle.Flat; b.BackColor=Color.FromArgb(52,52,60); b.ForeColor=Color.Gainsboro; b.FlatAppearance.BorderColor=Color.FromArgb(80,80,90); }
    static void Light(Control c){ c.BackColor=Color.White; c.ForeColor=Color.Black; ComboBox cb=c as ComboBox; if(cb!=null) cb.FlatStyle=FlatStyle.Flat; }
    GroupBox Gb(string t, int x, int y, int w, int h){ GroupBox g=new GroupBox(); g.Text=t; g.SetBounds(x,y,w,h); g.ForeColor=Color.FromArgb(150,170,210); Controls.Add(g); return g; }
    void AddNum(Control p, string t, int x, int y, out NumericUpDown n, int min, int max){ Label l=new Label(); l.Text=t; l.SetBounds(x,y+3,72,18); l.ForeColor=Color.Gray; p.Controls.Add(l); n=new NumericUpDown(); n.SetBounds(x+76,y,60,24); n.Minimum=min; n.Maximum=max; n.Value=Math.Min(max, Math.Max(min, 100)); p.Controls.Add(n); }
    void AddNum2(Control p, string t, int x, int y, out NumericUpDown n){ Label l=new Label(); l.Text=t; l.SetBounds(x,y+3,40,18); l.ForeColor=Color.Gray; p.Controls.Add(l); n=new NumericUpDown(); n.SetBounds(x+44,y,90,24); n.Minimum=-40; n.Maximum=2000; p.Controls.Add(n); }

    void ColorDraw(object s, DrawItemEventArgs e)
    {
        if (e.Index < 0) return;
        e.DrawBackground();
        string f = Sk.ColorFields[e.Index];
        Rectangle r = new Rectangle(e.Bounds.X + 1, e.Bounds.Y + 1, 18, e.Bounds.Height - 2);
        using (SolidBrush b = new SolidBrush(Sk.Get(f))) e.Graphics.FillRectangle(b, r);
        e.Graphics.DrawRectangle(Pens.Gray, r);
        using (SolidBrush b = new SolidBrush(e.ForeColor)) e.Graphics.DrawString(Sk.ColorNames[e.Index] + "  " + Sk.Hex(Sk.Get(f)), e.Font, b, e.Bounds.X + 24, e.Bounds.Y + 1);
    }
    void PickColor()
    {
        if (lstColor.SelectedIndex < 0) return;
        string f = Sk.ColorFields[lstColor.SelectedIndex];
        ColorDialog cd = new ColorDialog(); cd.Color = Sk.Get(f); cd.FullOpen = true;
        if (cd.ShowDialog() != DialogResult.OK) return;
        Sk.Set(f, cd.Color); lstColor.Invalidate(); prev.Invalidate();
    }
    void LoadElem()
    {
        if (lstElem.SelectedIndex < 0) return;
        string nm = Elems.Keys[lstElem.SelectedIndex]; int[] br = Elems.Base(nm);
        int x=br[0], y=br[1], w=br[2], h=br[3];
        if (Sk.Elem.ContainsKey(nm)) { int[] a = Sk.Elem[nm]; x=a[0];y=a[1];w=a[2];h=a[3]; }
        loading = true; numDx.Value=x; numDy.Value=y; numDw.Value=w; numDh.Value=h; loading = false;
        prev.Invalidate();
    }
    void SetElem()
    {
        if (loading || lstElem.SelectedIndex < 0) return;
        Sk.Elem[Elems.Keys[lstElem.SelectedIndex]] = new int[]{ (int)numDx.Value, (int)numDy.Value, Math.Max(6,(int)numDw.Value), Math.Max(6,(int)numDh.Value) };
        prev.Invalidate();
    }
    void NewSkin()
    {
        Sk.Name="默认"; Sk.FontName="Segoe UI"; Sk.FontSize=9f; Sk.TopH=282; Sk.StepH=322; Sk.ScopeW=0.55;
        Sk.Elem.Clear();
        string[] defs = {"formBg=18181C","text=DCDCDC","textDim=808080","header=8296BE","status=90EE90","value=87CEEB","warn=FFA500","stepNo=708090",
            "topBg=202026","stepBg=121216","monBg=141418","cellBg=1C1C22","cellBgOff=121214","divider=787891","btnBg=34343C","btnFg=DCDCDC",
            "btnBorder=50505A","toggleBg=2C2C34","accent=1E90FF","accentFg=FFFFFF","playhead=FFD700","playFg=000000","onAcc=FF4500","onSlide=00BFFF",
            "onRest=9370DB","onTie=2E8B57","noteTie=F0E68C","scopeBg=080C08","scopeGrid=122212","scopeMid=1C371C","scopeTrace=00FF00",
            "barBg=24242A","barFill=2E5A96","knobBg=202026","knobFace=303038","knobRing=5F5F6C","knobPtr=78C8FF","togOn=3CAA50","togOff=484854",
            "togKnob=FFFFFF","inputBg=FFFFFF","inputFg=000000"};
        foreach (string d in defs){ int eq=d.IndexOf('='); Sk.Set(d.Substring(0,eq), Sk.Parse(d.Substring(eq+1), Color.Black)); }
        curPath = null; RefreshAll();
    }
    void RefreshAll()
    {
        loading = true;
        txtName.Text = Sk.Name; txtFont.Text = Sk.FontName; numFont.Value = (decimal)Sk.FontSize; txtBg.Text = Sk.BgImage;
        numTop.Value = Sk.TopH; numStep.Value = Sk.StepH; numScope.Value = (decimal)Sk.ScopeW;
        lstColor.Items.Clear(); for (int i=0;i<Sk.ColorFields.Length;i++) lstColor.Items.Add(Sk.ColorNames[i]);
        lstElem.Items.Clear(); foreach (string n in Elems.Names) lstElem.Items.Add(n);
        loading = false; prev.Invalidate(); RefreshScr();
    }
    void Open()
    {
        OpenFileDialog d = new OpenFileDialog(); d.Filter = "303paint 皮肤 (*.303paint)|*.303paint|所有文件 (*.*)|*.*";
        if (d.ShowDialog() != DialogResult.OK) return;
        try { Sk.Load(d.FileName); curPath = d.FileName; try { Sk.BaseDir = Path.GetDirectoryName(Path.GetFullPath(d.FileName)); } catch {} RefreshAll(); lblStatus.Text = "已打开 " + Path.GetFileName(d.FileName); } catch (Exception ex) { MessageBox.Show("打开失败: " + ex.Message); }
    }
    void PickBg()
    {
        OpenFileDialog d = new OpenFileDialog(); d.Filter = "图片 (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|所有文件 (*.*)|*.*";
        try { if (!string.IsNullOrEmpty(Sk.BaseDir)) d.InitialDirectory = Sk.BaseDir; } catch {}
        if (d.ShowDialog() != DialogResult.OK) return;
        try {
            if (!string.IsNullOrEmpty(Sk.BaseDir)) { Sk.BgImage = Path.GetFileName(d.FileName); File.Copy(d.FileName, Path.Combine(Sk.BaseDir, Sk.BgImage), true); }
            else Sk.BgImage = d.FileName;
        } catch { Sk.BgImage = d.FileName; }
        txtBg.Text = Sk.BgImage; prev.Invalidate();
    }
    void Save(bool asNew)
    {
        string path = curPath;
        if (asNew || path == null) { SaveFileDialog d = new SaveFileDialog(); d.Filter = "303paint 皮肤 (*.303paint)|*.303paint"; d.FileName = Sk.Name + ".303paint"; if (d.ShowDialog() != DialogResult.OK) return; path = d.FileName; }
        Sk.Name = txtName.Text;
        for (int k = 0; k < Elems.Keys.Length; k++) { string e = Elems.Keys[k]; if (!Sk.Elem.ContainsKey(e)) { int[] bb = Elems.Base(e); Sk.Elem[e] = new int[] { bb[0], bb[1], bb[2], bb[3] }; } }   // 保存完整布局(绝对位置/大小)
        try { Sk.Save(path); curPath = path; try { Sk.BaseDir = Path.GetDirectoryName(Path.GetFullPath(path)); } catch {} lblStatus.Text = "已保存 " + path; } catch (Exception ex) { MessageBox.Show("保存失败: " + ex.Message); }
    }

    // ---- Pico 小屏 ----
    static decimal ClampN(int v, NumericUpDown n) { decimal d = v; if (d < n.Minimum) d = n.Minimum; if (d > n.Maximum) d = n.Maximum; return d; }
    void RefreshScr()
    {
        loading = true;
        numSH.Value = ClampN(Scr.ScopeH, numSH); numSC.Value = ClampN(Scr.Cols, numSC); numSR.Value = ClampN(Scr.Rows, numSR);
        numSW.Value = ClampN(Scr.CellW, numSW); numSHt.Value = ClampN(Scr.CellH, numSHt); numSKR.Value = ClampN(Scr.KnobR, numSKR);
        lstScrColor.Items.Clear(); for (int i = 0; i < Scr.ColorKeys.Length; i++) lstScrColor.Items.Add(Scr.ColorNames[i]);
        for (int i = 0; i < 8; i++) if (cellCb[i] != null) cellCb[i].SelectedIndex = Scr.CellFn[i];
        loading = false; prevScr.Invalidate();
    }
    void ScrColorDraw(object s, DrawItemEventArgs e)
    {
        if (e.Index < 0) return;
        e.DrawBackground(); string f = Scr.ColorKeys[e.Index];
        Rectangle r = new Rectangle(e.Bounds.X + 1, e.Bounds.Y + 1, 18, e.Bounds.Height - 2);
        using (SolidBrush b = new SolidBrush(Scr.Get(f))) e.Graphics.FillRectangle(b, r);
        e.Graphics.DrawRectangle(Pens.Gray, r);
        using (SolidBrush b = new SolidBrush(e.ForeColor)) e.Graphics.DrawString(Scr.ColorNames[e.Index] + "  " + Scr.Hex(Scr.Get(f)), e.Font, b, e.Bounds.X + 24, e.Bounds.Y + 1);
    }
    void PickScrColor()
    {
        if (lstScrColor.SelectedIndex < 0) return;
        string f = Scr.ColorKeys[lstScrColor.SelectedIndex];
        ColorDialog cd = new ColorDialog(); cd.Color = Scr.Get(f); cd.FullOpen = true;
        if (cd.ShowDialog() != DialogResult.OK) return;
        Scr.Set(f, cd.Color); lstScrColor.Invalidate(); prevScr.Invalidate();
    }
    void FlashScreen()
    {
        if (!sp.IsOpen) { lblScrStatus.Text = "未连接(先在上方连接串口)"; lblScrStatus.ForeColor = Color.Orange; return; }
        try {
            sp.DiscardInBuffer(); sp.Write("=" + Scr.Wire() + "\n");
            string r = WaitForText(4000);
            bool ok = (r != null && r.Contains("saved"));
            lblScrStatus.Text = ok ? "已烧录 ✓" : ("失败: " + (r == null ? "无回复" : r.Trim()));
            lblScrStatus.ForeColor = ok ? Color.LightGreen : Color.OrangeRed;
        } catch (Exception ex) { lblScrStatus.Text = "失败: " + ex.Message; lblScrStatus.ForeColor = Color.OrangeRed; }
    }
    string WaitForText(int ms)
    {
        int dl = Environment.TickCount + ms; StringBuilder sb = new StringBuilder(); byte[] buf = new byte[2048];
        while (Environment.TickCount < dl)
        {
            int av = 0; try { av = sp.BytesToRead; } catch {}
            if (av > 0) { int n = 0; try { n = sp.Read(buf, 0, Math.Min(av, buf.Length)); } catch {} for (int i = 0; i < n; i++) sb.Append((char)buf[i]); if (sb.ToString().IndexOf("#SCREEN") >= 0) return sb.ToString(); }
            else System.Threading.Thread.Sleep(5);
        }
        return sb.Length > 0 ? sb.ToString() : null;
    }
    void QueryFlash()
    {
        try { sp.DiscardInBuffer(); sp.Write("T\n"); ParseFlash(WaitForText(1500)); } catch {}
    }
    void ParseFlash(string r)
    {
        if (string.IsNullOrEmpty(r)) return;
        try {
            System.Text.RegularExpressions.Match m = System.Text.RegularExpressions.Regex.Match(r, @"imgUsed=(\d+)\s+flash=(\d+)(?:\s+imgCap=(\d+))?");
            if (m.Success) {
                imgUsed = long.Parse(m.Groups[1].Value); flashTotal = long.Parse(m.Groups[2].Value);
                imgCapBytes = m.Groups[3].Success ? long.Parse(m.Groups[3].Value) : (flashTotal - (long)(0.3 * 1048576));
                double totalMb = flashTotal / 1048576.0;
                double media = imgUsed / 1048576.0;
                double freeMb = (imgCapBytes - imgUsed) / 1048576.0; if (freeMb < 0) freeMb = 0;
                lblFlash.Text = "总 " + totalMb.ToString("0.0") + "MB | 图片区 " + (imgCapBytes / 1048576.0).ToString("0.0") + "MB | 媒体 " + media.ToString("0.00") + "MB | 皮肤/设置/音序 各4KB | 剩 " + freeMb.ToString("0.0") + "MB";
                fb1.total = flashTotal; fb1.media = imgUsed; fb1.Invalidate();
            }
        } catch {}
    }
    void EraseEditorData(int what)
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        try {
            sp.DiscardInBuffer(); sp.Write("?" + what + "\n");
            bool ok = WaitFor("#ERASED", 3000);
            SetStatus(ok ? ("已擦除" + (what == 1 ? "图片" : (what == 2 ? "屏幕皮肤" : "绘制器数据")) + " (恢复默认)") : "擦除失败");
            if (ok) QueryFlash();
        } catch (Exception ex) { SetStatus("擦除失败: " + ex.Message); }
    }

    // ===== 串口 =====
    void SetStatus(string s){ lblStatus.Text = s; }
    void ScanPorts() {
        cboPort.Items.Clear();
        string found = null;
        foreach (string p in SerialPort.GetPortNames()) {
            cboPort.Items.Add(p);
            if (found != null) continue;
            try {
                SerialPort t = new SerialPort(p, 115200); t.ReadTimeout = 300; t.DtrEnable = true; t.RtsEnable = true; t.Open();
                System.Threading.Thread.Sleep(1200); t.DiscardInBuffer();
                t.Write("T\n"); System.Threading.Thread.Sleep(500);
                string r = t.ReadExisting(); t.Close();
                if (!string.IsNullOrEmpty(r) && r.Contains("temp=")) found = p;
            } catch {}
        }
        if (found != null) { cboPort.SelectedIndex = cboPort.Items.IndexOf(found); SetStatus("识别到文本口: " + found); }
        else if (cboPort.Items.Count > 0) { cboPort.SelectedIndex = 0; SetStatus("未识别到文本口(板子没连?), 先手动选一个再连接"); }
        else SetStatus("没有串口");
    }
    void RefreshPorts()
    {
        string cur = cboPort.Text; cboPort.Items.Clear();
        try { foreach (string p in SerialPort.GetPortNames()) cboPort.Items.Add(p); } catch {}
        if (cboPort.Items.Count > 0) { int i = cboPort.FindStringExact(cur); cboPort.SelectedIndex = i >= 0 ? i : 0; }
    }
    void ToggleConnect()
    {
        if (sp.IsOpen) { try { sp.Write("H0\n"); System.Threading.Thread.Sleep(80); sp.Close(); } catch {} btnConn.Text = "连接"; SetStatus("已断开 (小屏恢复本地界面)"); return; }
        if (cboPort.SelectedIndex < 0) { SetStatus("没有可用串口, 点「扫描」"); return; }
        try {
            sp.PortName = cboPort.Text; sp.BaudRate = 115200; sp.ReadTimeout = 200; sp.WriteTimeout = 8000; sp.DtrEnable = true; sp.RtsEnable = true; sp.Open();
            btnConn.Text = "断开"; SetStatus("已连接 " + sp.PortName);
            System.Threading.Thread.Sleep(300); sp.DiscardInBuffer(); sp.Write("T\n"); System.Threading.Thread.Sleep(600);
            string rr = ""; try { rr = sp.ReadExisting(); } catch {}
            if (string.IsNullOrEmpty(rr) || !rr.Contains("temp=")) SetStatus("已连接 " + sp.PortName + " 但无回复 → 多半是选成音频口/被占用；点「扫描」自动识别文本口");
            else { QueryFlash(); WriteLine("H1"); System.Threading.Thread.Sleep(150); WriteLine("S0"); System.Threading.Thread.Sleep(60); SetStatus("已连接 " + sp.PortName + " (小屏已进 PC 模式: 显示已存图片/媒体)"); }
        }
        catch (Exception ex) { SetStatus("连接失败(可能被 OV-03 占用): " + ex.Message); }
    }
    // 免按 BOOTSEL: 连接后发 'b', Pico 复位为 U 盘 RPI-RP2, 拖入 .uf2 即可烧录
    void EnterBoot()
    {
        if (!sp.IsOpen) { SetStatus("请先点「连接」, 再进 BOOT"); return; }
        try { sp.DiscardInBuffer(); WriteLine("b"); System.Threading.Thread.Sleep(150); }
        catch (Exception ex) { SetStatus("进 BOOT 失败: " + ex.Message); return; }
        try { sp.Close(); } catch {}
        btnConn.Text = "连接";
        SetStatus("已发送 BOOT: Pico 即将以 U 盘 RPI-RP2 出现, 把 .uf2 拖进去即可烧录 (烧完自动重启)");
    }
    void WriteLine(string s){ try { sp.Write(s + "\n"); } catch {} }
    bool WaitFor(string marker, int ms)
    {
        int dl = Environment.TickCount + ms; StringBuilder sb = new StringBuilder(); byte[] buf = new byte[4096];
        while (Environment.TickCount < dl)
        {
            int av = 0; try { av = sp.BytesToRead; } catch {}
            if (av > 0) { int n = 0; try { n = sp.Read(buf, 0, Math.Min(av, buf.Length)); } catch {} for (int i = 0; i < n; i++) sb.Append((char)buf[i]); if (sb.ToString().IndexOf(marker) >= 0) return true; if (sb.Length > 8192) sb.Remove(0, sb.Length - 4096); }
            else System.Threading.Thread.Sleep(2);
        }
        return false;
    }
    void Reconnect()
    {
        if (!sp.IsOpen) return;
        try { sp.Close(); } catch {}
        System.Threading.Thread.Sleep(1500);
        try { sp.Open(); WriteLine("H1"); System.Threading.Thread.Sleep(150); WriteLine("S0"); SetStatus("图片操作完成, 已重连 " + sp.PortName + " (小屏 PC 模式)"); } catch (Exception ex) { btnConn.Text = "连接"; SetStatus("重连失败: " + ex.Message); }
    }
    bool BuildImageData(string path, bool dither, out byte[] data, out int mode, out string err)
    {
        data = null; mode = 1; err = "";
        try {
            Bitmap dst = new Bitmap(240, 240);
            using (Bitmap src = new Bitmap(path)) {
                if (dither) {
                    int G = 120; Bitmap small = new Bitmap(G, G);
                    using (Graphics gs = Graphics.FromImage(small)) { gs.Clear(Color.Black); gs.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        double s = Math.Min((double)G / src.Width, (double)G / src.Height); int w = Math.Max(1, (int)(src.Width * s)), h = Math.Max(1, (int)(src.Height * s)); gs.DrawImage(src, (G - w) / 2, (G - h) / 2, w, h); }
                    using (Graphics g = Graphics.FromImage(dst)) { g.Clear(Color.Black); g.InterpolationMode = InterpolationMode.NearestNeighbor; g.PixelOffsetMode = PixelOffsetMode.Half; g.DrawImage(small, 0, 0, 240, 240); }
                    small.Dispose();
                } else using (Graphics g = Graphics.FromImage(dst)) { g.Clear(Color.Black); g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    double s = Math.Min(240.0 / src.Width, 240.0 / src.Height); int w = Math.Max(1, (int)(src.Width * s)), h = Math.Max(1, (int)(src.Height * s)); g.DrawImage(src, (240 - w) / 2, (240 - h) / 2, w, h); }
            }
            data = new byte[240 * 240 * 2]; int i = 0;
            for (int y = 0; y < 240; y++) for (int x = 0; x < 240; x++) { Color c = dst.GetPixel(x, y); int v = ((c.R >> 3) << 11) | ((c.G >> 2) << 5) | (c.B >> 3); data[i++] = (byte)(v & 0xFF); data[i++] = (byte)(v >> 8); }
            mode = 1; dst.Dispose();
        } catch (Exception ex) { err = ex.Message; return false; }
        return true;
    }
    void UploadImage()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        OpenFileDialog d = new OpenFileDialog(); d.Filter = "图片 (*.png;*.jpg;*.jpeg;*.bmp;*.gif)|*.png;*.jpg;*.jpeg;*.bmp;*.gif|所有文件 (*.*)|*.*";
        d.InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        if (d.ShowDialog() != DialogResult.OK) return;
        byte[] data; int mode; string err;
        if (!BuildImageData(d.FileName, chkDither.Checked, out data, out mode, out err)) { SetStatus("处理图片失败: " + err); return; }
        long need = 4096 + data.Length;
        long cap = imgCapBytes > 0 ? imgCapBytes : (long)(flashTotal - 0.3 * 1048576);
        if (need > cap) { SetStatus("文件过大: 需 " + (need/1048576.0).ToString("0.00") + "MB > 上限 " + (cap/1048576.0).ToString("0.0") + "MB (芯片-0.3MB), 超出 flash 容量, 无法烧录"); return; }
        SetStatus("上传图片中... (" + data.Length + " 字节)");
        System.Threading.Thread t = new System.Threading.Thread(delegate () { UploadWorker(data, mode); }); t.IsBackground = true; t.Start();
    }
    void UploadWorker(byte[] data, int mode)
    {
        try {
            try { sp.DiscardInBuffer(); } catch {}
            int oldWT = sp.WriteTimeout; sp.WriteTimeout = 5000;
            WriteLine("H1"); System.Threading.Thread.Sleep(150);
            WriteLine("S0"); System.Threading.Thread.Sleep(80);
            WriteLine("I0"); System.Threading.Thread.Sleep(80);
            try { sp.DiscardInBuffer(); } catch {}
            WriteLine("^" + mode + "," + data.Length);
            if (!WaitFor("#IMGRDY", 12000)) throw new Exception("无 #IMGRDY");
            int off = 0;
            while (off < data.Length) {
                int n = Math.Min(4096, data.Length - off); sp.Write(data, off, n); off += n;
                if (off >= data.Length) { if (!WaitFor("#IMGOK", 20000)) throw new Exception("末尾无 #IMGOK"); }
                else { if (!WaitFor("#IMGC", 12000)) throw new Exception("分块超时"); }
            }
            sp.WriteTimeout = oldWT;
            BeginInvoke((MethodInvoker)delegate { SetStatus("图片已上传, 重连中..."); Reconnect(); });
        } catch (Exception ex) {
            try { if (sp.IsOpen) sp.DiscardInBuffer(); } catch {}
            BeginInvoke((MethodInvoker)delegate { SetStatus("上传失败: " + ex.Message); });
        }
    }
    void ClearImage()
    {
        if (!sp.IsOpen) { SetStatus("未连接"); return; }
        try { WriteLine("^X"); } catch {}
        SetStatus("已清空图片, 重连中...");
        System.Threading.Thread t = new System.Threading.Thread(delegate () { System.Threading.Thread.Sleep(200); BeginInvoke((MethodInvoker)delegate { Reconnect(); }); }); t.IsBackground = true; t.Start();
    }

    // ===== 媒体序列 =====
    string FindFfmpeg() {
        string p = Path.Combine(Application.StartupPath, "tools", "ffmpeg.exe"); if (File.Exists(p)) return p;
        return "ffmpeg";
    }
    string RunCapture(string exe, string args) {
        System.Diagnostics.ProcessStartInfo psi = new System.Diagnostics.ProcessStartInfo(exe, args);
        psi.UseShellExecute = false; psi.CreateNoWindow = true; psi.RedirectStandardOutput = true; psi.RedirectStandardError = true;
        System.Diagnostics.Process pr = System.Diagnostics.Process.Start(psi);
        System.Text.StringBuilder so = new System.Text.StringBuilder(), se = new System.Text.StringBuilder();
        System.Threading.Thread t1 = new System.Threading.Thread(delegate () { try { so.Append(pr.StandardOutput.ReadToEnd()); } catch { } });
        System.Threading.Thread t2 = new System.Threading.Thread(delegate () { try { se.Append(pr.StandardError.ReadToEnd()); } catch { } });
        t1.IsBackground = true; t2.IsBackground = true; t1.Start(); t2.Start();
        pr.WaitForExit(); t1.Join(8000); t2.Join(8000);
        return so.ToString() + se.ToString();
    }
    byte[] BmpTo565(Bitmap src, int w, int h) {
        byte[] o = new byte[w * h * 2];
        using (Bitmap b = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format24bppRgb)) {
            using (Graphics g = Graphics.FromImage(b)) { g.Clear(Color.Black); g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.PixelOffsetMode = PixelOffsetMode.HighQuality; g.CompositingQuality = CompositingQuality.HighQuality; g.DrawImage(src, 0, 0, w, h); }
            System.Drawing.Imaging.BitmapData bd = b.LockBits(new Rectangle(0, 0, w, h), System.Drawing.Imaging.ImageLockMode.ReadOnly, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
            byte[] row = new byte[bd.Stride]; int i = 0;
            for (int y = 0; y < h; y++) {
                System.Runtime.InteropServices.Marshal.Copy(IntPtr.Add(bd.Scan0, y * bd.Stride), row, 0, bd.Stride);
                for (int x = 0; x < w; x++) { int si = x * 3; int B = row[si], G = row[si+1], R = row[si+2]; int v = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3); o[i++] = (byte)(v & 0xFF); o[i++] = (byte)(v >> 8); }
            }
            b.UnlockBits(bd);
        }
        return o;
    }
    void RefreshMedia() {
        lstMedia.Items.Clear(); long total = 6 + mFrames.Count * 6; int dur = 0;
        for (int i = 0; i < mFrames.Count; i++) { total += mFrames[i].Length; dur += mDelays[i]; lstMedia.Items.Add((i + 1) + ". " + mFrames[i].Length + "B  " + mDelays[i] + "ms"); }
        lblMediaInfo.Text = "帧数: " + mFrames.Count + " | 分辨率: " + mW + "×" + mH + " | 大小: " + (total / 1048576.0).ToString("0.00") + "MB | 时长≈" + (dur / 1000.0).ToString("0.0") + "s";
    }
    bool ConfirmImportMode() {
        if (mFrames.Count == 0) return true;
        DialogResult r = MessageBox.Show("列表里已有 " + mFrames.Count + " 帧。\n\n【是】= 清空后导入（替换）\n【否】= 追加到末尾\n【取消】= 不导入", "导入方式", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        if (r == DialogResult.Cancel) return false;
        if (r == DialogResult.Yes) { mFrames.Clear(); mDelays.Clear(); }
        return true;
    }
    void MediaAddImages() {
        OpenFileDialog d = new OpenFileDialog(); d.Multiselect = true;
        d.Filter = "图片 (*.png;*.jpg;*.jpeg;*.bmp;*.webp)|*.png;*.jpg;*.jpeg;*.bmp;*.webp|所有文件 (*.*)|*.*";
        if (d.ShowDialog() != DialogResult.OK) return;
        if (!ConfirmImportMode()) return;
        foreach (string p in d.FileNames) { try { using (Bitmap b = new Bitmap(p)) { mFrames.Add(BmpTo565(b, mW, mH)); mDelays.Add((int)numHold.Value); } } catch {} }
        RefreshMedia();
    }
    int MaxFrames() { int fb = mW * mH * 2; long cap = imgCapBytes > 0 ? imgCapBytes : (long)(flashTotal - 0.3 * 1048576); int mf = (int)(cap / fb); return mf < 1 ? 1 : mf; }
    void MediaAddVideo() {
        OpenFileDialog d = new OpenFileDialog();
        d.Filter = "视频/GIF (*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.gif;*.mpg;*.mpeg;*.ts;*.3gp)|*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.gif;*.mpg;*.mpeg;*.ts;*.3gp|所有文件 (*.*)|*.*";
        if (d.ShowDialog() != DialogResult.OK) return;
        if (!ConfirmImportMode()) return;
        string ff = FindFfmpeg(); int fps = (int)numFps.Value; int maxF = MaxFrames();
        // 先探测时长(秒), 估算帧数 -> 超上限直接拦(不抽帧)
        string ffprobe = ff.ToLower().EndsWith("ffmpeg.exe") ? ff.Substring(0, ff.Length - 10) + "ffprobe.exe" : "ffprobe";
        double durSec = 0;
        try { string so = RunCapture(ffprobe, "-v error -show_entries format=duration -of default=nw=1:nk=1 \"" + d.FileName + "\""); double.TryParse(so.Trim(), NumberStyles.Any, CultureInfo.InvariantCulture, out durSec); } catch {}
        if (durSec > 0) {
            long est = (long)(durSec * fps);
            if (est > maxF) {
                DialogResult r = MessageBox.Show("视频约 " + est + " 帧，超过上限 " + maxF + " 帧（≈" + (maxF / (double)fps).ToString("0.0") + "s）。\n\n【是】= 只导入前 " + maxF + " 帧\n【否】= 取消\n\n想放更久：勾选「压缩 (120×120)」可放约 570 帧，或把帧率调低。", "视频太长", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (r != DialogResult.Yes) return;
            }
        }
        string tmp = Path.Combine(Path.GetTempPath(), "ov03_f_" + Environment.TickCount); Directory.CreateDirectory(tmp);
        lblMediaStatus.Text = "抽帧中(ffmpeg)..."; lblMediaStatus.ForeColor = Color.LightSkyBlue;
        string args = "-y -i \"" + d.FileName + "\" -vf \"fps=" + fps + ",scale=" + mW + ":" + mH + ":force_original_aspect_ratio=decrease,pad=" + mW + ":" + mH + ":(ow-iw)/2:(oh-ih)/2\" -frames:v " + maxF + " -pix_fmt rgb24 \"" + tmp + "\\f_%05d.png\"";
        try { RunCapture(ff, args); } catch (Exception ex) { lblMediaStatus.Text = "ffmpeg 失败: " + ex.Message; return; }
        string[] files = Directory.GetFiles(tmp, "f_*.png"); Array.Sort(files);
        int delay = Math.Max(20, 1000 / fps);
        foreach (string p in files) { try { using (Bitmap b = new Bitmap(p)) { mFrames.Add(BmpTo565(b, mW, mH)); mDelays.Add(delay); } } catch {} }
        try { Directory.Delete(tmp, true); } catch {}
        RefreshMedia(); lblMediaStatus.Text = "已加入 " + files.Length + " 帧";
    }
    void MediaReScale(int nw, int nh) {
        List<byte[]> nf = new List<byte[]>();
        for (int i = 0; i < mFrames.Count; i++) {
            try {
                using (Bitmap b = new Bitmap(mW, mH)) {
                    for (int y = 0; y < mH; y++) for (int x = 0; x < mW; x++) { int si = (y * mW + x) * 2; int v = mFrames[i][si] | (mFrames[i][si + 1] << 8); b.SetPixel(x, y, Color.FromArgb(((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)); }
                    nf.Add(BmpTo565(b, nw, nh));
                }
            } catch { }
        }
        mFrames = nf; mW = nw; mH = nh; RefreshMedia();
    }
    void MediaMove(int dir) {
        int i = lstMedia.SelectedIndex; if (i < 0) return; int j = i + dir; if (j < 0 || j >= mFrames.Count) return;
        byte[] f = mFrames[i]; mFrames[i] = mFrames[j]; mFrames[j] = f;
        int dl = mDelays[i]; mDelays[i] = mDelays[j]; mDelays[j] = dl;
        RefreshMedia(); lstMedia.SelectedIndex = j;
    }
    void MediaDel() { int i = lstMedia.SelectedIndex; if (i < 0) return; mFrames.RemoveAt(i); mDelays.RemoveAt(i); RefreshMedia(); }
    Bitmap BmpFrom565(byte[] d, int w, int h) {
        Bitmap b = new Bitmap(w, h, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
        System.Drawing.Imaging.BitmapData bd = b.LockBits(new Rectangle(0, 0, w, h), System.Drawing.Imaging.ImageLockMode.WriteOnly, System.Drawing.Imaging.PixelFormat.Format24bppRgb);
        byte[] row = new byte[bd.Stride];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) { int si = (y*w+x)*2; int v = (si + 1 < d.Length) ? (d[si] | (d[si+1] << 8)) : 0; int di = x*3; row[di] = (byte)((v & 31) << 3); row[di+1] = (byte)(((v >> 5) & 63) << 2); row[di+2] = (byte)(((v >> 11) & 31) << 3); }
            System.Runtime.InteropServices.Marshal.Copy(row, 0, IntPtr.Add(bd.Scan0, y * bd.Stride), bd.Stride);
        }
        b.UnlockBits(bd);
        return b;
    }
    void ShowFrame() {
        int i = lstMedia.SelectedIndex;
        if (i < 0 || i >= mFrames.Count) { if (picPrev.Image != null) { picPrev.Image.Dispose(); picPrev.Image = null; } return; }
        try { Image old = picPrev.Image; picPrev.Image = BmpFrom565(mFrames[i], mW, mH); if (old != null) old.Dispose(); } catch {}
        loading = true; numFrameDur.Value = Math.Max(numFrameDur.Minimum, Math.Min(numFrameDur.Maximum, mDelays[i])); loading = false;
    }
    void MediaPlay() {
        if (mFrames.Count == 0) return;
        mediaPlayIdx = lstMedia.SelectedIndex >= 0 ? lstMedia.SelectedIndex : 0;
        mediaNextAt = Environment.TickCount + 200; mediaTimer.Start();
    }
    void EnsureConnect() {
        if (sp.IsOpen) return;
        if (cboPort.SelectedIndex < 0) RefreshPorts();
        if (cboPort.SelectedIndex < 0) return;
        try { sp.PortName = cboPort.Text; sp.BaudRate = 115200; sp.ReadTimeout = 200; sp.WriteTimeout = 8000; sp.DtrEnable = true; sp.RtsEnable = true; sp.Open(); btnConn.Text = "断开"; SetStatus("已连接 " + sp.PortName); }
        catch (Exception ex) { SetStatus("连接失败(可能被 OV-03 占用): " + ex.Message); }
    }
    void ReadMedia() {
        EnsureConnect();
        if (!sp.IsOpen) { lblMediaStatus.Text = "串口未连接, 请先在③里选 COM 口并点「连接」"; return; }
        if (mediaBusy) return;
        mediaBusy = true; lblMediaStatus.ForeColor = Color.LightSkyBlue; lblMediaStatus.Text = "读取中...";
        System.Threading.Thread t = new System.Threading.Thread(delegate () { DoReadMedia(); }); t.IsBackground = true; t.Start();
    }
    string ReadLine(int ms) {
        int dl = Environment.TickCount + ms; StringBuilder sb = new StringBuilder();
        while (Environment.TickCount < dl) {
            int av = 0; try { av = sp.BytesToRead; } catch {}
            if (av > 0) {
                byte[] b = new byte[av]; int n = 0; try { n = sp.Read(b, 0, av); } catch {}
                for (int i = 0; i < n; i++) {
                    if (b[i] == 10) { for (int j = i + 1; j < n; j++) rxLeft.Add(b[j]); return sb.ToString(); }   // 换行后的字节留缓冲
                    sb.Append((char)b[i]);
                }
            } else System.Threading.Thread.Sleep(3);
        }
        return sb.Length > 0 ? sb.ToString() : null;
    }
    void DoReadMedia() {
        try {
            sp.DiscardInBuffer(); rxLeft.Clear();
            sp.Write("I0\n"); System.Threading.Thread.Sleep(120);
            sp.Write(";\n");
            string line = ReadLine(10000);
            if (line == null || !line.Contains("#IMGRD")) { MStat("无 #IMGRD"); return; }
            string[] pp = line.Trim().Split(' ');
            if (pp.Length < 2 || pp[1] == "0") { MStat("Pico 里没有媒体"); return; }
            int mode = int.Parse(pp[1]), w = int.Parse(pp[2]), h = int.Parse(pp[3]); long len = long.Parse(pp[4]);
            MStat("读取中... " + (len/1048576.0).ToString("0.00") + "MB");
            byte[] data = new byte[len]; int off = 0; int dl0 = Environment.TickCount + 120000; int lastP = -10;
            while (off < len && rxLeft.Count > 0) { data[off++] = rxLeft[0]; rxLeft.RemoveAt(0); }
            while (off < len && Environment.TickCount < dl0) {
                int av = 0; try { av = sp.BytesToRead; } catch {}
                if (av > 0) { int n = 0; try { n = sp.Read(data, off, Math.Min(av, (int)(len - off))); } catch {} off += n;
                    int pct = (int)(off * 100L / len); if (pct >= lastP + 5) { lastP = pct; MStat("读取中... " + pct + "% (" + (off/1048576.0).ToString("0.0") + "/" + (len/1048576.0).ToString("0.0") + "MB)"); } }
                else System.Threading.Thread.Sleep(4);
            }
            if (off < len) { MStat("读取超时 " + off + "/" + len); return; }
            List<byte[]> nf = new List<byte[]>(); List<int> nd = new List<int>(); int nw = w, nh = h;
            if (mode == 2) {
                int cnt = data[0] | (data[1] << 8); nw = data[2] | (data[3] << 8); nh = data[4] | (data[5] << 8);
                int to = 6, cum = 6 + cnt * 6;
                for (int i = 0; i < cnt; i++) {
                    int dl = data[to] | (data[to+1] << 8); long ln = (uint)(data[to+2] | (data[to+3] << 8) | (data[to+4] << 16) | (data[to+5] << 24)); to += 6;
                    if (cum + ln > data.Length) break; byte[] fr = new byte[ln]; Array.Copy(data, cum, fr, 0, (int)ln); nf.Add(fr); nd.Add(dl); cum += (int)ln;
                }
            } else { nf.Add(data); nd.Add(1000); }
            BeginInvoke((MethodInvoker)delegate { mFrames = nf; mDelays = nd; mW = nw; mH = nh; chkZip.Checked = (nw <= 120); RefreshMedia(); lblMediaStatus.ForeColor = Color.LightGreen; lblMediaStatus.Text = "已读取 " + nf.Count + " 帧 (" + nw + "×" + nh + ")"; });
        } catch (Exception ex) { MStat("读取失败: " + ex.Message); }
        finally { try { BeginInvoke((MethodInvoker)delegate { mediaBusy = false; }); } catch {} }
    }
    void ExportPng() {
        if (mFrames.Count == 0) { lblMediaStatus.Text = "没有帧"; return; }
        FolderBrowserDialog fb = new FolderBrowserDialog(); fb.Description = "选择导出 PNG 的文件夹";
        if (fb.ShowDialog() != DialogResult.OK) return;
        int n = 0;
        for (int i = 0; i < mFrames.Count; i++) { try { using (Bitmap b = BmpFrom565(mFrames[i], mW, mH)) b.Save(Path.Combine(fb.SelectedPath, "frame_" + (i + 1).ToString("D4") + ".png"), System.Drawing.Imaging.ImageFormat.Png); n++; } catch {} }
        lblMediaStatus.Text = "已导出 " + n + " 张 PNG";
    }
    void MediaTick() {
        if (mFrames.Count == 0) { mediaTimer.Stop(); return; }
        if ((int)(Environment.TickCount - mediaNextAt) < 0) return;
        if (mediaPlayIdx < 0 || mediaPlayIdx >= mFrames.Count) mediaPlayIdx = 0;
        lstMedia.SelectedIndex = mediaPlayIdx;
        int dl = mDelays[mediaPlayIdx]; if (dl < 20) dl = 20;
        mediaNextAt = Environment.TickCount + dl;
        mediaPlayIdx = (mediaPlayIdx + 1) % mFrames.Count;
    }
    void UploadMedia() {
        EnsureConnect();
        if (!sp.IsOpen) { lblMediaStatus.Text = "串口未连接, 请先在③里选 COM 口并点「连接」"; return; }
        if (mFrames.Count == 0) { lblMediaStatus.Text = "还没有帧, 先添加图片/视频"; return; }
        int count = mFrames.Count;
        long total = 6 + (long)count * 6; foreach (byte[] fr in mFrames) total += fr.Length;
        long usable = imgCapBytes > 0 ? imgCapBytes : (flashTotal - (long)(0.3 * 1048576));
        if (total > usable) { lblMediaStatus.ForeColor = Color.OrangeRed; lblMediaStatus.Text = "文件过大: 需 " + (total/1048576.0).ToString("0.00") + "MB > 上限 " + (usable/1048576.0).ToString("0.0") + "MB, 超出 flash 容量, 无法烧录"; return; }
        byte[] blob = new byte[total]; int o = 0;
        blob[o++] = (byte)(count & 0xFF); blob[o++] = (byte)((count >> 8) & 0xFF);
        blob[o++] = (byte)(mW & 0xFF); blob[o++] = (byte)((mW >> 8) & 0xFF);
        blob[o++] = (byte)(mH & 0xFF); blob[o++] = (byte)((mH >> 8) & 0xFF);
        for (int i = 0; i < count; i++) { int dl = (i < mDelays.Count) ? mDelays[i] : 100; blob[o++] = (byte)(dl & 0xFF); blob[o++] = (byte)((dl >> 8) & 0xFF); int ln = mFrames[i].Length; blob[o++] = (byte)ln; blob[o++] = (byte)(ln >> 8); blob[o++] = (byte)(ln >> 16); blob[o++] = (byte)(ln >> 24); }
        foreach (byte[] fr in mFrames) { Array.Copy(fr, 0, blob, o, fr.Length); o += fr.Length; }
        mediaBusy = true; btnMediaUp.Enabled = false;
        lblMediaStatus.ForeColor = Color.LightSkyBlue; lblMediaStatus.Text = "准备上传 " + (total/1048576.0).ToString("0.00") + "MB ...";
        System.Threading.Thread t = new System.Threading.Thread(delegate () { DoUploadMedia(blob, count, total); });
        t.IsBackground = true; t.Start();
    }
    void MStat(string s) { try { BeginInvoke((MethodInvoker)delegate { lblMediaStatus.Text = s; }); } catch {} }
    void DoUploadMedia(byte[] blob, int count, long total) {
        try {
            sp.DiscardInBuffer();
            sp.Write("H1\n"); System.Threading.Thread.Sleep(3500); sp.Write("I0\n"); System.Threading.Thread.Sleep(100); sp.DiscardInBuffer();   // 等 3.5s: 上次若中断, 板子 3s 超时自动放弃
            sp.Write("^2," + total + "\n");
            if (!WaitFor("#IMGRDY", 30000)) { MStat("无 #IMGRDY (上传未开始)"); return; }
            int off = 0, lastPct = -10;
            while (off < blob.Length) {
                int n = Math.Min(4096, blob.Length - off); sp.Write(blob, off, n); off += n;
                if (off >= blob.Length) { if (!WaitFor("#IMGOK", 90000)) { MStat("末尾无 #IMGOK"); return; } }
                else if (!WaitFor("#IMGC", 20000)) { MStat("分块超时 @" + off + "/" + blob.Length); return; }
                int pct = (int)(off * 100L / blob.Length);
                if (pct >= lastPct + 5) { lastPct = pct; MStat("上传中... " + pct + "% (" + (off/1048576.0).ToString("0.0") + "/" + (total/1048576.0).ToString("0.0") + "MB)"); }
            }
            MStat("已上传 " + count + " 帧 (" + (total/1048576.0).ToString("0.00") + "MB), 开始循环播放 ✓");
            try { BeginInvoke((MethodInvoker)delegate { lblMediaStatus.ForeColor = Color.LightGreen; QueryFlash(); }); } catch {}
        } catch (Exception ex) { MStat("上传失败: " + ex.Message); }
        finally { try { BeginInvoke((MethodInvoker)delegate { mediaBusy = false; btnMediaUp.Enabled = true; }); } catch {} }
    }

    [STAThread]
    static void Main()
    {
        bool made;
        System.Threading.Mutex mx = new System.Threading.Mutex(true, "303paint_App", out made);
        if (!made) { MessageBox.Show("303paint 已经打开了。", "303paint", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        System.Threading.Mutex other = null;
        try { other = System.Threading.Mutex.OpenExisting("OV-03_App"); } catch {}
        if (other != null) { MessageBox.Show("OV-03 正在运行。两者不能同时打开（串口会冲突），请先关闭 OV-03。", "303paint", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        Application.EnableVisualStyles();
        Application.Run(new Editor());
    }
}
