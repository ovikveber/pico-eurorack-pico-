# OV-JP8000

用 **OV-303 的硬件**（Pico 2 / RP2350、ADS8688、PCM5102、ST7789、AD5664）实现的
**JP-8000 风格 SuperSaw 复音合成器**。硬件不变，只换固件和旋钮映射。

> **状态：可用（当前 v37，2026-10-19）。**
> 375MHz @ 1.30V、8 声部、PIO 小屏、看门狗自动恢复；双 ADSR（AMP + FILTER 同时生效），
> 8 旋钮 + 3 开关。开发过程见 [`OV-JP8000日志.md`](OV-JP8000日志.md)。

## 功能

- 8 声部 SuperSaw VCO：每声部 7 个带限锯齿，detune / mix / 随机相位 / 立体声
- 2048 点带限 mipmap 锯齿表（最小 512 点），线性插值
- JP-8000 风格 VCF：TPT SVF，12/24 dB，LP / BP / HP，带共振
- **双 ADSR**：AMP 包络控制 VCA，FILTER 包络控制 cutoff，两套同时工作；
  SW1 只切换“旋钮编辑/屏幕显示”的对象
- 包络曲线对标 Vital：`powerScale(power=-2)`，D/R 为实际衰减时间（0.5ms–20s），
  release 从 noteOff 当前电平开始，时间固定
- 立体声：7 个锯齿按 detune 左右 pan，每声部 L/R 各一套 SVF，`i2s.write16(L,R)`
- CV/Gate：AD5664 输出最高音 CV、Gate、滤波包络、AMP 包络
- 小屏显示 8 个参数，24Hz 局部刷新，非阻塞 DMA

## 硬件

复用 OV-303 板：ABrobot RP2350A（Pico 2 封装）、ADS8688、PCM5102A、ST7789、AD5664。
完整引脚见 [`../OV-03开源文件/硬件/OV-03接线表.txt`](../OV-03开源文件/硬件/OV-03接线表.txt)。

| 面板 | 功能 |
| --- | --- |
| TUNE | Cutoff |
| CUTOFF | Resonance |
| RES | Decay（当前编辑目标） |
| ENV.MOD | Sustain（当前编辑目标） |
| DECAY | Release（当前编辑目标） |
| ACCENT | Filter Env Depth（±6 oct，中心 0） |
| DIST | Detune |
| COLOR | Mix |
| SW1 | ADSR 编辑对象：低 = AMP，高 = FILTER |
| SW2 | 滤波器 12 / 24 dB |
| SW3 | LP → BP → HP |

## 构建 / 烧录

1. Arduino IDE 2.x + Earle Philhower RP2350 核心，装 Adafruit TinyUSB。
2. 选择 ABrobot RP2350A / 兼容 RP2350，USB Stack 选 **TinyUSB**。
3. 打开 `pico_jp8000/pico_jp8000.ino`（同目录含 `font5x7.h`、`lcd_spi.pio.h`）。
4. 编译上传；或按住 BOOTSEL 刷 `firmware/pico_jp8000_latest.uf2`。

```c
// 375MHz 前必须同步 QMI/XIP 读分频，否则 USB 枚举失败
cd = ceil(CPU_KHZ / 100MHz);
qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~0xFF) | cd;
dummy XIP read;
set_sys_clock_khz(...);
```

## 文件

```
pico_jp8000/pico_jp8000.ino    固件
pico_jp8000/lcd_spi.pio.h      小屏 PIO 驱动
pico_jp8000/font5x7.h          5x7 字库
firmware/pico_jp8000_latest.uf2
参考资料/Roland-JP-8000-Service-Notes.pdf
OV-JP8000日志.md               完整开发日志（主要文档）
```

## 已知 / 待办

- 375MHz / 8 声部下 core1 约 55–58%；若现场爆音/卡顿，先降 6 声部或 300MHz
- 滤波“味道”：当前 TPT SVF；后续按听感调共振映射/驱动/键跟踪
- 更高采样率 / 更干净高频：4096 点表或 2× 过采样（CPU 代价大）
- 屏幕目前只显示参数，未做菜单/预设

## 协议

- 固件：GPL-3.0-or-later
- 硬件设计文件（OV-303）：CERN-OHL-S-2.0
- `TB-303` / `JP-8000` 是 Roland 的商标，本项目为独立复刻，仅供学习与 DIY

---

## English summary

A JP-8000-style SuperSaw polyphonic synth running on OV-303 hardware (RP2350, ADS8688,
PCM5102A, ST7789, AD5664). 8 voices × 7 band-limited saws, TPT SVF (12/24 dB, LP/BP/HP),
dual ADSR (AMP + FILTER running simultaneously) with Vital-style `powerScale` curves,
stereo detune spread, PIO-driven 24 Hz partial LCD refresh, watchdog recovery.
Current version v37 at 375 MHz. Status: working.
