/*
 * fn-vision ESP32-S3 LCD DMA 传输模块的 MicroPython 绑定。
 */

#include "py/obj.h"
#include "py/runtime.h"
#include "mphalport.h"

#include "fn_lcd_dma.h"

#define FN_LCD_API_VERSION (1)

static fn_lcd_dma_context_t fn_lcd_context;

/** 返回 fn_lcd 原生模块接口版本。 */
static mp_obj_t fn_lcd_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(FN_LCD_API_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_api_version_obj, fn_lcd_api_version);

/** 初始化内部 DMA 双缓冲，并返回实际使用的单块容量。 */
static mp_obj_t fn_lcd_init(size_t n_args, const mp_obj_t *args) {
    size_t requested_chunk_size = n_args > 0
        ? mp_obj_get_int(args[0]) : FN_LCD_DMA_MAX_CHUNK_SIZE;
    size_t actual_chunk_size = 0;
    esp_err_t result = fn_lcd_dma_init(
        &fn_lcd_context, requested_chunk_size, &actual_chunk_size
    );
    check_esp_err(result);
    return mp_obj_new_int_from_uint(actual_chunk_size);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fn_lcd_init_obj, 0, 1, fn_lcd_init);

/** 释放 fn_lcd 占用的内部 DMA 双缓冲。 */
static mp_obj_t fn_lcd_deinit(void) {
    fn_lcd_dma_deinit(&fn_lcd_context);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_deinit_obj, fn_lcd_deinit);

/** 通过指定 machine.SPI 对象写入一块像素数据。 */
static mp_obj_t fn_lcd_write(mp_obj_t spi_in, mp_obj_t pixels_in) {
    if (!fn_lcd_dma_is_initialized(&fn_lcd_context)) {
        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT("fn_lcd is not initialized")
        );
    }
    mp_buffer_info_t pixels;
    mp_get_buffer_raise(pixels_in, &pixels, MP_BUFFER_READ);
    spi_device_handle_t spi = machine_hw_spi_get_device(spi_in);
    uint32_t completed_transactions = 0;
    esp_err_t result = fn_lcd_dma_write(
        &fn_lcd_context,
        spi,
        pixels.buf,
        pixels.len,
        &completed_transactions
    );
    check_esp_err(result);
    return mp_obj_new_int_from_uint(pixels.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(fn_lcd_write_obj, fn_lcd_write);

/** 返回 DMA 缓冲容量及累计写入统计。 */
static mp_obj_t fn_lcd_stats(void) {
    mp_obj_t result = mp_obj_new_dict(4);
    mp_obj_dict_store(
        result,
        MP_OBJ_NEW_QSTR(MP_QSTR_chunk_size),
        mp_obj_new_int_from_uint(fn_lcd_context.chunk_size)
    );
    mp_obj_dict_store(
        result,
        MP_OBJ_NEW_QSTR(MP_QSTR_write_count),
        mp_obj_new_int_from_uint(fn_lcd_context.write_count)
    );
    mp_obj_dict_store(
        result,
        MP_OBJ_NEW_QSTR(MP_QSTR_byte_count),
        mp_obj_new_int_from_ull(fn_lcd_context.byte_count)
    );
    mp_obj_dict_store(
        result,
        MP_OBJ_NEW_QSTR(MP_QSTR_transaction_count),
        mp_obj_new_int_from_uint(fn_lcd_context.transaction_count)
    );
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_stats_obj, fn_lcd_stats);

static const mp_rom_map_elem_t fn_lcd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_lcd) },
    { MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_lcd_api_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&fn_lcd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&fn_lcd_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&fn_lcd_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&fn_lcd_stats_obj) },
};
static MP_DEFINE_CONST_DICT(fn_lcd_module_globals, fn_lcd_module_globals_table);

const mp_obj_module_t fn_lcd_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&fn_lcd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_lcd, fn_lcd_user_cmodule);
