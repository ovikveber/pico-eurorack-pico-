# MIDI-to-CV 项目日志（USB-MIDI → 8×CV/Gate + 私有总线）

> ★ 本项目原代号 **OV-MIDI**，现统一叫 **midi-to-cv**（属"纯数字合成器"系列）。
> 目标：**Pico + DAC8568** 做一块 **12HP、8 复音**的 USB-MIDI → CV/Gate 模块，
> 并带一条**私有数字总线**把复音/音符数据发给系列里的复音 VCO。

## 0. 路径速查
- **PCB 已到货（2026-10-22）；左半 4 组 CV/Gate 已焊并验证；屏幕/编码器 UI 已完成。**
- 屏幕/编码器独立测试：`firmware\screen_test\screen_test.ino`
  → `firmware\screen_test_20261022_encoder.uf2`（历史测试；UI 已合并进主固件）
  - PIO SPI + DMA；编码器 A=GP13 B=GP12 SW=GP11（15 脉冲 / 30 定位）
  - 早期问题：逐格 `Serial.print` 会堵主循环；阈值 4 要转两格；已修正为阈值 2
- **超频实测（`firmware\oc_test`，RAM 校准 + stress，不依赖 `clock_get_hz`）**：
  - 125 / 400 / 420 / 432MHz 稳定（420 实测 421，432 实测 433）
  - 436MHz 会复位；440 / 448 / 450MHz 挂死
  - **500MHz 是假象**：`clock_get_hz` 报 500，但 `CLK_SYS_CTRL` 仍在 PLL_USB 48MHz
    （510/500/480 实测 47MHz；520 落在 ROSC ~7MHz）
  - 结论：选 **420MHz**（1.3V，boot2 /4 → flash 105MHz，安全），失败回退 125MHz
  - oc_test 里看门狗必须在超频前启用，否则 450 这类挂死不会自动恢复
- **屏幕最终频率**：
  - 主固件 **420MHz CPU + 52.5MHz SCLK**（420 ÷ 2 ÷ **4.0**，整数分频）
  - SCK/MOSI 用 **12mA + fast slew**；52.5MHz 下颜色/刷新稳定
  - 62.5MHz 在 420 下是 3.36 小数分频，动态刷新会间歇红屏/花屏，已放弃
  - 70MHz 起偏红（PIO 140MHz+）；84/105MHz 颜色错误（PIO 168/210MHz），
    属于数据位错误，不是 RGB 顺序，无法用补色修好
- 已编译并烧录主固件：`firmware\midi2cv_20261022_ui.uf2`
  （16MB + boot2 /4 + tinyusb；清理掉所有串口调试/计时/BRINGUP/超频阶梯）
  - 枚举为 **OV-MIDI**（VID 239A/PID CAFE），另有 CDC 口 COM18 可做 1200bps touch
  - 组1..4 = MIDI ch1..4，当前只测左半 4 组
- 主固件：`midi to cv\midi2cv\midi2cv.ino`（8 通道 + 屏幕 UI）
- 接线表：`midi to cv\硬件\OV-MIDI接线表.txt`
- 物料：`midi to cv\物料\物料清单.txt`
- 编译 FQBN（16MB）：
  `rp2040:rp2040:generic:flash=16777216_0,boot2=boot2_w25q128jvxq_4_padded_checksum,freq=125,usbstack=tinyusb`
  - `$cli="D:\aruidno ide\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"`；`$env:ARDUINO_DIRECTORIES_DATA="D:\Arduino15"`
- 烧录：1200bps touch 进 BOOTSEL（出现 `RPI-RP2` 盘）→ 拖 `.uf2`。

## 1. 硬件（当前方案, 12HP / 8 复音）
- **Pico (RP2040)**。
- **DAC8568**（八路 16bit，内部 2.5V 基准 ×2 = 满量程 0–5V，上电复位到 0V，TSSOP-16）。可同封装同命令换 DAC8168/DAC7568/AD5668-2。
- **2× 74AHCT541**：U19 = SPI 3.3V→5V 电平转换；U20 = 8×Gate 缓冲。
- **EC11 旋转编码器**（带按键）；**1.3" ST7789 SPI OLED**（TFT1）。
- **17× 3.5mm 单声道座** = 8×CV + 8×Gate + 1× 私有数字总线。
- **电源**：从 Eurorack 16pin bus 只取 **+5V + GND**（CN1 = 2×8 简易牛角）；
  **+3V3 由 Pico 脚36 提供，只给 OLED**。去耦 C1–C5/C7(0.1µF) + C6(10µF)。
