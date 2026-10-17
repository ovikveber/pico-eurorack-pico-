<a id="chinese"></a>
# midi to cv

[中文](#chinese) | [English](#english)

RP2040 USB-MIDI 转 8 路 CV/Gate 转换器。一颗 DAC8568 提供 8 路 16bit 输出，使用内部
2.5V 基准（0-5V）；8 路 Gate 经过缓冲，跟随每个声部的音符堆栈。

## 功能

- 8 个声部，每个声部独立 MIDI 通道（1..16，保存在 EEPROM）
- 1V/oct CV 输出，可选 1/2/5/8 V/oct
- Gate 极性可配置
- EC11 编码器菜单：短按进入/下一项，长按退出并保存
- USB 描述符固定，避免 Windows 和 FL Studio 累积幽灵设备
- ST7789 OLED 和私有数字总线已预留，尚未实现

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

Arduino IDE 2.x + Earle Philhower RP2040 核心，板子选 **Raspberry Pi Pico**，
USB Stack 选 **TinyUSB**：

```
rp2040:rp2040:rpipico:usbstack=tinyusb
```

依赖库：Adafruit TinyUSB、EEPROM（RP2040 核心自带）。

## 校准

`NOTE0`（对应 0V 的 MIDI 音符）和 `CV_FULLSCALE_V` 是常量，DAC8568 内部基准准确，
一般不需要校准。V/oct、MIDI 通道和 Gate 极性在编码器菜单里设置。

## 文件

```
firmware/midi2cv/midi2cv.ino   固件
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

## Features

- 8 voices, each with its own MIDI channel (1..16, stored in EEPROM)
- 1V/oct CV output, selectable 1/2/5/8 V/oct
- Configurable gate polarity
- EC11 encoder menu: short press = enter / next item, long press = exit and save
- Fixed USB descriptors so Windows and FL Studio do not accumulate ghost devices
- ST7789 OLED and the private digital bus are reserved but not implemented yet

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

Arduino IDE 2.x with the Earle Philhower RP2040 core, board **Raspberry Pi Pico**, USB
stack **TinyUSB**:

```
rp2040:rp2040:rpipico:usbstack=tinyusb
```

Libraries: Adafruit TinyUSB, EEPROM (bundled with the RP2040 core).

## Calibration

`NOTE0` (the MIDI note that maps to 0 V) and `CV_FULLSCALE_V` are constants because the
DAC8568 internal reference is accurate. V/oct, MIDI channels and gate polarity are set
from the encoder menu.

## Files

```
firmware/midi2cv/midi2cv.ino   firmware
hardware/                      BOM, Gerber archive, board photos
docs/wiring-table.txt          pin and connector table
docs/project-log.md            development log
```

## License

Firmware and software: GPL-3.0-or-later. Hardware design files: CERN-OHL-S-2.0.
