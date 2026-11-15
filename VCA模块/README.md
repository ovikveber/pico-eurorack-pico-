# VCA模块

双路模拟 VCA（Dual VCA）：**2× 独立 VCA**，基于 **1× LM13700M-TD**（双 OTA），
纯模拟、**不使用 Pico**。每路 1 个 Audio In、1 个 CV In、1 个 Audio Out、**1 个 Level 旋钮**
和 **1 个 CV LED**。**CV 口不插线就没有声音**（和 Benjiao MiniVCA 一致）。

> **状态：设计决定（原理图前，2026-11-10）。** 方案、器件、控制和面板方向已定，
> 原理图/PCB 未开始。设计细节、计算和待办见 [`设计决定日志.md`](设计决定日志.md)，
> 物料见 [`BOM-物料清单.txt`](BOM-物料清单.txt)。
> 按仓库约定，EDA 文件（原理图/PCB/工程/库）需作者确认后再动，AI 只给器件清单、参数计算和设计建议。

## 功能

- 2× 独立 VCA（1× LM13700M-TD，双 OTA）
- 每路：Audio In / CV In / Audio Out
- 每路：**1× B10k Level 旋钮**（CV 衰减器，0~100%）
- 每路：**1 个 CV LED**（指示 CV 有无/大小）
- 控制：`增益 = Level × CV / 10`（线性，CV 钳位 0~10V）
- **没插 CV = 无声**（不做 +10V 常闭、不做手动 offset）
- 无音频 LED、无第二 CV、无指数、无固件、无 MCU
- ±12V 供电，16pin 总线
- 目标面板宽度：**6HP（紧）~ 8HP（稳）**

## 来源

- 原始电路：**Juanito Moore** 的 LM13700 VCA 设计
- 单路实现：**Benjiao Modular MiniVCA**（MIT）
  - https://benjiaomodular.com/post/2021-12-17-lm13700-vca/
  - https://github.com/benjiaomodular/MiniVCA
- 资料包/优化版：**小蔡同学（Ciiyte）** 整理的 Gerber/KiCad（电路与 Benjiao v1.1 相同）

本模块在 Benjiao 单路基础上做如下改动：做成 2 路（一颗 LM13700 的两半）、
每路只保留 1 个 CV 输入、把 B100k 换成 B10k、增加 1 个 CV LED、去掉音频 LED。
**没有 +10V 常闭**，行为和 Benjiao 一样：CV 不插就没声。

## 架构

| 部分 | 器件 | 说明 |
| --- | --- | --- |
| VCA 核心 | 1× LM13700M-TD | 双 OTA = 2 路 |
| 运放 | 1× TL074（或 TL084） | 每路 2 个运放：输入/求和 + I/V；2 路共 4 个 |
| 电流源 | 2× 2N3906 | 每路 1 只 PNP，驱动 Iabc |
| CV LED | 2× 2N3904 + 2× LED | 每路 1 组：信号→100k→2N3904→LED |
| 保护 | 1k + BAT54S | 所有插孔钳位到 GND/+12V |
| 电源 | ±12V（16pin 总线） | 无 +5V / +3V3 / 基准 |

## 电平

| 信号 | 电平 |
| --- | --- |
| 音频输入 | 10Vpp（±5V） |
| 音频输出 | 10Vpp（±5V），1k 串阻 |
| CV 输入 | 0~+10V（钳位，负 CV 当作 0） |
| Level | 0~100%（B10k 线性，CV 衰减） |
| 电源 | +12V / −12V（16pin 总线） |

## 控制说明

```
增益 = Level × (CV / 10V)      // 线性；CV 钳位 0~10V
没插 CV → 增益 = 0 → 无声
```

- **Level 大旋钮**：CV 的衰减器；不插 CV 时旋钮不起作用（没有 offset）。
- **CV 插口**：普通插座即可，不需要开关脚。
- **CV LED**：跟 CV 大小亮/灭；CV 是音频速率时会闪，慢速包络/LFO 时跟随。
- 不做反相（Level 0~100%，负 CV 当作 0）。
- 线性：`增益 ∝ CV`，包络直接控制时音量形状=包络形状。

## 信号链与器件作用

```
音频：Jack → 1k(保护) → 100k/470r(衰减到~47mVpp) → 470r → OTA
      → 56k(I/V) → 1k(输出串阻/保护) → Audio Out

CV  ：Jack → 1k(保护) → B10k(Level) → 22k → 控制运放
      → 2N3906(电流源) → OTA Iabc

CV LED：CV 节点 → 100k → 2N3904 → LED → 1k → +12V
```

| 器件 | 作用 | 不要它会怎样 |
| --- | --- | --- |
| 输入/输出 1k | Eurorack 保护串阻 | 插错/短路时容易损坏 |
| 100k + 470r | 把 10Vpp 音频衰减到约 47mVpp | OTA 输入过载，严重失真 |
| 470r（R5） | OTA 输入串阻 | 影响很小，可保留 |
| 56k | OTA 电流→电压（I/V） | 没有输出信号 |
| 1k（R10） | 输出串阻 | 输出保护/限流变差 |
| 2N3906 ×2 | CV→Iabc 电流源，控制增益 | VCA 关不死/控制不准 |
| 2N3904 ×2 | CV LED 驱动 | 只是 LED 不亮，可整组省掉 |

