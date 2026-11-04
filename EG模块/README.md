# EG模块

多路包络发生器：**2× ADSR + 2× AR + 4× Decay**，每一路都有独立的 **Gate** 和
**Retrigger** 输入，8 路 **0~10V** CV 输出。基于 Raspberry Pi Pico 1（RP2040）。

> **状态：原理图 + PCB 已全板复核（2026-11-04，对照数据手册），未打样。**
> 电源/输入/输出/模拟控制、Pico 数字侧（SPI/选择线）均已接好；PCB 已布线并通过 DRC
> （位号提示除外）。方案、BOM 已定；剩余为少量可选项与固件。
> 设计细节和待办见 [`项目日志.txt`](项目日志.txt)，物料见 [`BOM-物料清单.txt`](BOM-物料清单.txt)。

## 功能

- 2× ADSR（A / D / S / R）
- 2× AR（A / R）
- 4× Decay（单 Decay）
- 每一路都有独立的 Gate 输入和 Retrigger 输入（全部带重触发）
- 8× **0~10V** CV 输出（VCV/Eurorack 单极性 CV 标准）
- 8× Gate + 8× Retrigger 输入（0/10V，带保护）
- 8 个推子（ADSR）+ 8 个旋钮（AR / Decay），全部 **10k 线性**；
  指数/对数曲线由固件生成
- 24 个 LED：8 个输出 + 8 个 Gate + 8 个 Retrig（TLC5947，12 位 PWM）
- 目标面板宽度：**20~24HP**（20HP 偏紧，24HP 更舒服）

## 架构

| 模块 | 器件 | 说明 |
| --- | --- | --- |
| 主控 | Raspberry Pi Pico 1（RP2040） | 26 个 GPIO、3 路 ADC、2 个 SPI |
| CV 输出 | DAC8568 | 8 通道、16 位、SPI、内部 2.5V 基准（0~5V），运放 ×2 → 0~10V |
| Gate / Retrig 输入 | 2× ADS8688 | 8+8 通道、16 位、±10.24V、1MΩ、±20V OVP |
| 模拟控制 | CD74HC4067 | 16 通道 mux → Pico ADC；8 推子 + 8 旋钮 |
| LED | TLC5947 | 24 通道、12 位 PWM、恒流 |

本模块**不含 VCA**，只做纯 CV/包络；音频 VCA 留给独立模块。

## 电平

| 信号 | 电平 |
| --- | --- |
| 包络输出 | 0 ~ +10V |
| Gate / Retrig 输入 | 0 / +10V（固件阈值 + 迟滞） |
| 模拟控制 | 0~3.3V，10k 线性 |
| 电源 | +12V / −12V / +5V（均取自 16pin 总线；+5V 由电源模块提供，板上不再生成） |

输出串 1k。Gate/Retrig 输入每路 1k 串阻 + BAT54S 钳到 **GND/+12V**；ADS8688 内部
±20V 保护作为第二道防线。

## 接线表（已定稿，2026-11-04）

### Pico 数字侧

| 功能 | Pico 引脚 | 外设引脚 |
| --- | --- | --- |
| TLC5947 SIN / SCLK / BLANK / XLAT | GP0 / GP1 / GP2 / GP3 | 4 / 3 / 2 / 30 |
| ADCB（B = Retrig）SDI / CS# / SCLK / SDO | GP6 / GP7 / GP8 / GP9 | 1 / 38 / 37 / 36 |
| ADCA（A = Gate）SDI / CS# / SCLK / SDO | GP10 / GP11 / GP12 / GP13 | 1 / 38 / 37 / 36 |
| DAC8568 LDAC# / SYNC# / SCLK / DIN | GP17 / GP18 / GP19 / GP20 | 1 / 2 / 16 / 15 |
| CD74HC4067 S0 / S1 / S2 / S3 | GP26 / GP27 / GP21 / GP22 | 10 / 11 / 14 / 13 |
| CD74HC4067 COMMON | GP28（ADC2，经 R51 1k） | 1 |
| 备用 GPIO | GP4 / GP5 / GP14 / GP15 / GP16 | — |
| ADC_VREF（pin35） | C45 100nF 到 GND | — |

所有 SPI/类 SPI 接口统一走 PIO（4 个状态机：DAC、ADCA、ADCB、TLC）。
四组总线各自是连续 4 脚，便于 PIO 的 side-set/out 引脚组：
PIO0 = DAC（GP17–20）+ ADCA（GP10–13），PIO1 = ADCB（GP6–9）+ TLC（GP0–3）。
CS/XLAT/BLANK/LDAC 用普通 GPIO。共 21 根（16 SPI + mux 5）。

> 注意：DAC 引脚相对旧草案已改（旧为 GP2/3/5/6），固件请以本表为准。

### 模拟输入 / 输出