- **无 LED**（旧版的 LED 已删）。
- 引脚分配见 `硬件\OV-MIDI接线表.txt`（已更新到 8 复音版）。

## 2. DAC 关键点（DAC8568 / AD5668 族；已核对 ADI no-OS / bobhart 库 / Rust crate 测试向量）
- **主选 DAC8568（TI）**：八路 16bit，SPI，内部 2.5V 基准，满量程 0–5V，温漂 2ppm/°C，INL ±4 LSB，TSSOP-16。与 AD5668 同封装同命令。
- ⚠️ 购买注意：选**上电复位到 0V**的型号（避免 midscale）。
- **SPI 帧 = 32bit，MSB first**：byte1=`cmd`(低4位)；byte2=`(addr<<4)|(data>>12)`；byte3=`(data>>4)&0xFF`；byte4=`(data<<4)&0xF0`。
- **命令(4bit)**：0x0 写输入 / 0x1 更新 / 0x2 写+更新全部 / **0x3 写+更新本通道** / 0x4 上电掉电 / 0x5 清码 / 0x6 LDAC / **0x7 复位** / **0x8 内部基准设置(byte4=0x01 使能)**。
- **地址**：A..H = 0..7，All = 0xF。
- **上电流程**：`reset(0x7)` → `内部基准 ON(0x8,0x01)`。
- **电平转换不损精度**（只翻译数字线）；`CV_FULLSCALE_V=5.0` 精确（内部基准，无需标定）。

## 3. 固件 `midi2cv.ino`（现状：**8 通道 + 屏幕 UI**）
- Pico 作 **USB-MIDI 设备**（Adafruit TinyUSB），设备名 OV-MIDI，固定序列号 `OVMIDI0001`。
- **8 路单音（按优先+音符栈），每路 CV+Gate**，通道默认 V1=1…V8=8。
- **引脚**：DAC SCLK=GP28/DIN=GP27/SYNC=GP26；Gate=GP22,21,20,19,18,17,16,15；Enc A/B/SW=GP13/12/11。
- **每路映射**（真实，来自 PCB）：`gatePin[]={22,21,20,19,15,16,17,18}`，
  `dacCh[]={B,D,F,H,A,C,E,G}`（见"4.5"）。CV=1/2/5 V/oct（`NOTE0=24`=0V）；16bit=5.0V。
- 编码器 UI（单列，可滚动，行高 22px，行间 1px 分隔线）：
  - 导航：旋转选择行（1px 蓝框）；按下进入编辑（橙框）；再按退出并保存
  - **MIDI 模式（10 行）**：V1–V8 + GATE + V/OCT
    - 通道行：`Vn`、蓝色通道条、`CH n`、12px gate 指示灯
    - GATE 显示 **+ / -**；V/OCT 只支持 **1 / 2 / 5**（8V/oct 无法输出）
  - **CLOCK 模式（IO24 进入/退出）**：
    - 组1–4 的 **8 个插孔全部是 clock**：C1–C8
      - C1=组1 Gate，C2=组1 CV，C3=组2 Gate，C4=组2 CV，
        C5=组3 Gate，C6=组3 CV，C7=组4 Gate，C8=组4 CV
    - 组5–8 仍是 MIDI **V5–V8**
    - 单列列表：PLAY 在最顶，然后 C1–C8、V5–V8、BPM、GATE、V/OCT（共 16 行）
    - Clock 分频用 docB PwmClock 的 **24 个分数值**（16/1 … 1/128），
      主 tempo 为 **BPM 20–300**，周期 = 60/BPM × 分数拍数
    - **PLAY 行**：选中后按编码器启动/暂停；默认暂停，PLAY 灯灭
    - 第一次启动从相位 0 开始；暂停保留相位，再启动继续
    - USB MIDI Start/Stop/Continue 也可控制 clock（只在 clock 模式生效）
    - 无 RUN/RESET 硬件输出；组1 的两个插孔已改成 clock
  - **RANDOM 模式（IO23 进入/退出，移植 RandomSynth-VCV）**：
    - 全屏粒子场地（240×210）；底部左侧只留 KNOB 绑定，右侧 OUT 音名/GATE/TRIG 状态
    - 主页面编码器默认调 **场地大小**；按下进入设置页；设置页 BACK 返回
    - 连续参数显示统一为 0.00–1.00，内部最大/最小效果不变（球大小最大仍是 1.6×）
    - 设置行：BACK、SHAPE、FIELD、BALL、SPEED、WAVE、W.SPD、PITCH、ADSR、
      FX RATIO、RANGE、SCALE、V/OCT、GATE、TRIG、FX SEND、+BALL、+BOOST、RESET、KNOB
    - `+BALL` 行显示当前球数，`+BOOST` 行显示加速器数量，都是按下直接执行
    - RESET 与 MIDI 的 PLAY/RUN 行一样，选中后直接按编码器重置
    - BIND/KNOB 行可把任意参数绑定到主页面编码器，主页面只绑一个
    - V/OCT 量程 1/2/5；GATE/TRIG 极性 +/-；FX SEND 0–100%
    - 输出：**G1 CV=V/OCT，G2 CV=FX SEND，G1 Gate=GATE，G2 Gate=TRIG**
      （每组只有 1 个 DAC CV + 1 个 Gate；FX 只能走 G2 CV）
  - GPIO 中断读编码器；IO23=RANDOM 短按；IO24 短按=MIDI/CLOCK，长按 0.8s=RANDOM
  - 实测 IO24 正常、IO23 无电平变化，判定为 GP23 未接/按钮或焊点问题；加 IO24 长按兜底
