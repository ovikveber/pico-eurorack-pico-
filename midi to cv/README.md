<a id="chinese"></a>
# midi to cv

[中文](#chinese) | [English](#english)

RP2040 USB-MIDI 转 8 路 CV/Gate 转换器。一颗 DAC8568 提供 8 路 16bit 输出，使用内部
2.5V 基准（0-5V）；8 路 Gate 经过缓冲，跟随每个声部的音符堆栈。

> **状态：开发中。** PCB 已于 2026-10-22 到货；左半 4 组 CV/Gate 已焊并验证，
> ST7789 屏幕 + 编码器通道设置 UI 已完成。当前固件：
> `firmware/midi2cv_random_20261022.uf2`。CPU 420MHz（PLL 校验，失败回退 125MHz），
> 屏幕 SCLK 52.5MHz。右半 5–8 组、私有总线和校准仍待做。

## 功能

- 8 个声部，每个声部独立 MIDI 通道（1..16，保存在 EEPROM）
- CV 支持 **1V / 2V / 5V per octave**；Gate 极性 **+ / -** 可设置
- ST7789 240×240 单列可滚动 UI：
  - MIDI 模式：V1–V8、GATE、V/OCT，共 10 行；通道行有蓝色通道条和 gate 指示灯
  - Clock 模式（IO24）：组1–4 八个插孔=C1–C8，组5–8 仍是 MIDI V5–V8
  - RandomSynth 模式（IO23）：粒子场地占满 240×222；底部左侧只显示 KNOB 绑定，
    右侧上一行 OUT 音名（含 cents 后缀），下一行 GATE/TRIG ON/OFF；
    编码器默认调场地大小，按下进入设置页
- EC11 编码器：旋转选行（1px 蓝框），按下进入编辑（橙框），再按退出并保存
- Clock：PLAY 行在列表最顶上；BPM 20–300 + docB PwmClock 的 24 个分数分频
- Clock 默认暂停；MIDI Start/Stop/Continue 也可控制；无 RUN/RESET 硬件输出
- RandomSynth：12 球起，碰撞生成 V/OCT、GATE、TRIG、FX；设置里有球大小/速度、
  波形、音高范围/音阶、PITCH/ADSR/FX 比例、加球、加速器、重置和 SHAPE
- RandomSynth 输出：**G1 CV=V/OCT，G2 CV=FX SEND，G1 Gate=GATE，G2 Gate=TRIG**
  （每组只有 1 个 DAC CV + 1 个 Gate，所以 FX 放在 G2 CV）
- RandomSynth 设置：V/OCT 量程 1/2/5、GATE/TRIG 极性、FX SEND 可调；
  BIND 行可把任意参数绑定到主页面编码器（主页面只能绑一个）
- RandomSynth 连续参数在 UI 上统一为 **0.00–1.00**（最大/最小效果不变）；
  但 RANGE 显示实际八度并保留 **2 位小数**
- **FINE** 按钮行在 RANGE 上面：OFF 时按 0.5 八度步进，ON 时按 0.01 八度微调
- 场地高度 222px；小球碰撞为柔和自身发光/闪白，没有外圈线条
- IO23 短按进入 Random；如果 IO23 未接/失效，**长按 IO24 约 0.8 秒**也可进入/退出 Random
- 编码器使用 GPIO 中断；指示灯只在状态变化时局部刷新；MIDI 模式不注册 clock 定时器
- RP2040 420MHz（1.3V，PLL/时钟源校验；失败回退 125MHz）
- ST7789 SCLK 52.5MHz（整数分频，12mA + fast slew），整帧约 17.55ms
- USB 描述符固定，避免 Windows 和 FL Studio 累积幽灵设备
- 私有数字总线（GP14）已预留，尚未实现

## 硬件

| 信号 | 引脚 |
| --- | --- |
| DAC SCLK / DIN / SYNC | GP28 / GP27 / GP26 |
| Gate V1..V8 | GP22 GP21 GP20 GP19 GP18 GP17 GP16 GP15 |
| 编码器 A / B / SW | GP13 / GP12 / GP11 |
| 数字总线 | GP14 |
| OLED SCL / SDA / RST / DC / CS / BL | GP10 / GP9 / GP8 / GP7 / GP6 / GP5 |

DAC：DAC8568（或 AD5668），32bit SPI 帧，MSB first。板上 `~LDAC` 硬接 GND，`~CLR`
接 5V。74AHCT541 负责 SPI 电平转换和 Gate 缓冲。

## 输出映射

DAC 通道顺序和插座顺序不是一一对应的（PCB 走线原因），以固件里的映射为准：

| 声部 | CV | Gate |
| --- | --- | --- |
| V1 | DAC B (1) | GP22 |
| V2 | DAC D (3) | GP21 |
| V3 | DAC F (5) | GP20 |
| V4 | DAC H (7) | GP19 |
| V5 | DAC A (0) | GP15 |
| V6 | DAC C (2) | GP16 |
| V7 | DAC E (4) | GP17 |
| V8 | DAC G (6) | GP18 |

## 编译

Arduino IDE 2.x + Earle Philhower RP2040 核心。本板是 16MB flash 的 RP2040，
编译用 Generic RP2040 + boot2 /4 + TinyUSB：

```
rp2040:rp2040:generic:flash=16777216_0,boot2=boot2_w25q128jvxq_4_padded_checksum,freq=125,usbstack=tinyusb
```

依赖库：Adafruit TinyUSB、EEPROM（RP2040 核心自带）。

固件运行时把 CPU 设到 **420MHz**（1.3V，PLL/时钟源校验，失败回退 125MHz），
屏幕 SCLK **62.5MHz**。`firmware/oc_test` 是超频验证工具：实测 420/432MHz 稳定，
436MHz 起会复位或挂死；500MHz 是 `clock_get_hz` 假象，实际停在 48MHz/ROSC。

## 校准

`NOTE0`（对应 0V 的 MIDI 音符）和 `CV_FULLSCALE_V` 是常量，DAC8568 内部基准准确，
一般不需要校准。MIDI 通道、V/oct 量程和 Gate 极性都在屏幕/编码器 UI 里设置。

## 文件

```
firmware/midi2cv/midi2cv.ino   主固件
firmware/midi2cv/Display.cpp    ST7789 PIO+DMA 驱动
firmware/midi2cv/Display.h
firmware/midi2cv/font5x7.h      5x7 字库
firmware/midi2cv/RandomWorld.h  RandomSynth 粒子世界
firmware/midi2cv/RandomWorld.cpp
firmware/screen_test/           屏幕/编码器测试 sketch
firmware/pin_scan/              GPIO 扫描排查工具
firmware/oc_test/               超频验证工具（420/432 稳定，436+ 不稳）
firmware/midi2cv_random_20261022.uf2  当前固件
firmware/screen_test_20261022_encoder.uf2  屏幕测试固件
hardware/                      BOM、Gerber、板子照片
docs/wiring-table.txt          引脚和接口表
docs/project-log.md            开发日志
```

## 协议

固件和软件：GPL-3.0-or-later。硬件设计文件：CERN-OHL-S-2.0。

---

<a id="english"></a>
# midi to cv

[中文](#chinese) | [English](#english)

An RP2040 USB-MIDI to 8x CV/Gate converter. A single DAC8568 provides eight 16-bit outputs
from its internal 2.5 V reference (0-5 V); eight buffered gate outputs follow the note stack
of each voice.

> **Status: in development.** The PCB arrived on 2026-10-22; the left 4 CV/Gate groups are
> soldered and verified, and the ST7789 + encoder channel UI is implemented. Current
> firmware: `firmware/midi2cv_random_20261022.uf2`. CPU runs at 420 MHz (PLL/source verified,
> falls back to 125 MHz); display SCLK is 52.5 MHz. Groups 5–8, the private bus and
> calibration are still pending.

## Features

- 8 voices, each with its own MIDI channel (1..16, stored in EEPROM)
- CV range **1V / 2V / 5V per octave**; gate polarity **+ / -**
- ST7789 240x240 single-column scrolling UI:
  - MIDI mode: V1–V8, GATE, V/OCT (10 rows); channel rows have a blue bar and gate LED
  - Clock mode (IO24): groups 1–4's eight jacks = C1–C8,
    groups 5–8 remain MIDI V5–V8
  - RandomSynth mode (IO23): the field fills 240×222; bottom-left shows only the KNOB
    binding, bottom-right has OUT note (with cents) on one line and GATE/TRIG ON/OFF
    on the next; the encoder adjusts field size by default, press to open settings
- EC11 encoder: rotate to select a row (1px blue box), press to edit (orange box),
  press again to exit and save
- Clock: the PLAY row is at the very top; BPM 20–300 + the 24 docB PwmClock ratios
- Clock defaults to paused; MIDI Start/Stop/Continue also work; no RUN/RESET hardware out
- RandomSynth: starts with 12 balls; collisions produce V/OCT, GATE, TRIG and FX.
  Settings cover ball size/speed, waves, pitch range/scale, PITCH/ADSR/FX ratios,
  add-ball, booster, reset and SHAPE
- RandomSynth outputs: **G1 CV=V/OCT, G2 CV=FX SEND, G1 Gate=GATE, G2 Gate=TRIG**
  (each group has only one DAC CV + one gate, so FX goes to the G2 CV jack)
- RandomSynth settings: V/OCT range 1/2/5, GATE/TRIG polarity and FX SEND.
  The BIND row binds any parameter to the main-page encoder (one at a time)
- RandomSynth continuous values display as **0.00–1.00** (min/max effect unchanged);
  RANGE is the exception: it shows actual octaves with **2 decimals**
- The **FINE** button row sits above RANGE and switches between 0.5-octave steps
  (OFF) and 0.01-octave fine steps (ON)
- The field is 222 px high and collision glow is a soft self-glow (no ring outline)
- IO23 short press enters Random; if IO23 is missing/dead, **hold IO24 ~0.8 s** instead
- Encoder runs on GPIO interrupts; LEDs update locally on change; MIDI mode has no clock timer
- RP2040 at 420 MHz (1.3 V, PLL/clock-source verified; falls back to 125 MHz)
- ST7789 SCLK 52.5 MHz (integer divider, 12 mA + fast slew), full frame ~17.55 ms
- Fixed USB descriptors so Windows and FL Studio do not accumulate ghost devices
- Private digital bus (GP14) is reserved but not implemented yet

## Hardware

| Signal | Pin |
| --- | --- |
| DAC SCLK / DIN / SYNC | GP28 / GP27 / GP26 |
| Gate V1..V8 | GP22 GP21 GP20 GP19 GP18 GP17 GP16 GP15 |
| Encoder A / B / SW | GP13 / GP12 / GP11 |
| Digital bus | GP14 |
| OLED SCL / SDA / RST / DC / CS / BL | GP10 / GP9 / GP8 / GP7 / GP6 / GP5 |

DAC: DAC8568 (or AD5668), 32-bit SPI frame, MSB first. The board hard-wires `~LDAC` to GND
and `~CLR` to 5 V. A 74AHCT541 handles SPI level shifting and gate buffering.

## Output mapping

The DAC channel order does not match the jack order because of PCB routing. The mapping in
the firmware is the reference:

| Voice | CV | Gate |
| --- | --- | --- |
| V1 | DAC B (1) | GP22 |
| V2 | DAC D (3) | GP21 |
| V3 | DAC F (5) | GP20 |
| V4 | DAC H (7) | GP19 |
| V5 | DAC A (0) | GP15 |
| V6 | DAC C (2) | GP16 |
| V7 | DAC E (4) | GP17 |
| V8 | DAC G (6) | GP18 |

## Build

Arduino IDE 2.x with the Earle Philhower RP2040 core. This board has 16 MB flash, so build
for Generic RP2040 with boot2 /4 and TinyUSB:

```
rp2040:rp2040:generic:flash=16777216_0,boot2=boot2_w25q128jvxq_4_padded_checksum,freq=125,usbstack=tinyusb
```

Libraries: Adafruit TinyUSB, EEPROM (bundled with the RP2040 core).

At runtime the firmware sets the CPU to **420 MHz** (1.3 V, PLL/clock-source verified,
falls back to 125 MHz) and the display SCLK to **52.5 MHz** (integer divider). `firmware/oc_test` is the
overclock verification tool: 420/432 MHz were stable; 436 MHz and above reset or hang.
The old 500 MHz reading was `clock_get_hz` lying; the clock was actually on 48 MHz/ROSC.

## Calibration

`NOTE0` (the MIDI note that maps to 0 V) and `CV_FULLSCALE_V` are constants because the
DAC8568 internal reference is accurate. MIDI channels, V/oct range and gate polarity are
set from the screen/encoder UI.

## Files

```
firmware/midi2cv/midi2cv.ino   main firmware
firmware/midi2cv/Display.cpp   ST7789 PIO+DMA driver
firmware/midi2cv/Display.h
firmware/midi2cv/font5x7.h     5x7 font
firmware/midi2cv/RandomWorld.h RandomSynth particle world
firmware/midi2cv/RandomWorld.cpp
firmware/screen_test/          screen/encoder test sketch
firmware/pin_scan/             GPIO scan bring-up tool
firmware/oc_test/              overclock verification tool (420/432 stable)
firmware/midi2cv_random_20261022.uf2       current firmware
firmware/screen_test_20261022_encoder.uf2  screen test firmware
hardware/                      BOM, Gerber archive, board photos
docs/wiring-table.txt          pin and connector table
docs/project-log.md            development log
```

## License

Firmware and software: GPL-3.0-or-later. Hardware design files: CERN-OHL-S-2.0.
