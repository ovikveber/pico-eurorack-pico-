# 可编程 VCO（programmable VCO）

JP-8000 风格 **SuperSaw 复音 VCO**，基于 Pico 2 / RP2350。带 4 路音频速率调制、
4 路慢速 CV 调制，以及 **Audio Matrix / CV Matrix** 两层调制矩阵。

> **状态：设计定稿（2026-10-20），未画原理图/PCB。**
> 方案、接口、芯片和矩阵已冻结；完整决策过程见
> [`设计决定日志.md`](设计决定日志.md)（以文末第 19 节为准）。

## 功能

- **8 声部**，每声部 **7 个锯齿**（RP2350 可做 8×7 = 56 个振荡器）
- PolyBLEP 抗混叠，立体声 spread
- **4 路 Audio Mod**（PCM1865，音频速率，AC 耦合）
- **4 路 CV Mod**（ADS8688，慢速/DC；CV1 = V/oct 锁定）
- **Audio Matrix**：4 Audio Mod × {FM, PM, PWM, RM, AM} = 20 个 amount
  - FM Lin / Exp 由面板钮子开关切换，切换时 5–10ms 交叉淡化
  - 只调 amount（静态发送量），depth 在 CV Matrix
- **CV Matrix**：4 CV Mod × 25 目标 = 100 个 depth
  - 目标 = 20 个 Audio Matrix cell + Detune / Mix / Shape / Spread / Waveform
  - destination-centric（类似 Serum）：先选目标，再调各 CV 到该目标的 depth
  - 只调 depth；多个 CV 送到同一目标时求和
- **Sync In**（比较器 + GPIO/PIO，Hard / Soft）
- **4 路 Audio Out**（PCM1681，只用 4 路），10Vpp
- **MIDI In**：3.5mm TRS Type A（A/B 跳线），6N138/6N137 光耦，8 声部分配 + voice stealing
- **USB-C**：烧录 / USB-MIDI

不做：LFO、Gate/Trig 输入、第二振荡器、CV Out / Gate Out / MIDI Out / Sync Out。

## 架构

```
MIDI In / USB-MIDI ──► Pico 2 (RP2350)
                         │
Audio In 1–4 ─ PCM1865 ──┼─► Audio Matrix 4×5 ─► FM / PM / PWM / RM / AM ─► DSP
                         │
CV In 1–4 ── ADS8688 ────┼─► CV Matrix 4×25 ─► 20 cells + 5 全局参数
                         │
Sync In ── 比较器 ───────┼─► Hard / Soft Sync
                         │
                         └─► PCM1681 (4ch) ─► OPA1612 ×2.67 ─► 10Vpp ×4
```

## 主要器件

| 功能 | 器件 | 说明 |
| --- | --- | --- |
| 主控 | Pico 2 / RP2350 | 音频 DSP + UI + MIDI |
| Audio ADC | PCM1865 | 4ch、24bit/192k、110dB、PGA、I2C/SPI |
| CV ADC | ADS8688 | 8ch（只用 4）、16bit、±10.24V、1MΩ、±20V OVP |
| Audio DAC | PCM1681 | 8ch（只用 4）、24bit/192k、105dB、5V/3.3V |
| 输出运放 | OPA1612 ×2 | 3.75Vpp → 10Vpp |
| 输入缓冲 | OPA4197 / OPA2320 | Audio In ×4 |
| MIDI 光耦 | 6N138 / 6N137 | 31250 baud |
| Sync | 比较器 | 不走 ADC |

Audio Mod 输入不能直接吃 10Vpp（PCM1865 绝对最大 −1.7～+5.0V），输入级：
`Jack → 1k + BAV99 → 100k 缓冲 → 0.5 衰减 → 10µF → PCM1865`，折算 SNR ≈ 104dB。

## 面板

- 接口：4 Audio In + 4 CV In + 4 Audio Out + MIDI In + Sync In + USB-C = **15 个**
- **18HP = 91.44mm** 偏密，**20HP = 101.6mm** 更舒服
- 3 个编码器 + 3 个按钮（PAGE / TARGET / SHIFT），3 编码器 × Shift = 6 个虚拟旋钮
- 1.3" ST7789 240×240（或 0.96" OLED）
- 页面：Audio Matrix / CV Matrix / CV Input / Global；预设 8–16 组

## 下一步

1. 按第 19 节冻结 BOM 和接口表
2. 原理图：Pico 2、PCM1865 输入级、ADS8688（4 路）、PCM1681（4 路）、MIDI In、Sync、电源
3. 固件架构：音频 DSP、ADS8688 扫描、PCM1865/PCM1681 配置、MIDI、UI、预设
4. UI 原型：3 编码器 + PAGE/TARGET/SHIFT、四页布局
5. 在 Pico 2 上做 32 / 56 saw DSP 基准
6. 面板 18HP / 20HP 布局定稿
7. 测试：V/oct 精度、10Vpp 输出、SNR、FM/PM/RM/AM、复音、预设、保护

## 协议

- 固件和软件：GPL-3.0-or-later
- 硬件设计文件：CERN-OHL-S-2.0

---

## English summary

A JP-8000-style SuperSaw polyphonic VCO on Pico 2 / RP2350. 8 voices × 7 band-limited
saws, 4 audio-rate modulation inputs (PCM1865), 4 slow CV inputs (ADS8688, CV1 = V/oct),
a 4×5 Audio Matrix (FM/PM/PWM/RM/AM) and a destination-centric 4×25 CV Matrix,
Sync input, 4× 10Vpp audio outputs (PCM1681), TRS MIDI and USB-C. 18–20HP panel.
Status: design frozen, schematic/PCB not started.