- ST7789 240×240：PIO SPI + DMA，**420MHz CPU / 52.5MHz SCLK**，
  SCK/MOSI 12mA + fast slew，`MADCTL=0x60`（90° 方向）
- 屏幕闪烁修复：不再持续整帧刷新；LED 仅在状态变化时用局部 12px 小方块刷新，
  每个局部窗口后加 20µs；MIDI 空闲和 clock 模式均不再红闪/花屏
- **模式隔离**：MIDI/暂停时 clock 定时器根本未注册；播放才 add，暂停/退出立即 cancel
- 所有设置（通道、GATE、V/OCT、BPM、8 个分频、Random 参数/绑定）写入 EEPROM
- **待做**：私有总线(PIO UART, GP14)、右半 5–8 组、DAC 校准。

## 4. 进度
### 已完成
- 硬件方案定稿（12HP / 8 复音 / 2×541 / OLED / EC11 / 私有总线 / 16pin 电源）。
- 面板 12HP 外框 + 4 角安装孔 + 背板；导出 `12hp.dxf`。
- **原理图已接：** SPI 链（Pico→U19→DAC）、Gate 链（Pico→U20）、电源/去耦（含 C7=VREF）、
  编码器(U23)、OLED(TFT1)、总线(U17→GP14)。**已逐脚核对无误。**
- `$1Nundefined` 幽灵网络**已清除**（原理图恢复正常）。
- **输出映射(组别)已定稿并落地**：8 组 = 8 路复音，每组 = 1 Gate 线(GPIO,U20) + 1 CV 线(DAC)。
  详见接线表「十、组别/输出映射总表」。**这是接 CV/Gate 座子的依据。**
- **CV/Gate 座已接好**（U1-U16）；固件已按真实映射改 8 通道。
- **PCB 已布局布线**：连通性逐网络核对**无误**（48 网络）；DRC 剩 5 条（见 4.6，均为"计划内/待确认"）。
- **USB 引出方案定了**：USB 在 Pico **左端**（pin1/pin40 端），Pico 右移使 USB 收回板内；
  左上角开一穿孔（非金属化）走**4 芯软线** → 前面板 USB-C 母座（母座固定在面板，空中走线，不走 FPC 穿板）。
### 4.6 PCB 复核（2026-10-16，只读检查）
- **DRC 3 条**：均为 `U22_41/42/43`(Pico 调试脚) 距板框 0.11mm < 0.3mm → **计划割除**（忽略）。
  （原先与 U22/U23 重叠的两条，已把禁布区改为"只禁铜/内电层、不禁器件"后消失 ✅）
- **禁布区**：矩形 `(2305,-373.58)` 130×316mil，覆盖调试脚；规则=只禁铜。该区 **0 条走线**，
  调试脚 `41/42/43` 网络为空(SMD 焊盘, 无内层连接) → 割除**不会暴露内电层/铜**（前提：叠层里该禁布区对内层也生效）。
