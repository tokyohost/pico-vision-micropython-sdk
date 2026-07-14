/*
 * ESP32-S3 固件内置数据 CDC 的 MicroPython 绑定。
 * 数据收发由 TinyUSB C 驱动完成，Python 层只访问已经缓存的数据。
 */

#include "py/runtime.h"
#include "esp_heap_caps.h"
#include "usb_cdc_data_port.h"

#if MICROPY_HW_USB_CDC_DATA

static uint8_t *usb_cdc_data_rx_buffer;

// 初始化数据 CDC 接收缓冲区，优先使用 PSRAM，避免长期占用内部 DRAM。
static mp_obj_t usb_cdc_data_init(void) {
    if (usb_cdc_data_rx_buffer == NULL) {
        size_t allocation_size = MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE + 1;
        usb_cdc_data_rx_buffer = heap_caps_malloc(
            allocation_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (usb_cdc_data_rx_buffer == NULL) {
            usb_cdc_data_rx_buffer = heap_caps_malloc(
                allocation_size,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            );
        }
        if (usb_cdc_data_rx_buffer == NULL) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("USB CDC RX buffer allocation failed"));
        }
    }
    mp_usbd_cdc_data_rx_configure(
        usb_cdc_data_rx_buffer,
        MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE + 1
    );
    return MP_OBJ_NEW_SMALL_INT(MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_init_obj, usb_cdc_data_init);

// 返回数据 CDC 当前无需等待即可读取的字节数。
static mp_obj_t usb_cdc_data_any(void) {
    return mp_obj_new_int_from_uint(mp_usbd_cdc_data_rx_any());
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_any_obj, usb_cdc_data_any);

// 把数据 CDC 已收到的字节复制到调用方提供的可写缓冲区。
static mp_obj_t usb_cdc_data_readinto(mp_obj_t buffer_in) {
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(buffer_in, &buffer, MP_BUFFER_WRITE);
    size_t count = mp_usbd_cdc_data_rx_read(buffer.buf, buffer.len);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(usb_cdc_data_readinto_obj, usb_cdc_data_readinto);

// 把调用方提供的数据写入固件内置数据 CDC。
static mp_obj_t usb_cdc_data_write(mp_obj_t buffer_in) {
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(buffer_in, &buffer, MP_BUFFER_READ);
    size_t count = mp_usbd_cdc_data_tx_write(buffer.buf, buffer.len);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(usb_cdc_data_write_obj, usb_cdc_data_write);

// 返回主机是否已经打开固件内置数据 CDC 端口。
static mp_obj_t usb_cdc_data_is_open(void) {
    return mp_obj_new_bool(mp_usbd_cdc_data_connected());
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_is_open_obj, usb_cdc_data_is_open);

// 立即提交数据 CDC 的 TinyUSB 发送 FIFO。
static mp_obj_t usb_cdc_data_flush(void) {
    mp_usbd_cdc_data_tx_flush();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_flush_obj, usb_cdc_data_flush);

// 返回固件内置数据 CDC 绑定的接口版本。
static mp_obj_t usb_cdc_data_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(1);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_api_version_obj, usb_cdc_data_api_version);

static const mp_rom_map_elem_t usb_cdc_data_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__usb_cdc_data) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&usb_cdc_data_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&usb_cdc_data_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&usb_cdc_data_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&usb_cdc_data_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_open), MP_ROM_PTR(&usb_cdc_data_is_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&usb_cdc_data_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&usb_cdc_data_api_version_obj) },
};
static MP_DEFINE_CONST_DICT(usb_cdc_data_module_globals, usb_cdc_data_module_globals_table);

const mp_obj_module_t usb_cdc_data_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_cdc_data_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__usb_cdc_data, usb_cdc_data_user_cmodule);

#endif
