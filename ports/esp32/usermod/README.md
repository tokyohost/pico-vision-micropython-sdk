# fn-vision ESP32-S3 原生模块

本目录将 `fn_canvas` 和 `fn_protocol` 接入 MicroPython 的 ESP32 CMake 构建。
两个模块只使用通用 MicroPython C API，不依赖 RP2 外设；ESP32-S3 构建直接复用
现有单份源码，以保证 Pico 与 ESP32-S3 的接口和行为一致。

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

带八线 PSRAM 的 ESP32-S3 模组可使用官方变体：

```sh
make -C ports/esp32 \
  BOARD=ESP32_GENERIC_S3 \
  BOARD_VARIANT=SPIRAM_OCT \
  USER_C_MODULES="$(pwd)/ports/esp32/usermod/micropython.cmake"
```

这里使用绝对路径是因为 ESP32 的 MicroPython 主组件位于 `ports/esp32/main`，
可避免 `USER_C_MODULES` 相对路径以主组件目录为基准时产生歧义。构建日志应包含：

```text
Found User C Module(s): usermod_fn_canvas, usermod_fn_protocol
```

固件位于 `ports/esp32/build-ESP32_GENERIC_S3/firmware.bin`；使用 PSRAM 变体时位于
`ports/esp32/build-ESP32_GENERIC_S3-SPIRAM_OCT/firmware.bin`。

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
import fn_protocol

print(fn_canvas.api_version())
print(fn_protocol.api_version())
```

当前应分别输出 `7` 和 `1`。完整设备端冒烟测试位于
`ports/esp32/usermod/tests/smoke_test.py`，可使用 `mpremote` 执行：

```sh
mpremote connect /dev/ttyACM0 run ports/esp32/usermod/tests/smoke_test.py
```

若只需单个模块，可将 `USER_C_MODULES` 分别指向以下文件的绝对路径：

- `ports/esp32/usermod/fn_canvas/micropython.cmake`
- `ports/esp32/usermod/fn_protocol/micropython.cmake`