- **调试脚区无过孔**（16 个 GND 缝合孔都在区外）；相邻的信号焊盘 20/21(GP15/GP16→20A8/20A7) 距板边 0.9mm，**不要误割**。
- **过孔 958 个**：钻孔 0.3048mm / 外径 0.4064mm → 不触发嘉立创小孔附加费；数量偏多(多为 GND 缝合)可减。
- **线宽**：最小 0.1mm（另有 0.254 / 0.5mm）→ 满足免费多层规则。
- **板框 60.96 × 100.0mm** → 满足 ≤100×100（正好 100.0，临界）。
- **安装孔**：MULTI 层 6 个（5×Ø2.4=M2 + 1×Ø3.2 屏幕 M3）；左上矩形槽距边 0.508mm ≥0.3 ✓。
- **电源核对**：5V = Pico VSYS + U19/U20 VCC + DAC AVDD(3)/CLR#(9) + C2/C3/C5/C6 + CN1(11/12)；
  3V3 = Pico 3V3(36) + OLED VCC(2)/CSF(9) + C1/C4；GND 全覆盖（含 DAC LDAC#(1)、两片 541 的 OE）。
  **均正确**。
### 待办
- [ ] **私有总线(PIO UART, GP14)**。
- [ ] 右半 5–8 组 CV/Gate 焊接与验证。
- [ ] V/oct（1/2/5/8）和 Gate 极性 UI（第二页）。
- [ ] DAC 输出精度实测/校准（如需要）。
- [ ] **【重要】内电层**：板为 4 层，**Inner1/Inner2 为 PLANE 但需确认已分配网络(GND/5V)并铺铜**
      （API 未检出内层铜；很可能立创对 PLANE 层自动浇筑，需在叠层里确认网络分配）。
- [ ] 确认禁布区**不要扫到编码器 U23**（或把该区设为只禁铜、不禁器件）→ 让 DRC 干净。
- [ ] 过孔数量偏多，可选**减少 GND 缝合孔**。
- [ ] （可选）CV 输出加运放缓冲/保护；CN1 补封装。

## 4.5 输出映射（真实, 来自 PCB 走线）
- 一个「组」= 一路复音 = 1 Gate(GPIO) + 1 CV(DAC)。组号按物理座子对：U1&U2=组1 … U15&U16=组8。
- U1..U8：奇=Gate 偶=CV；U9..U16：奇=CV 偶=Gate。
- **DAC 通道顺序被 PCB 打乱**，固件唯一依据如下：

  | 组 | Gate座 | CV座 | Gate GPIO | CV(DAC) | MIDI |
  |---|---|---|---|---|---|
  | 1 | U1 | U2 | GP22 | VOUTB | ch1 |
  | 2 | U3 | U4 | GP21 | VOUTD | ch2 |
  | 3 | U5 | U6 | GP20 | VOUTF | ch3 |
  | 4 | U7 | U8 | GP19 | VOUTH | ch4 |
  | 5 | U10 | U9 | GP15 | VOUTA | ch5 |
  | 6 | U12 | U11 | GP16 | VOUTC | ch6 |
  | 7 | U14 | U13 | GP17 | VOUTE | ch7 |
  | 8 | U16 | U15 | GP18 | VOUTG | ch8 |

### 4.7 上板验证清单（2026-10-22 PCB 到货）
- [x] 目检/供电：板子已上电，屏幕、DAC、Gate 缓冲工作
- [x] 已烧 `firmware\screen_test_20261022_encoder.uf2`：屏幕/编码器/超频测试通过
- [x] 已烧 `firmware\midi2cv_20261022_ui.uf2`（1200bps touch）
- [x] Windows 已出现 `OV-MIDI`（COM18 为 CDC）
- [x] 组1–4：NOTE0=24(C1)=0V，12 半音=1V；Gate 0/5V 正常
- [x] 屏幕 UI：V1..V8 列表、蓝框选择、橙框编辑、旋转改通道、按键退出保存
- [ ] 断电重启确认 EEPROM 通道配置保留
- [ ] 右半 5–8 组焊接并逐路验证
- [ ] 记录 DAC 实测偏差，决定是否需要校准
- [ ] 私有总线（GP14）

### 4.8 2026-10-22 左半 4 组已焊（当前测试范围）
- 只焊了左侧 4 组，右侧 5-8 组未焊。
- **Gate 从上到下**：U20 Y1/Y2/Y3/Y4 → **GP22/GP21/GP20/GP19**
- **CV 从上到下**：DAC 引脚 **13/12/11/10** → VOUTB/VOUTD/VOUTF/VOUTH
- 每两个横排座子为一组：
  - 组1 = U1(Gate)+U2(CV) = **MIDI ch1**
  - 组2 = U3+U4 = **MIDI ch2**
  - 组3 = U5+U6 = **MIDI ch3**
  - 组4 = U7+U8 = **MIDI ch4**
