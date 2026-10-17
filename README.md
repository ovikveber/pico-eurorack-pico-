<a id="chinese"></a>
# pico-eurorack 合成器及模块

[中文](#chinese) | [English](#english)

基于 RP2040 / RP2350 的开源 Eurorack 合成器项目。每个项目都包含固件、硬件设计文件和
搭建、改装所需的资料。

## 项目

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| [`midi to cv`](midi%20to%20cv/README.md) | 可用 | RP2040 USB-MIDI 转 8 路 CV/Gate，DAC8568，编码器菜单，1V/oct |
| [`pico303`（OV-03开源文件）](OV-03开源文件/README.md) | 可用 | RP2350 上的 TB-303 电路级复刻，16 步音序器、LCD 界面、USB MIDI 和 CV/Gate |

`pico303` 只支持 RP2350（实测 RP2040 无法运行）；原版 303 复刻的电路、外壳和仿真资料
不在这个仓库里。

## 目录结构

```
midi to cv/
  firmware/midi2cv/midi2cv.ino   固件
  hardware/                      BOM、Gerber、板子照片
  docs/                          接线表、项目日志
  README.md

OV-03开源文件/
  固件/                          最新 UF2
  源码/                          pico_tb303.ino + 4 个依赖文件、303seq.cs、303paint.cs、OV-03.iss
  硬件/                          接线表、V1.2 EDA、初版 Gerber 和面板
  物料/                          BOM，以及主控、屏、插座、DAC 的资料
  软件/                          Windows 安装包（体积超限，未入库，见项目 README）
  皮肤/ 音序/ 素材/              皮肤、示例音序、面板素材
  参考资料/                      303 相关资料（版权归原作者，仅供学习）
  README.md

README.md
CONTRIBUTING-贡献指南.md
LICENSE
.gitignore
```

## 编译

1. 安装 [Arduino IDE 2.x](https://www.arduino.cc/en/software) 和
   [Raspberry Pi Pico / RP2040 / RP2350 核心](https://github.com/earlephilhower/arduino-pico)。
2. 安装各项目 README 里列出的库（Adafruit TinyUSB、Adafruit NeoPixel）。
3. 打开 sketch，选择对应板子，USB Stack 选 **TinyUSB**。
4. 编译上传；或者按住 BOOTSEL 刷入 `固件/` 里的 UF2。

`midi to cv` 的 FQBN 示例：

```
rp2040:rp2040:rpipico:usbstack=tinyusb
```

## AI 说明

部分固件和文档由 **DeepSeek V4 Flash**（AI）协助编写，所有改动都由作者在真实硬件上
测试和维护。AI 协助的代码沿用项目本身的协议。

## 贡献与改装

欢迎 fork 和提交 PR，详见 [CONTRIBUTING-贡献指南.md](CONTRIBUTING-贡献指南.md)。

- 改动尽量放在对应的项目文件夹里
- 接线、引脚变化写进项目 README
- 第三方代码和参考资料保留原协议和出处
- 大改动先开 issue 讨论方向

## 协议

- 固件和软件：[MIT](LICENSE)
- 硬件设计文件：CERN-OHL-P v2
- 第三方库和参考资料保留各自协议

`TB-303` 是 Roland 的商标。`pico303` 是独立复刻，仅供学习和 DIY，与 Roland 无关。

---

<a id="english"></a>
# pico-eurorack synth modules

[中文](#chinese) | [English](#english)

An open-source Eurorack synth project built around RP2040 / RP2350. Each project contains
the firmware, hardware design files and the documentation needed to build and modify it.

## Projects

| Project | Status | Description |
| --- | --- | --- |
| [`midi to cv`](midi%20to%20cv/README.md) | working | RP2040 USB-MIDI to 8x CV/Gate, DAC8568, encoder menu, 1V/oct |
| [`pico303` (OV-03 open-source files)](OV-03开源文件/README.md) | working | TB-303 circuit-level clone on RP2350, 16-step sequencer, LCD UI, USB MIDI and CV/Gate |

`pico303` only supports RP2350 (RP2040 was tested and does not run). The circuit, case and
simulation files of the original 303 replica are not part of this repository.

## Repository layout

```
midi to cv/
  firmware/midi2cv/midi2cv.ino   firmware
  hardware/                      BOM, Gerber archive, board photos
  docs/                          wiring table, project log
  README.md

OV-03开源文件/                   (OV-03 open-source files)
  固件/                          latest UF2
  源码/                          pico_tb303.ino + 4 dependencies, 303seq.cs, 303paint.cs, OV-03.iss
  硬件/                          wiring table, V1.2 EDA, initial Gerber and panel
  物料/                          BOM, MCU / display / jack / DAC documentation
  软件/                          Windows installer (too large, not committed; see project README)
  皮肤/ 音序/ 素材/              skins, example patterns, panel assets
  参考资料/                      303 reference material (copyright belongs to the authors, study only)
  README.md

README.md
CONTRIBUTING-贡献指南.md
LICENSE
.gitignore
```

## Building

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software) and the
   [Raspberry Pi Pico / RP2040 / RP2350 core](https://github.com/earlephilhower/arduino-pico).
2. Install the libraries listed in each project README (Adafruit TinyUSB, Adafruit NeoPixel).
3. Open the sketch, select the matching board and set the USB stack to **TinyUSB**.
4. Compile and upload, or hold BOOTSEL and flash the UF2 from `固件/`.

FQBN example for `midi to cv`:

```
rp2040:rp2040:rpipico:usbstack=tinyusb
```

## AI assistance

Parts of the firmware and documentation were written with the help of **DeepSeek V4 Flash**
(AI). All changes are reviewed, tested on real hardware and maintained by the author.
AI-assisted code follows the license of the project it belongs to.

## Contributing / mods

Forks and pull requests are welcome. See [CONTRIBUTING-贡献指南.md](CONTRIBUTING-贡献指南.md).

- keep changes inside the matching project folder
- document wiring and pin changes in the project README
- keep third-party code and reference documents with their original licenses
- open an issue before large changes so we can agree on the direction

## License

- Firmware and software: [MIT](LICENSE)
- Hardware design files: CERN-OHL-P v2
- Third-party libraries and reference documents keep their own licenses

`TB-303` is a trademark of Roland. `pico303` is an independent clone for study and DIY use,
not affiliated with or endorsed by Roland.
