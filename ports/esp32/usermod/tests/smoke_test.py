"""验证 ESP32-S3 固件内 fn-vision 原生模块的基本行为。"""

import fn_canvas
import fn_lcd
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

    assert fn_canvas.api_version() == 8
    assert callable(fn_canvas.font_glyph)
    assert callable(fn_canvas.text_width)
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


def _test_lcd_dma():
    """验证 LCD 原生模块可初始化完整方案并自动比较完整画布。"""
    configuration = {
        "width": 4,
        "height": 3,
        "spi_id": 2,
        "sck": 12,
        "mosi": 11,
        "miso": 15,
        "cs": 10,
        "dc": 9,
        "rst": 14,
        "backlight": 13,
        "baudrate": 40_000_000,
        "dma_chunk_size": 4092,
        "strip_height": 2,
        "tile_width": 2,
        "tile_height": 1,
    }
    assert fn_lcd.api_version() == 2
    assert fn_lcd.init(configuration) == 4092
    stats = fn_lcd.stats()
    assert stats["chunk_size"] == 4092
    assert stats["strip_buffer_size"] == 16
    assert stats["write_count"] == 0
    frame = bytearray(4 * 3 * 2)
    assert fn_lcd.dirty_regions(frame) == [(0, 0, 4, 3)]
    fn_lcd.commit_frame()
    assert fn_lcd.dirty_regions(frame) == []
    fn_lcd.commit_frame()
    frame[0:2] = b"\x12\x34"
    assert fn_lcd.dirty_regions(frame) == [(0, 0, 2, 1)]
    fn_lcd.discard_frame()
    frame[16:18] = b"\x56\x78"
    assert fn_lcd.dirty_regions(frame) == [(0, 0, 2, 3)]
    fn_lcd.discard_frame()
    fn_lcd.deinit()
    assert fn_lcd.stats()["chunk_size"] == 0


def main():
    """执行全部原生模块冒烟测试。"""
    _test_canvas()
    _test_lcd_dma()
    _test_protocol()
    print("fn_canvas、fn_lcd 与 fn_protocol ESP32-S3 冒烟测试通过")


main()
