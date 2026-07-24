# fn-vision ESP32-S3 原生模块

本目录将 `fn_canvas`、`fn_lcd`、`fn_protocol` 和 `_usb_cdc_data` 接入
MicroPython 的 ESP32 CMake 构建。绘图和协议模块复用通用源码；`fn_lcd` 接收
屏幕与 GPIO 方案，自动比较完整 RGB565 画布，使用两块内部 SRAM 条带缓冲和
两块内部 DMA 缓冲只发送变化区域；数据 CDC 模块绑定 ESP32-S3 固件内置的第二路
TinyUSB CDC。

## 准备环境

先按 MicroPython ESP32 端口说明安装并导出仓库所要求版本的 ESP-IDF，然后构建
`mpy-cross` 和 ESP32 子模块：

```sh
make -C mpy-cross
make -C ports/esp32 submodules BOARD=ESP32_GENERIC_S3
```

## 构建 ESP32-S3 固件

在 MicroPython 仓库根目录执行：

```sh
make -C ports/esp32 \
  BOARD=ESP32_GENERIC_S3 \
  USER_C_MODULES="$(pwd)/ports/esp32/usermod/micropython.cmake"
```

N8R8 模组使用 8MiB Flash 与 8MiB Octal PSRAM 变体：

```sh
make -C ports/esp32 \
  BOARD=ESP32_GENERIC_S3 \
  BOARD_VARIANT=N8R8 \
  USER_C_MODULES="$(pwd)/ports/esp32/usermod/micropython.cmake"
```

N16R8 模组使用 16MiB Flash 与 8MiB Octal PSRAM 变体：

```sh
make -C ports/esp32 \
  BOARD=ESP32_GENERIC_S3 \
  BOARD_VARIANT=N16R8 \
  USER_C_MODULES="$(pwd)/ports/esp32/usermod/micropython.cmake"
```

这里使用绝对路径是因为 ESP32 的 MicroPython 主组件位于 `ports/esp32/main`，
可避免 `USER_C_MODULES` 相对路径以主组件目录为基准时产生歧义。构建日志应包含：

```text
Found User C Module(s): usermod_fn_canvas, usermod_fn_lcd, usermod_fn_protocol, usermod_fn_usb_cdc
```

普通固件位于 `ports/esp32/build-ESP32_GENERIC_S3/firmware.bin`；N8R8 与 N16R8
固件分别位于 `ports/esp32/build-ESP32_GENERIC_S3-N8R8/firmware.bin` 和
`ports/esp32/build-ESP32_GENERIC_S3-N16R8/firmware.bin`。

## 烧录和验证

将下面命令中的串口替换为设备实际端口：

```sh
make -C ports/esp32 \
  BOARD=ESP32_GENERIC_S3 \
  USER_C_MODULES="$(pwd)/ports/esp32/usermod/micropython.cmake" \
  PORT=/dev/ttyACM0 deploy
```

烧录后可在 REPL 中检查接口版本：

```python
import fn_canvas
import fn_lcd
import fn_protocol
import _usb_cdc_data

print(fn_canvas.api_version())
print(fn_lcd.api_version())
print(fn_protocol.api_version())
print(_usb_cdc_data.api_version())
print(_usb_cdc_data.init())
```

当前带双语字体、完整画布 LCD DMA 和原生双 CDC 的 ESP32-S3 固件应依次输出
`8`、`2`、`1`、`1` 和 `32768`。`fn_lcd.init()` 的屏幕、脚位和缓冲配置示例已
包含在设备端冒烟测试中，不应再使用 API 1 的单整数初始化方式。数据 CDC 的
32 KB 接收环形缓冲由 `init()` 优先从 PSRAM 分配，PSRAM 不可用时才回退到内部 DRAM。`fn_canvas` 同时提供
`font_glyph()` 与 `text_width()`，并内置 `wqy_8x16`、`fusion_pixel_8x16` 两套
英文半角八像素、中文全角十六像素字体。完整设备端冒烟测试位于
`ports/esp32/usermod/tests/smoke_test.py`，可使用 `mpremote` 执行：

```sh
mpremote connect /dev/ttyACM0 run ports/esp32/usermod/tests/smoke_test.py
```

若只需单个模块，可将 `USER_C_MODULES` 分别指向以下文件的绝对路径：

- `ports/esp32/usermod/fn_canvas/micropython.cmake`
- `ports/esp32/usermod/fn_lcd/micropython.cmake`
- `ports/esp32/usermod/fn_protocol/micropython.cmake`
