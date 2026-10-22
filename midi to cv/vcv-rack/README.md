# OV MIDI to CV — VCV Rack 2 port

这是 `midi to cv` 固件的 VCV Rack 2 复刻版。**一个模块**同时提供：

- **8× CV + 8× Gate**：通用 CV/Gate 输出，给其他模块用
- **1× 私有总线**：给自制 VCO 传 8 voice 的 note/gate/bend
  - 硬件上是面板那个 3.5mm GP14 接口
  - Rack 里用一个 **polyphonic 输出口**表示，16 通道

## 功能

- 8 voice，每 voice 独立 MIDI channel（1–16）
- 16 深 note stack，最后音优先
- 1/2/5/8 V/oct
- Gate 极性可反转
- Pitch Bend 进入私有总线（通用 CV/Gate 不叠加 bend，和硬件固件一致）
- 没有屏幕/编码器：设置全在右键菜单
- 设置随 Rack patch 保存（JSON）

## 文件

```
plugin.json
Makefile
src/plugin.hpp
src/plugin.cpp
src/Midi2Cv.cpp
src/PrivateBus.hpp
res/Midi2Cv.svg
```

## 构建

需要：

- VCV Rack SDK 2.x（已安装在 `D:\Rack-SDK\Rack-SDK`）
- MSYS2 + mingw-w64
- `make`、`g++`

当前机器还没有 `g++` / `make`，所以没有在这里实际编译。装好 MSYS2 后：

```bash
cd "/d/.../midi to cv/vcv-rack"
export RACK_DIR=/d/Rack-SDK/Rack-SDK
make
```

构建后把整个 `vcv-rack` 文件夹复制到：

```
C:\Users\<你>\Documents\Rack2\plugins\
```

## 使用

面板：

- 左列：V1..V8 CV
- 右列：V1..V8 Gate
- 底部：**PRIVATE BUS** poly 输出口

把 VCO 模块的 poly 输入接到这个 PRIVATE BUS 口；8 个 CV/Gate 是另外的通用输出，不是私有总线。

右键模块：

- **V/oct**：1 / 2 / 5 / 8
- **Invert gates**
- **MIDI channels**：每个 voice 选 MIDI 通道 1–16
- **All notes off**
- 最下方：MIDI 驱动/设备/通道

**MIDI 设备的 Channel 请选 All**，否则 per-voice 通道映射收不到全部 16 个通道。

## 私有协议

硬件 GP14 的字节帧：

```
[0xAA][voice][note][gate][bendL][bendH][CRC8]
```

Rack 里用 poly 16 通道承载同样的字段：

```
channel 2v   = voice(3) | note(7) | gate(1)      // 11 bit
channel 2v+1 = bend(16) | CRC8(8)                // 24 bit
```

- 每个值都 ≤ 2^24，float 可精确表示
- `src/PrivateBus.hpp` 同时提供字节帧 codec 和 poly pack/unpack
- Pico 固件用字节帧 + GP14 UART；Rack 用 poly 传输，字段与 CRC 一致

## 注意

- 8× CV/Gate 是**通用输出**，不是私有总线。
- Rack 没有数字 3.5mm 接口，所以私有总线用 poly 口承载；硬件上才是 GP14 3.5mm。
- 硬件 CV 输出 0–5V（NOTE0=24 为 0V），低于 C1 会钳到 0V。
- Gate 输出 0/5V（硬件行为）；不认 5V gate 的 Rack 模块可把 `5.f` 改成 `10.f`。
- Rack 的 SVG 渲染器不渲染普通 `<text>`，面板文字可能不显示；端口 tooltip 仍有名字。
- 当前环境没有 MSYS2/g++，未实际编译。
