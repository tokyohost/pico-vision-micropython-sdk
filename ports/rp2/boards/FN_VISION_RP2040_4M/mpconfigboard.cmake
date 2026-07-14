# 为 fn-vision 四兆字节 RP2040 板选择标准 Pico 硬件定义。
set(PICO_BOARD "pico")
set(PICO_PLATFORM "rp2040")
set(PICO_FLASH_SIZE_BYTES 4194304)
set(FN_CANVAS_BUILTIN_FONTS 1)
set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)

# 为双语字体固件预留 1.5 MiB，其余 2.5 MiB 作为 MicroPython 文件系统。
set(MICROPY_HW_FLASH_STORAGE_BYTES 2621440)
