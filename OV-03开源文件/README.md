<a id="chinese"></a>
# pico303 / OV-03 开源文件

[中文](#chinese) | [English](#english)

在 **RP2350** 上实现的 TB-303 风格 acid bass 合成器，包含固件、PC 端编辑器、硬件设计、
物料清单和参考资料。

> 注意：本项目只支持 RP2350。实测两块 RP2040 板子都无法正常运行，不要用 RP2040 烧录。

## 项目说明

本项目已经做成很久了，直到今天我才发现其实可以有更多的选择。例如 DAC 有更好的选择，
更便宜的，也不需要基准电压；ADS8688 用于读电位器有点暴殄天物。所以这个只当一个记录项目。
你当然可以复刻，但是我只能说，如果换成我现在新做一个的话会做得更好：电路可能会简化许多，
删掉许多不需要的芯片以腾出位置；甚至 PCM5102 和 6.35 插座有冲突，6.35 插座焊盘过小插不进去。
这些都是问题。

如果你能接受这些并且自己可以进行修改的话，我是非常赞同你去复刻的；但是如果不能的话，
我建议不要碰，因为真的一坨……

当然你也可以直接买一个 Pico 2 / RP2350A 或 B，然后直接烧录文件和运行程序，那样的话
你可以不需要外围的电路，也能游玩这个项目。所以外围电路不是必需品，这取决于你。

## DSP 引擎

当前使用的是 **Syrebas** 电路级复刻引擎（MIT）：
https://github.com/holystebroe/Syrebas

- 振荡器：连续锯齿核 + polyBLEP；方波由锯齿比较导出，占空比随音高在 45%..70% 之间变化
- 滤波器：4 级耦合二极管梯形，RK2 中点过采样，级间 tanh 非线性，动态反馈高通
  （150..250Hz），可自激；过采样倍数 4/8/12/16 可配置
- VCA、两个包络、accent、slide 按 303 的行为实现
- 早期版本用过 Open303，现在已经不是 Open303 引擎

## 功能

- 锯齿/方波振荡器，tanh 软削波方波，支持 slide 和 accent
- 4 级二极管梯形低通，带共振和自激
- 滤波包络（env mod）、AMP 包络、accent（力度 >95）、60ms 滑音
- 16 步音序器，支持保存 pattern、连音和休止
- 1.54 寸 ST7789 LCD、EC11 编码器、WS2812 RGB
- USB MIDI（TinyUSB），AD5664 输出 CV / Gate / Accent / 滤波包络
- PC 编辑器皮肤、pattern 交换文件

## 硬件

- 主控：ABrobot RP2350A（Pico 封装），**不支持 RP2040**
- ADC：ADS8688，8 通道：TUNE / CUTOFF / RES / ENV.MOD / DECAY / ACCENT / DIST / COLOR
- DAC：AD5664，A=CVOUT B=GATEOUT C=ACCENTOUT D=FTENVOUT
- 开关：SW1 WAVE=GP6，SW2 MORERES=GP7，SW3 BUTTON=GP8
- 音频 DAC：PCM5102A，BCLK=GP13，WS=GP14，DIN=GP15
- 编码器 A/B/SW = GP12/GP11/GP10，LCD 使用 sketch 里定义的 SPI/PIO 引脚

音频 DAC 小板使用 GY-PCM5102A 的 KiCad 开源复刻：

- GitHub：https://github.com/Giuseppe499/KiCad-GY-PCM5102A
- 协议：CERN-OHL-S-2.0
- 本项目的原理图、封装和图片在 `物料/音频DAC_PCM5102/`

完整接口和网络表见 `硬件/OV-03接线表.txt`。

## 目录

```
固件/                  OV-03_firmware_20261012_ac.uf2（最新固件，直接烧录）
源码/                  pico_tb303.ino + 4 个依赖文件、303seq.cs、303paint.cs、OV-03.iss
硬件/                  接线表、V1.2 EDA、初版 Gerber 和面板
物料/                  BOM，主控、屏、插座、DAC 的资料
软件/                  Windows 安装包（约 104MB，超过 GitHub 单文件限制，未入库）
皮肤/                  303paint 皮肤
音序/                  示例 .303 pattern
素材/                  面板图、字体、UI 素材
参考资料/              303 / x0xb0x / TD-3 相关资料（版权归原作者，仅供学习）
```

## 编译 / 烧录

### 直接烧录

按住 BOOTSEL，把 `固件/OV-03_firmware_20261012_ac.uf2` 拖进 U 盘。

### 从源码编译

1. Arduino IDE 2.x + Earle Philhower RP2350 核心。
2. 选择 ABrobot RP2350A / 兼容 RP2350 板，USB Stack 选 **TinyUSB**。
3. 把 `源码/` 里的 `pico_tb303.ino`、`font5x7.h`、`lcd_spi.pio`、`lcd_spi.pio.h`、
   `smiley_bmp.h` 放进一个名为 `pico_tb303` 的文件夹，再用 Arduino 打开。
4. 安装 Adafruit NeoPixel、Adafruit TinyUSB。
5. 编译上传。

## PC 软件

`源码/303seq.cs` 是音序器/编辑器，`源码/303paint.cs` 是皮肤编辑器，用 Visual Studio /
.NET WinForms 编译。`源码/OV-03.iss` 是 Inno Setup 安装脚本。

`软件/OV-03_v1.2_Setup.exe` 约 104MB，超过 GitHub 单文件 100MB 限制，所以没有提交到
仓库。需要安装包时请到 [Release 303-v1.2](https://github.com/ovikveber/pico-eurorack-pico-/releases/tag/303-v1.2)
下载，或者用源码自己编译。

## 注意

- 本仓库只有 pico303（OV-03）。原版 303 复刻的电路、外壳和仿真资料不在这个项目里。
- RP2040 不支持，只能上 RP2350。
- CV/Gate 输出在固件里有校准常量，接模块前先用万用表量一下。
- `TB-303` 是 Roland 的商标。本项目是独立复刻，仅供学习和 DIY，与 Roland 无关。
- `参考资料/` 里的文档版权归原作者，只用于学习研究。

## 协议 / 致谢

- 固件和 PC 软件：GPL-3.0-or-later
- 硬件设计文件：CERN-OHL-S-2.0
- Syrebas 引擎：MIT，https://github.com/holystebroe/Syrebas
- 音频 DAC 小板：CERN-OHL-S-2.0，https://github.com/Giuseppe499/KiCad-GY-PCM5102A
- 第三方文档保留各自版权

早期版本曾以 MIT 发布，已经分发出去的旧版本仍可按 MIT 使用；从当前版本起改为
GPL-3.0-or-later。

---

<a id="english"></a>
# pico303 / OV-03 open-source files

[中文](#chinese) | [English](#english)

A TB-303 style acid bass synthesizer on **RP2350**, including firmware, a PC editor,
hardware design, BOM and reference material.

> Note: this project only supports RP2350. Two RP2040 boards were tested and neither runs.
> Do not try to flash it to an RP2040.

## Project notes

This project was finished a long time ago. Only recently did I realize there are more
choices. For example, there are better and cheaper DACs that do not need a voltage
reference, and using an ADS8688 just to read potentiometers is overkill. So treat this as a
record of the project. You are welcome to clone it, but I can only say that if I were
building it today I would do it better: the circuit could be much simpler, many unnecessary
chips could be removed to free up space, and the PCM5102 even conflicts with the 6.35 mm
jack — the jack pads are too small and the plug does not fit. These are all real problems.

If you can accept them and modify the design yourself, I fully encourage you to clone it.
If not, I suggest you stay away, because it really is a mess...

Of course, you can also just buy a Pico 2 / RP2350A or B, flash the file and run the
program. Then you do not need the peripheral circuit at all and can still play with the
project. The peripheral circuit is optional; it is up to you.

## DSP engine

The current engine is the **Syrebas** circuit-level model (MIT):
https://github.com/holystebroe/Syrebas

- Oscillator: continuous saw core + polyBLEP; the square wave is derived by comparing the
  saw, with duty cycle varying with pitch between 45% and 70%
- Filter: 4-stage coupled diode ladder, RK2 midpoint oversampling, inter-stage tanh
  nonlinearity, dynamic feedback high-pass (150..250 Hz), capable of self-oscillation;
  oversampling factor 4/8/12/16 is configurable
- VCA, both envelopes, accent and slide follow 303 behavior
- Early versions used Open303; the current engine is no longer Open303

## Features

- Saw/square oscillator, tanh soft-clipped square, slide and accent
- 4-pole diode ladder low-pass with resonance and self-oscillation
- Filter envelope (env mod), amp envelope, accent (velocity >95), 60 ms slide
- 16-step sequencer with pattern save, tie and rest
- 1.54" ST7789 LCD, EC11 encoder, WS2812 RGB
- USB MIDI (TinyUSB), AD5664 outputs CV / Gate / Accent / filter envelope
- PC editor skins and pattern exchange files

## Hardware

- MCU: ABrobot RP2350A (Pico footprint), **RP2040 not supported**
- ADC: ADS8688, 8 channels: TUNE / CUTOFF / RES / ENV.MOD / DECAY / ACCENT / DIST / COLOR
- DAC: AD5664, A=CVOUT B=GATEOUT C=ACCENTOUT D=FTENVOUT
- Switches: SW1 WAVE=GP6, SW2 MORERES=GP7, SW3 BUTTON=GP8
- Audio DAC: PCM5102A, BCLK=GP13, WS=GP14, DIN=GP15
- Encoder A/B/SW = GP12/GP11/GP10, LCD uses the SPI/PIO pins defined in the sketch

The audio DAC board uses the open-source KiCad recreation of the GY-PCM5102A:

- GitHub: https://github.com/Giuseppe499/KiCad-GY-PCM5102A
- License: CERN-OHL-S-2.0
- Schematics, footprints and photos for this project are in `物料/音频DAC_PCM5102/`

See `硬件/OV-03接线表.txt` for the complete connector and net list.

## Contents

```
固件/                  OV-03_firmware_20261012_ac.uf2 (latest firmware, flash directly)
源码/                  pico_tb303.ino + 4 dependencies, 303seq.cs, 303paint.cs, OV-03.iss
硬件/                  wiring table, V1.2 EDA, initial Gerber and panel
物料/                  BOM, MCU / display / jack / DAC documentation
软件/                  Windows installer (~104 MB, over the GitHub file limit, not committed)
皮肤/                  303paint skins
音序/                  example .303 patterns
素材/                  panel artwork, fonts, UI assets
参考资料/              303 / x0xb0x / TD-3 reference material (copyright belongs to the authors, study only)
```

## Build / flash

### Flash a release

Hold BOOTSEL and drag `固件/OV-03_firmware_20261012_ac.uf2` onto the USB drive.

### Build from source

1. Arduino IDE 2.x with the Earle Philhower RP2350 core.
2. Select ABrobot RP2350A or a compatible RP2350 board and set the USB stack to **TinyUSB**.
3. Put `pico_tb303.ino`, `font5x7.h`, `lcd_spi.pio`, `lcd_spi.pio.h` and `smiley_bmp.h`
   from `源码/` into a folder named `pico_tb303`, then open it with Arduino.
4. Install Adafruit NeoPixel and Adafruit TinyUSB.
5. Compile and upload.

## PC software

`源码/303seq.cs` is the sequencer/editor and `源码/303paint.cs` is the skin editor. Build
them with Visual Studio / .NET WinForms. `源码/OV-03.iss` is the Inno Setup script.

`软件/OV-03_v1.2_Setup.exe` is about 104 MB, over the 100 MB GitHub file limit, so it is not
committed. Download it from the [303-v1.2 release](https://github.com/ovikveber/pico-eurorack-pico-/releases/tag/303-v1.2)
or build it from source.

## Notes

- This repository only contains pico303 (OV-03). The circuit, case and simulation files of
  the original 303 replica are not part of this project.
- RP2040 is not supported; use RP2350 only.
- The CV/Gate outputs use calibration constants in the firmware. Measure them with a
  multimeter before connecting a modular system.
- `TB-303` is a trademark of Roland. This is an independent clone for study and DIY use,
  not affiliated with or endorsed by Roland.
- Documents in `参考资料/` remain the property of their authors and are included for study
  only.

## License / credits

- Firmware and PC software: GPL-3.0-or-later
- Hardware design files: CERN-OHL-S-2.0
- Syrebas engine: MIT, https://github.com/holystebroe/Syrebas
- Audio DAC board: CERN-OHL-S-2.0, https://github.com/Giuseppe499/KiCad-GY-PCM5102A
- Third-party documents keep their own copyright

Earlier versions were published under MIT. Copies distributed under MIT keep those rights;
from the current version onward the project is GPL-3.0-or-later.