- 与固件一致：`gatePin[]={22,21,20,19,...}`、`dacCh[]={B,D,F,H,...}`
- 测试音符：ch1..ch4；`NOTE0=24 (C1)=0V`，每 +12 半音 = +1V
- **2026-10-22 屏幕 UI（已烧 `midi2cv_20261022_ui.uf2`）**：
  - MIDI 模式列出 V1..V8 + GATE + V/OCT；Clock 模式列出 C1–C8、V5–V8、PLAY、设置
  - 导航模式：旋转选择行（1px 蓝色框）；按下进入编辑
  - 编辑模式：该行橙框/底色高亮；再按退出并保存 EEPROM
  - 编码器用 GPIO 中断，推屏期间不会漏步
  - 选择框用弹簧动画（stiffness 0.30 / damping 0.55，约 60fps），
    有轻微过冲；52.5MHz 下整帧刷新约 17.55ms
  - 面板方向：ST7789 `MADCTL=0x60`（纠正 90° 旋转；原来是 0x00）
  - V/oct 为 **1 / 2 / 5**；GATE 极性 **+ / -**
  - Clock 默认暂停；PLAY 行按编码器启动/暂停；无 RUN/RESET 硬件输出
- **2026-10-22 排查结论**：ch1 曾无反应，串口抓到 `MIDI st=90` 说明 USB-MIDI 正常；
  原因是编码器配置改过通道。强制 ch1..8 + 自检后，组1–4 CV/Gate 均正常。
  最终 UI 已可直观查看/修改通道，不再需要盲操作。

## 5. 版本
- 2026-10-15 v2：删旧设计（ADS8688/TM1638/ADR4550/2×AD5664/TL074/LED），改单颗 DAC 极简架构。
- **2026-10-16 v3：扩到 8 复音；+2× 74AHCT541、+OLED、+EC11、+私有总线、+16pin 电源牛角；无 LED；原理图连线完成并核对。**
- **2026-10-16 v4：清除 `$1Nundefined` 幽灵网络；定稿「组别/输出映射总表」(接线表第十节)，作为接 CV/Gate 座子的依据；固件更新暂缓（先接线定座子位置）。**
- **2026-10-16 v5：CV/Gate 座接好(U1-U16)；通读原理图+PCB：DRC 通过(0 错误)、映射逐网络核对无误；
  固件 `midi2cv.ino` 改 8 通道、引脚换成新分配、去掉 LED；接线表写入真实座子映射。
  发现内电层 Inner1/Inner2 声明为 PLANE 但无铜（待处理）。**
- **2026-10-16 v6：布局调整后的 PCB 复核**（Pico 右移、屏幕右下移、C1/C3/C4 右移；USB 在左端）。
  连通性逐网络核对无误；DRC 5 条（调试脚越小 0.3mm 待割除 + 禁布区扫到 U22/U23）；
  核对嘉立创规则（线宽/过孔/板框均达标）。
- **2026-10-22 v7：PCB 到货；左半 4 组 CV/Gate 已焊；屏幕/编码器测试通过；
  主固件按 16MB flash 编译并烧录，枚举为 OV-MIDI。**
- **2026-10-22 v8：ST7789 UI 完成**（V1..V8/通道列表、蓝框选择、橙框编辑、EEPROM 保存）；
  编码器改 GPIO 中断；`MADCTL=0x60`。
- **2026-10-22 v9（最终时钟/清理）**：
  - 超频实测：400/420/432MHz 稳定（420 实测 421）；436 复位；440/448/450 挂死；
    500/480/510 是 `clock_get_hz` 假象，实际停在 PLL_USB 48MHz；520 落在 ROSC ~7MHz
  - 固定 **420MHz**，校验 PLL lock + CLK_SYS 源，失败回退 125MHz；flash /4 安全
  - 屏幕曾用 62.5MHz（420÷2÷3.36 小数分频），后因动态刷新红屏改为 52.5MHz
  - 70MHz 起偏红/数据错误，84/105 不可用
  - 清理所有串口调试/计时/BRINGUP/超频阶梯；`oc_test` 保留为独立验证工具
- **2026-10-22 v10（clock 模式 + UI 收尾）**：
  - IO24 进入/退出 clock；组1=RUN/RESET，组2–4 六个插孔=C1–C6，组5–8=MIDI V5–V8
  - 主 BPM + 24 个 docB 风格分频；RUN=TRIG/HOLD
  - UI 单列可滚动；MIDI 10 行、clock 15 行；蓝色通道条、12px 指示灯、1px 细框
  - 修复屏幕间歇红闪/重启：去掉持续整帧刷新，LED 仅在变化时局部刷新
