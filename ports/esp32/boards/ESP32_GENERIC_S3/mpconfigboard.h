#ifndef MICROPY_HW_BOARD_NAME
// Can be set by mpconfigboard.cmake.
#define MICROPY_HW_BOARD_NAME               "Generic ESP32S3 module"
#endif
#define MICROPY_HW_MCU_NAME                 "ESP32-S3"

// 将 ESP32-S3 标准输入环形缓冲区扩大到四千零九十六字节，完整承接
// fn-vision 单轮协议读取预算；构建参数仍可按需覆盖为八千一百九十二字节。
#ifndef MICROPY_HW_STDIN_BUFFER_SIZE
#define MICROPY_HW_STDIN_BUFFER_SIZE        (4096)
#endif

// 启用固件内置的第二路 TinyUSB CDC，接口零保留给 REPL，接口一专供 PV1 数据。
#define MICROPY_HW_USB_CDC_DATA             (1)
// 数据 CDC 在 C 层使用独立环形缓冲，业务线程短时阻塞不会停止 USB OUT 接收。
#define MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE  (32768)
#define MICROPY_HW_USB_CDC_DATA_TX_TIMEOUT  (1000)
// 固定使用编译期双 CDC 描述符，禁止 Python 运行期重配整个 USB 设备。
#define MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE (0)

// Enable UART REPL for modules that have an external USB-UART and don't use native USB.
#define MICROPY_HW_ENABLE_UART_REPL         (1)

#define MICROPY_HW_I2C0_SCL                 (9)
#define MICROPY_HW_I2C0_SDA                 (8)
