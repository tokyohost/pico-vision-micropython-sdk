"""验证 ESP32-S3 固件内 fn-vision 原生模块的基本行为。"""

import fn_canvas
import fn_protocol


def _crc16_ccitt_false(data):
    """计算与 PV1 协议一致的 CRC-16/CCITT-FALSE。"""
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _test_canvas():
    """验证 RGB565 字节序、视口坐标和裁剪逻辑。"""
    width = 4
    height = 3
    buffer = bytearray(width * height * 2)

    assert fn_canvas.api_version() == 7
    fn_canvas.clear(buffer, width, height, 10, 20, 0x1234)
    assert buffer == bytes((0x12, 0x34)) * (width * height)

    fn_canvas.pixel(buffer, width, height, 10, 20, 11, 21, 0xABCD)
    pixel_offset = (1 * width + 1) * 2
    assert buffer[pixel_offset:pixel_offset + 2] == b"\xAB\xCD"

    fn_canvas.fill_rect(buffer, width, height, 10, 20, 8, 19, 3, 3, 0x07E0)
    assert buffer[0:2] == b"\x07\xE0"
    assert buffer[2:4] == b"\x12\x34"


def _test_protocol():
    """验证 PV1 帧解析、尾部填充和 CRC 校验。"""
    message_type = b"DATA"
    payload = b"abc"
    crc = _crc16_ccitt_false(message_type + b":" + payload)
    frame = (
        b"PV1:DATA:3:" + ("%04X" % crc).encode() + b":" + payload + b"  "
    )

    assert fn_protocol.api_version() == 1
    assert fn_protocol.parse_frame(frame, 32) == ("DATA", payload)

    invalid_frame = frame[:-3] + b"d" + frame[-2:]
    try:
        fn_protocol.parse_frame(invalid_frame, 32)
    except ValueError as error:
        assert str(error) == "BAD_FRAME_CRC"
    else:
        raise AssertionError("损坏的 PV1 载荷未触发 CRC 异常")


def main():
    """执行全部原生模块冒烟测试。"""
    _test_canvas()
    _test_protocol()
    print("fn_canvas 与 fn_protocol ESP32-S3 冒烟测试通过")


main()
