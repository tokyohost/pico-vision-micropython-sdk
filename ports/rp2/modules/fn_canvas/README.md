# fn_canvas

`fn_canvas` 是 fn-vision 面向 MicroPython RP2 固件的 RGB565 Canvas 原生模块，
当前接口版本为 `7`，提供清屏、点、矩形、线段、可调粗细边框、多边形、点阵网格、批量折线、
批量采样列、批量文字、批量绘图命令和可配置折线图组件。折线图组件在一次 C 调用中
完成数据缩放、连续插值、实心填充、数值区间着色与背景点阵绘制。模块同时支持
RP2040 和 RP2350，并对 RP2350 ARM、RISC-V 的非对齐内存访问规则保持兼容。

在 `ports/rp2` 目录执行：

```sh
make BOARD=RPI_PICO USER_C_MODULES=modules/fn_canvas/micropython.cmake
```

RP2350（Raspberry Pi Pico 2）构建命令：

```sh
make BOARD=RPI_PICO2 USER_C_MODULES=modules/fn_canvas/micropython.cmake
```

固件内可执行以下代码确认模块可用：

```python
import fn_canvas
print(fn_canvas.api_version())
```

输出 `7` 表示固件与当前 `canvasC.py` 兼容。矩形边框接口支持可选粗细参数：

```python
fn_canvas.draw_rect(buffer, width, height, origin_x, origin_y,
    x, y, rect_width, rect_height, color, thickness)
```

省略 `thickness` 时默认绘制一像素边框，以兼容旧调用。