- **2026-10-22 v11（最终 clock/隔离/屏幕）**：
  - 组1–4 的 8 个插孔全部做 clock：C1–C8；无 RUN/RESET 硬件输出
  - Clock 默认暂停；PLAY 行按编码器启动/暂停；MIDI Start/Stop/Continue 也可控制
  - 暂停/退出立即取消定时器；MIDI 模式根本未注册 clock 定时器
  - 屏幕 420MHz ÷ 2 ÷ 4.0 = **52.5MHz** 整数分频；12mA + fast slew
  - 通道条蓝色、CH 文字在条后、12px 指示灯、1px 细框、行间分隔线
- **2026-10-22 v12（RandomSynth 模式）**：
  - IO23 新增第三模式：移植 `RandomSynth-VCV` 的 WorldCV 粒子碰撞引擎
  - 场地适配 240×190 屏幕；圆形/矩形/椭圆/六角均铺满，底部留参数区
  - 主页面全屏场地 + 5 行小字参数；编码器默认调场地大小，按下进设置
  - 设置页沿用 MIDI 列表；加球/加速器显示数量，RESET 直接按；SHAPE 可选
  - 所有参数可绑定到主页面编码器；V/OCT 1/2/5、GATE/TRIG 极性、FX SEND 可调
  - 输出：G1 CV=V/OCT、G2 CV=FX SEND、G1 Gate=GATE、G2 Gate=TRIG
  - 屏幕 30fps 全场刷新；设置页不刷场地，避免闪屏
- **2026-10-22 v13（Random 界面/125MHz 测试/回超频）**：
  - 先降到 125MHz/1.1V 实测 Random 可运行，然后改回 420MHz/52.5MHz
  - Clock 列表 PLAY 移到最顶
  - Random 场地从 190 加到 210px；底部删掉全部参数说明，只留 KNOB
  - 底部右侧显示 OUT 音名、GATE ON/OFF、TRIG ON/OFF（TRIG 状态保持 120ms 便于看）
  - 连续参数 UI 统一 0.00–1.00，内部 min/max 效果不变（BALL 最大仍 1.6×）
  - 小球碰撞发光/闪白增强，碰撞场 alpha 提高

## 6. 当前状态（2026-10-22）
- 当前固件：`firmware\midi2cv_random_20261022.uf2`
  - 16MB flash + `boot2_w25q128jvxq_4` + TinyUSB
  - CPU **420MHz**（PLL 校验通过；失败回退 125MHz）
  - 屏幕 **52.5MHz SCLK**（整数分频），整帧约 17.55ms，UI 约 50fps
- 已验证：左半组1–4 CV/Gate、MIDI ch1–4、屏幕 UI、编码器选择/编辑/保存、颜色正常
- 已验证：MIDI 空闲和 clock 模式均不再红闪/花屏
- Clock 模式：8 路 C1–C8，BPM 20–300，24 个分数，PLAY 手动启停，4 个 MIDI 通道仍可用
- Random 模式：IO23 短按或长按 IO24 0.8s 进入；12 球粒子场、碰撞发出 V/OCT/GATE/TRIG/FX
- IO23 实测无反应（IO24 正常），疑似 GP23 未接/按钮焊接；IO23 代码保留
- 模式隔离：MIDI/暂停时无 clock 定时器；播放才注册，暂停/退出立即取消；Random 下定时器关闭
- Random 输出映射：G1 CV=V/OCT、G2 CV=FX、G1 Gate=GATE、G2 Gate=TRIG
- 未验证：右半组5–8、EEPROM 断电保持、DAC 精度、私有总线
- 超频结论：420/432 稳定，436+ 不稳定；500 从未真正跑起来
- 屏幕结论：52.5MHz 稳定；62.5MHz 小数分频动态刷新红屏，70MHz+ 颜色错误

## 7. 下次继续
1. 焊右半 5–8 组，逐路测 CV/Gate
2. 断电重启，确认通道/clock 设置能从 EEPROM 恢复
3. Random 模式实机验证：场地绘制、输出映射、按键/编码器、EEPROM 恢复
4. 私有总线（GP14，PIO UART，帧格式见 `vcv-rack/src/PrivateBus.hpp`）
5. 快速转动若跳步，加 **200µs** 编码器消抖（不要 1ms）
6. 需要时再做 DAC 输出精度校准
7. `firmware\oc_test` 是超频验证工具，不参与主固件编译