| 部分 | 接线 |
| --- | --- |
| CV 输出 | DAC VOUTA–H → R20/24/…/48(1k) + 100nF 到 GND → 运放同相 ×2（Rf=Rg=10k）→ R21/25/…/49(1k) → OUTA–H → J17–J24 tip |
| 输出保护 | 每路 OUTx：BAT54S pin1=GND、pin2=+12V、pin3=信号 |
| Gate/Retrig 输入 | J1–J16 tip → R1–R16(1k) → A0P…B7P → ADCA/ADCB；AIN_xGND=GND |
| 输入保护 | 每路 A/B：BAT54S pin1=GND、pin2=+12V、pin3=信号（0~10V 不导通） |
| 模拟控制 | 8 推子 → I0–I7；AR 旋钮 → I8–I11；Decay 旋钮 → I12–I15；KNOBBUS COMMON→R51→GP28 |
| 电位器 | 10k 线性；两端接 +3V3/GND，动片接 Ix。**推子：作者已实物确认当前接法正确（pin1=+3V3、pin2=GND、pin3→Ix），勿按“pin2=动片”的惯例去改**；旋钮 RK09K：pin1=GND、pin2=动片→Ix、pin3=+3V3 |
| LED | TLC5947 OUT0–23 → LED1–24 阴极（K）；所有 LED 阳极（A）= +5VLED；+5V→R52(0Ω)→+5VLED |

### 电源

| 网络 | 接线 |
| --- | --- |
| 16pin | 1/2=−12VIN，3–8=GND，9/10=+12VIN，11/12=+5VIN，13/14=CV，15/16=Gate |
| ±12V | +12VIN→D17(SS54)→+12V；−12VIN→D19(SS54)→−12V；D18/D20=SMBJ15CA 到 GND；100µF 储能 |
| +5V | +5VIN→Q1(AO3401A, D→IN/S→OUT/G→100k→GND)+BZX84C10→+5V；D21=SMBJ5.0CA；100µF 储能 |
| 3V3 | Pico 3V3(OUT) → +3V3（ADS DVDD、TLC VCC、KNOBBUS VCC、电位器） |
| 参考 | DAC VREF=10µF；ADS REFIO=10µF、REFCAP=10µF+100nF；ADC_VREF=C45 100nF |

## 固件计划

- Core 0：5~10kHz 定时器 → 8 路包络 → DAC8568 → LDAC 同时锁存
- Core 1：1~5kHz → ADS8688 Gate/Retrig 轮询 → 每 0.2ms 扫一个 mux 通道
- PIO：DAC/ADCA/ADCB/TLC 各 1 个 SM（共 4 个，PIO0 放 DAC+ADCA，PIO1 放 ADCB+TLC）
- 参数用双缓冲/无锁结构传给 Core 0
- 中断里不 delay，不阻塞式扫描；DAC 用 LDAC 同步更新
- 包络引擎放 RAM（`__not_in_flash_func`）；编程完成后禁用 USB 中断
- 时间常数不需要平滑；Sustain/Level 加一阶低通防 zipper
- 上电先把 DAC 全写 0，LED 全灭，避免“噗”

## 文件

- [`BOM-物料清单.txt`](BOM-物料清单.txt) — 物料清单（纯文本）
- [`项目日志.txt`](项目日志.txt) — 开发日志（纯文本）

## 待办

- [x] 原理图主体（电源/输入/输出/模拟控制）
- [x] Pico 数字侧接线（GPIO 分配见上表，GP28 经 R51 接 knobbus）
- [x] ADC_VREF（pin35）加 100nF 到 GND
- [x] 全板复核（原理图 + PCB，对照数据手册，见日志第十五节）
- [ ] 确认板宽：现板框 213mm≈42HP，文档写 20~24HP（需统一）
- [ ] 最终面板宽度/推子行程（20HP vs 24HP）
- [ ] PCB 布局（8 层：顶/GND/LED/GND/运放/3V3/5V/底；模拟与数字分区）
- [ ] 可选优化：ADS8688 REFCAP C3/C17→1µF；mux 输出加 10nF；TLC BLANK 加 10k 上拉；确认输出插座开关脚是否接地
- [ ] 固件：ADS8688 驱动、TLC5947 驱动、8 路包络引擎、参数映射、PIO 驱动
- [ ] 校准：DAC 0~10V、LED gamma、ADS8688 阈值
- [ ] 打样与测试计划
- [ ] 已决定：不装按钮

## 协议

- 固件和软件：GPL-3.0-or-later
- 硬件设计文件：CERN-OHL-S-2.0

---

## English summary

A multi-envelope generator for Eurorack: **2× ADSR + 2× AR + 4× Decay**, each with its own
Gate and Retrigger input, and 8× **0–10V** CV outputs. Based on a Raspberry Pi Pico 1
(RP2040), with a DAC8568 (8-ch 16-bit SPI DAC), 2× ADS8688 (8+8 gate/retrig inputs),
a CD74HC4067 (16 analog controls: 8 sliders + 8 knobs) and a TLC5947 (24 LEDs).
No VCA on board; this is a pure CV/envelope module. Status: design draft, not built.