> **输入/输出阻抗**：音频输入 ≈ **101k**（1k + 100k），CV 输入 ≈ **11k**（1k + B10k；换 B100k 则 ≈101k），输出 ≈ **1k**。
> 1k 只是保护串阻，**不是**输入阻抗。
> 音频路径里**没有三极管**；三极管只在 CV 控制和 LED 上。

## 接线表（初稿，待原理图定稿）

### 每路模拟侧

| 部分 | 接线 |
| --- | --- |
| Audio In | Jack tip → 1k + BAT54S → R6 100k → R7 470r → OTA |
| CV In | Jack tip → 1k + BAT54S → Level 电位器上端 |
| Level | B10k：上端 CV、下端 GND、动片 → R3 22k → 求和运放虚地 |
| 电流源 | 2N3906 + R8 22k + R9 56k，驱动 OTA Iabc |
| I/V | OTA 输出 → 运放反相 I/V（56k / 120pF）→ R10 1k → Audio Out |
| CV LED | CV 节点 → 100k → NPN 基极；发射极 GND；集电极 → LED → 1k → +12V |
| 保护 | 所有插孔 1k + BAT54S（pin1=GND、pin2=+12V、pin3=信号） |

### 电源

| 网络 | 接线 |
| --- | --- |
| 16pin | pin 1/2=−12V，3–8=GND，9/10=+12V，11/12=+5V，13/14=CV，15/16=Gate；本模块只用 ±12V/GND |
| 方向 | 红条 = −12V；牛角/排线方向必须确认 |
| ±12V | 防反接 + TVS；100µF 储能；每颗 IC 电源脚 100nF 退耦 |

## 面板（6~8HP）

```
 通道 1      通道 2
[ IN1 ]     [ IN2 ]
[ CV1 ]     [ CV2 ]
[ OUT1]     [ OUT2]
[  K1 ]     [  K2 ]
 (LED1)      (LED2)
```

- 2 列，每列：3 个插孔 + 1 旋钮 + 1 LED，竖排，3U 高度放得下。
- 3U 可用高度 128.5mm；1HP = 5.08mm。
- **6HP**：标称 30.48mm，实际做约 **30.1mm**；**8HP**：标称 40.64mm，实际约 **40.3mm**（都比标称小 0.3~0.4mm）。
- ≤10HP 用上下 **2 个 M3 孔**，孔距为 5.08mm 的整数倍。
- 普通 3.5mm 插座即可（不需要开关脚）。

## Eurorack 符合性

| 项目 | 标准 | 本模块 |
| --- | --- | --- |
| 音频电平 | 10Vpp（±5V） | 10Vpp 设计，最大约 0.93× |
| CV 电平 | 0~10V 常见 | 0~10V，钳位 |
| 音频输入阻抗 | 100k（de facto） | ≈101k |
| CV 输入阻抗 | 100k（de facto） | ≈11k（B10k）/ 101k（B100k） |
| 输出阻抗 | 1k（de facto） | 1k |
| 电源 | ±12V，16pin | ✔ |
| 面板 | 3U，1HP=5.08mm | 6~8HP |

> 唯一不严格符合的是 CV 输入阻抗：B10k 时约 11k；要 100k 就换 B100k。

## 固件

无。纯模拟模块，不需要固件。

## 文件

- [`设计决定日志.md`](设计决定日志.md) — 需求、架构、计算、待办
- [`BOM-物料清单.txt`](BOM-物料清单.txt) — 物料清单

## 待办

- [x] 确定架构：纯模拟、1× LM13700、2 路
- [x] 确定每路控制：1× Level 旋钮 + CV，无 CV 即无声
- [x] 确定 LED：每路只给 CV LED
- [x] 去掉 +10V 基准/常闭（和 Benjiao 一致）
- [ ] 原理图：输入/求和/I-V、2N3906 电流源、CV LED
- [ ] 去掉 CV_2 后，确认求和节点处理（接 GND）
- [ ] 面板定稿：6HP vs 8HP
- [ ] PCB 布局：OTA 输入走线短、地平面、2 路对称
- [ ] 打样 + 实测：增益范围、失真、噪声、串扰、CV LED 亮度

## 协议与致谢

- 原始设计：Juanito Moore；单路实现：Benjiao Modular（MIT）；资料整理：小蔡（Ciiyte）
- 本模块改动与文档：随本仓库，硬件设计文件 CERN-OHL-S-2.0
- 保留原作者出处与许可

---

## English summary

A **dual analog VCA** for Eurorack built around **1× LM13700M-TD** dual OTA, pure analog,
no microcontroller. Based on Benjiao Modular's MiniVCA (MIT), itself based on Juanito Moore's
LM13700 VCA. Each channel has an audio input, a CV input, an audio output, one B10k Level knob
(CV attenuator) and one CV LED: `gain = Level × CV/10` (linear, 0–10V). **No CV patched = silence**,
same as Benjiao. No audio LEDs, no exponential, no second CV, no +10V normal. ±12V, 6–8HP.
Status: design decisions only; schematic/PCB not started.
