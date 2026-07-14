// fn-vision 四兆字节 RP2040 显示板配置。
#define MICROPY_HW_BOARD_NAME "fn-vision RP2040 4M"
#define MICROPY_HW_MCU_NAME "RP2040"
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif
