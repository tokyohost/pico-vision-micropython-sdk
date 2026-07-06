# fn_canvas

`fn_canvas` 是 fn-vision 面向 MicroPython RP2 固件的 RGB565 Canvas 原生模块，
当前接口版本为 `5`，提供清屏、点、矩形、线段、边框、多边形、点阵网格、批量折线、
批量采样列和可配置折线图组件。折线图组件在一次 C 调用中完成数据缩放、连续插值、
实心填充、数值区间着色与背景点阵绘制。

在 `ports/rp2` 目录执行：

```sh
make BOARD=RPI_PICO USER_C_MODULES=modules/fn_canvas/micropython.cmake
```

固件内可执行以下代码确认模块可用：

```python
import fn_canvas
print(fn_canvas.api_version())
```

输出 `5` 表示固件与当前 `canvasC.py` 兼容。
