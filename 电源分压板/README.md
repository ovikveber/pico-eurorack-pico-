# 电源分压板（Eurorack power distribution）

Eurorack 电源分配板：从 PSU 的 16pin 输入 **+12V / −12V / +5V / GND**，并联出
**9×16P + 9×10P = 18 路** 输出。入口做反接保护、TVS 和三轨去耦，每轨一个 LED 指示。
纯无源板，无固件。

> **状态：可用（2026-10-30 完成原理图 + PCB，DRC 通过）。**
> 板框 284.5 × 35.0 mm ≈ **56HP**。设计细节见 [`项目日志.txt`](项目日志.txt)，
> 物料见 [`BOM-物料清单.txt`](BOM-物料清单.txt)。

## 功能

- 单个 16P 牛角座输入（原计划的第二路 10P 输入已删除）
- 输出：**9×16P + 9×10P**（CN2–CN10 / CN17–CN25），全部并联到受保护母线
- 不做每路保险/PTC：一路短路会拉低该轨，靠 PSU 限流兜底（有意取舍）
- CV/Gate 总线透传（13/14 = CV，15/16 = Gate）
- 三条母线各一套去耦：220µF / 100µF 固态 / 22µF / 10µF / 1µF CBB / 100nF CBB
- 每轨一个 LED：通电亮，母线被拉低/掉电时灭

## 保护

| 轨 | 方案 |
| --- | --- |
| +12V | `SS54` 串联（阳极进/阴极出）+ `SMBJ15CA` 到 GND |
| −12V | `SS54` 串联（方向相反）+ `SMBJ15CA` 到 GND |
| +5V | `AO3401A` P-MOS 防反接（D 进 / S 出 / G 经 100k 到 GND）+ `BZX84C10` 栅源钳位 + `SMBJ5.0CA` 到 GND |

- −12V 电解电容极性反装（+ 接 GND，− 接 −12V）。
- 保护只做在入口，不做每路独立保护。

## PCB

- 2 层（Top / Bottom），双面 GND 铺铜
- 板框 284.5 × 35.0 mm ≈ 56HP
- 电源母线 1.8–2.0 mm，CV/Gate 0.3 mm，LED 支路 0.7 mm（规则最小线宽 0.2 mm）
- 输入座 + 保护 + 去耦集中在左侧，输出插座向右排列；元件全在 Top
- 无安装孔（待补）

## 待办 / 建议

- [ ] 加安装孔（4× M3 或定位柱）
- [ ] 去耦沿插座分布几颗 1µF/100nF，改善最右端瞬态
- [ ] R1/R2 换 0805 或改 2.2k，降低发热
- [ ] 确认牛角座方向（红边 −12V 朝下），避免带框座反插
- [ ] 核对 −12V 电解电容封装 pin1 是否为正极
- [ ] 确认最终板宽（现 56HP）

## 协议

- 硬件设计文件：CERN-OHL-S-2.0
- 无固件

---

## English summary

A passive Eurorack power distribution board: one 16-pin input (+12V / −12V / +5V / GND)
and **9×16P + 9×10P = 18 outlets**. Input-side reverse-polarity protection (SS54 for ±12V,
AO3401A P-MOS for +5V), TVS on all rails, and six-cap decoupling per rail with a status LED.
2-layer PCB, 284.5 × 35.0 mm ≈ 56HP, DRC pass. Status: usable / done.
