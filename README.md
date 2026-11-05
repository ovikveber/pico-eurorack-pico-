<a id="chinese"></a>
# pico-eurorack 合成器及模块

[中文](#chinese) | [English](#english)

基于 RP2040 / RP2350 的开源 Eurorack 合成器项目。每个项目都包含固件、硬件设计文件和
搭建、改装所需的资料。

## 项目

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| [`midi to cv`](midi%20to%20cv/README.md) | 不可用（改造中） | RP2040 USB-MIDI 转 8 路 CV/Gate，DAC8568，1V/oct；组 1–4、屏幕和编码器 UI 已验证。当前输出只有 0–5V，**不满足 10Vpp**，正在加运放改输出级（CV ±5V / Gate 0–10V）；组 5–8、私有总线、校准待做 |
| [`pico303`（OV-03开源文件）](OV-03开源文件/README.md) | 可用 | RP2350 上的 TB-303 电路级复刻，16 步音序器、LCD 界面、USB MIDI 和 CV/Gate |
| [`电源模块`](电源模块/README.md) | 可用 | 18V 输入，三颗 TPS5430 + LM2940/LM2990 出 +12/−12/+5V，每路 1A；已打样并实机调试，±12V/+5V 正常。LM2990 为假货，现用 7912 临时替代；D7 需反装；待换正品/改板 |
| [`电源分压板`](电源分压板/README.md) | 可用 | Eurorack 电源分配板，9×16P + 9×10P（18 路），入口保护与去耦，56HP；已完成 |
| [`EG模块`](EG模块/README.md) | 板子在途 | 2× ADSR + 2× AR + 4× Decay 包络发生器，8× 0~10V CV，Gate/Retrig 输入；原理图+PCB 复核 DRC 通过，已打样，等待到板；待固件 |
| [`OV-JP8000`](OV-JP8000/README.md) | 可用 | 基于 OV-303 硬件的 JP-8000 风格 SuperSaw 合成器，8 声部、双 ADSR，当前 v37 |
| [`可编程VCO`](可编程vco模块/README.md) | 设计中 | 8 声部 SuperSaw + Audio/CV 调制矩阵，方案冻结，未画板 |

`pico303` 只支持 RP2350（实测 RP2040 无法运行）；原版 303 复刻的电路、外壳和仿真资料
不在这个仓库里。

## 目录结构

```
midi to cv/
  firmware/midi2cv/midi2cv.ino   固件
  firmware/                      测试固件、当前 UF2
  hardware/                      BOM、Gerber、板子照片
  docs/                          接线表、项目日志
  vcv-rack/                      VCV Rack 插件
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

OV-JP8000/
  pico_jp8000/                   固件源码、屏幕驱动、字库
  firmware/                      最新 UF2
  参考资料/                      JP-8000 服务手册
  OV-JP8000日志.md               开发日志（主要文档）
  README.md

EG模块/
  README.md                      功能、电平、引脚、方案说明
  BOM-物料清单.txt               BOM
  项目日志.txt                   设计决策、待办

电源模块/
  项目日志.md                    设计决策、计算、待办（当前主要文档）
  README.md

电源分压板/
  项目日志.txt                   设计、保护、PCB 说明
  BOM-物料清单.txt               BOM
  README.md

可编程vco模块/
  设计决定日志.md                需求、架构、矩阵、待办（主要文档）
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

- 固件和软件：[GPL-3.0-or-later](LICENSE)
- 硬件设计文件：CERN-OHL-S-2.0（见 [CERN-OHL-S-2.0.txt](CERN-OHL-S-2.0.txt)）
- 第三方库和参考资料保留各自协议

早期版本曾以 MIT 发布，已经分发出去的旧版本仍可按 MIT 使用；从当前版本起改为
GPL-3.0-or-later。

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
| [`midi to cv`](midi%20to%20cv/README.md) | not usable (being modified) | RP2040 USB-MIDI to 8x CV/Gate, DAC8568, 1V/oct; groups 1–4 and the display/encoder UI are verified. The current output is 0–5 V, **not 10 Vpp**, so op-amps are being added for the VCV-standard output (CV ±5V / Gate 0–10V). Groups 5–8, private bus and calibration are pending |
| [`pico303` (OV-03 open-source files)](OV-03开源文件/README.md) | working | TB-303 circuit-level clone on RP2350, 16-step sequencer, LCD UI, USB MIDI and CV/Gate |
| [`power supply`](电源模块/README.md) | working | 18V input, three TPS5430 pre-regulators + LM2940/LM2990 regulators (+12/−12/+5V, 1A each); built and debugged, ±12V/+5V work. The LM2990 was counterfeit and is temporarily replaced by a 7912; D7 must be reversed; genuine part / board revision pending |
| [`power distribution`](电源分压板/README.md) | working | Eurorack power distribution board, 9×16P + 9×10P (18 outlets), input protection and decoupling, 56HP; done |
| [`EG module`](EG模块/README.md) | board in transit | 2× ADSR + 2× AR + 4× Decay envelope generator, 8× 0–10 V CV outputs, gate/retrig inputs; schematic/PCB reviewed with DRC pass, board ordered and in transit; firmware pending |
| [`OV-JP8000`](OV-JP8000/README.md) | working | JP-8000-style SuperSaw synth on OV-303 hardware, 8 voices, dual ADSR, currently v37 |
| [`programmable VCO`](可编程vco模块/README.md) | design | 8-voice SuperSaw + audio/CV modulation matrix, design frozen, PCB not started |

`pico303` only supports RP2350 (RP2040 was tested and does not run). The circuit, case and
simulation files of the original 303 replica are not part of this repository.

## Repository layout

```
midi to cv/
  firmware/midi2cv/midi2cv.ino   firmware
  firmware/                      test sketches, current UF2
  hardware/                      BOM, Gerber archive, board photos
  docs/                          wiring table, project log
  vcv-rack/                      VCV Rack plugin
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

OV-JP8000/
  pico_jp8000/                   firmware source, display driver, font
  firmware/                      latest UF2
  参考资料/                      JP-8000 service notes
  OV-JP8000日志.md               development log (main document)
  README.md

EG模块/                          (EG module)
  README.md                      features, levels, pinout, design notes
  BOM-物料清单.txt               bill of materials
  项目日志.txt                   design decisions and to-do

电源模块/                        (power supply)
  项目日志.md                    design decisions, calculations, to-do (main document)
  README.md

电源分压板/                      (power distribution)
  项目日志.txt                   design, protection, PCB notes
  BOM-物料清单.txt               bill of materials
  README.md

可编程vco模块/                   (programmable VCO)
  设计决定日志.md                requirements, architecture, matrix, to-do (main document)
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

- Firmware and software: [GPL-3.0-or-later](LICENSE)
- Hardware design files: CERN-OHL-S-2.0 (see [CERN-OHL-S-2.0.txt](CERN-OHL-S-2.0.txt))
- Third-party libraries and reference documents keep their own licenses

Earlier versions were published under MIT. Copies distributed under MIT keep those rights;
from the current version onward the project is GPL-3.0-or-later.

`TB-303` is a trademark of Roland. `pico303` is an independent clone for study and DIY use,
not affiliated with or endorsed by Roland.








