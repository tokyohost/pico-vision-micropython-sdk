# fn_protocol

`fn_protocol` 是 fn-vision 面向 MicroPython RP2 固件的 PV1 原生解析模块。
当前接口版本为 `1`，负责帧头、长度、尾部填充、CRC-16/CCITT-FALSE 和消息类型校验。
JSON 解压、解析及业务分发仍由 Python 完成。

建议与 `fn_canvas` 一起构建：

```sh
make BOARD=RPI_PICO \
  USER_C_MODULES=modules/micropython.cmake
```

固件内可执行以下代码确认模块可用：

```python
import fn_protocol
print(fn_protocol.api_version())
```

输出 `1` 表示固件与当前 `protocol_backend.py` 兼容。模块不存在或接口版本不匹配时，
Pico 会自动使用原有 Python 协议解析器。
