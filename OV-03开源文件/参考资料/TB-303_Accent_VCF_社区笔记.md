# TB-303 accent / VCF — 社区/博主 笔记（本项目用）

## tinyloops.com — Sound: Accent
来源: https://tinyloops.com/tb303/sound_accent.html
- 引 Whittle：**连续重音时 1µF 没放完 → 后续响应曲线更高**。
- 频谱观测：某些时刻出现"**三层谐振**"（奇怪）。
- 得到"音高不断上升"的错觉 = **Shepard tone** 效应。
- 设置建议：**重音效果要好，必须"大量谐振 + Accent 拉满"；Decay 对重音无效（任意值都行）**；
  **Tuning 低、Cut off 在 1/3**（留出低通上扫的余量）。TEMPO 微调会改变扫频方向感。

## Eddy Bergman — TB-303 VCF 复刻（2025-03，修订 v3）
来源: https://www.eddybergman.com/2025/03/TB303-VCF.html
- 24dB/oct 晶体管梯形；梯形里的管子 B-C 短接当二极管用（每级=二极管+电容）。**需要配对管**。
- **"Resonance needs to be open to self oscillation"**；**"cutoff 在电位器行程的前段最有效"**（→ 对数/CW 集中）。
- **"典型梯形毛病：谐振升高 → 音量下降、低音变少"**（Moog ladder/Doepfer A-103 同样）→ 高谐振掉音量/掉低音。
- **"输入滤波的包络信号必须 Attack=0（瞬间 Attack），因为原始 303 没有 Attack 旋钮，只有 Decay。"**
  → **MEG 起音是"瞬间"**（我们目前 3ms）。
- 参考 Tim Stinchcombe 的 TB-303 ladder 分析（timstinchcombe.co.uk）。

## Arperture/tb303-monosynth（GitHub）
- **"Slide = pitch CV 上 ~60ms RC glide，gate 在整个转换期间保持高，包络不重触发。"**
- **"Accent = 提升 VCA + 加一个固定快速滤波扫频 + 忽略 DECAY 旋钮（重音音总是短扫）。"**

## r/synthesizers（社区）
- 歌 C62 在 gate 上升时被快速脉冲充电，经 **Decay 电位器**放电（~0.6V 后…）。
  **重音时 Decay 电位器被短路，只用串联的 68k 电阻 → 最短 decay 时间。**

## modwiggler（RE-303 decay CV mod）
- **303 里本来就有个模拟开关跨在 decay 电位器上**，接 accent 信号 → 证实"accent 短路 decay 电位器"。

## 结论（对本项目）
- accent = ①短路 Decay→MEG 最短、②VCA 控制电流加量（47k/0.033µF）、③滤波频率 sweep（1µF，速度/量由 **res 电位器第二联**控）＋累积。**不改 res 数值**。
- 高谐振时：音量/低音会掉（梯形特性）+ sweep 峰更突出 → 听感像"accent 动到 res"。
- **MEG 起音应为"瞬间(≈0)"**（我们用了 3ms，可能偏慢）。
