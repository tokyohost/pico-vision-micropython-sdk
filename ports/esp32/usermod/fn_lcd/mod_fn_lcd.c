/*
 * fn-vision ESP32-S3 完整画布 LCD DMA 模块的 MicroPython 绑定。
 */

#include "py/obj.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "mphalport.h"

#include "fn_lcd_dma.h"

#define FN_LCD_API_VERSION (3)

static fn_lcd_dma_context_t fn_lcd_context;

/** 从初始化字典读取必需整数，并统一完成 MicroPython 类型转换。 */
static int fn_lcd_config_int(mp_obj_t config, qstr key) {
    return mp_obj_get_int(mp_obj_dict_get(config, MP_OBJ_NEW_QSTR(key)));
}

/** 从初始化字典读取允许使用默认值的整数。 */
static int fn_lcd_config_int_default(mp_obj_t config, qstr key,
    int default_value) {
    mp_obj_dict_t *dictionary = MP_OBJ_TO_PTR(config);
    mp_map_elem_t *element = mp_map_lookup(
        &dictionary->map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return element == NULL ? default_value : mp_obj_get_int(element->value);
}

/** 从初始化字典读取允许使用默认值的布尔开关。 */
static bool fn_lcd_config_bool_default(mp_obj_t config, qstr key,
    bool default_value) {
    mp_obj_dict_t *dictionary = MP_OBJ_TO_PTR(config);
    mp_map_elem_t *element = mp_map_lookup(
        &dictionary->map, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return element == NULL ? default_value : mp_obj_is_true(element->value);
}

/** 返回 fn_lcd 原生模块接口版本。 */
static mp_obj_t fn_lcd_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(FN_LCD_API_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_api_version_obj, fn_lcd_api_version);

/** 使用屏幕方案、GPIO 脚位和缓冲参数初始化固件显示引擎。 */
static mp_obj_t fn_lcd_init(mp_obj_t config_in) {
    if (!mp_obj_is_type(config_in, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("LCD config must be a dict"));
    }
    fn_lcd_config_t config = {
        .width = fn_lcd_config_int(config_in, MP_QSTR_width),
        .height = fn_lcd_config_int(config_in, MP_QSTR_height),
        .x_offset = fn_lcd_config_int_default(
            config_in, MP_QSTR_x_offset, 0),
        .y_offset = fn_lcd_config_int_default(
            config_in, MP_QSTR_y_offset, 0),
        .strip_height = fn_lcd_config_int_default(
            config_in, MP_QSTR_strip_height, FN_LCD_DEFAULT_STRIP_HEIGHT),
        .tile_width = fn_lcd_config_int_default(
            config_in, MP_QSTR_tile_width, FN_LCD_DEFAULT_TILE_WIDTH),
        .tile_height = fn_lcd_config_int_default(
            config_in, MP_QSTR_tile_height, FN_LCD_DEFAULT_TILE_HEIGHT),
        .spi_id = fn_lcd_config_int(config_in, MP_QSTR_spi_id),
        .sck = fn_lcd_config_int(config_in, MP_QSTR_sck),
        .mosi = fn_lcd_config_int(config_in, MP_QSTR_mosi),
        .miso = fn_lcd_config_int_default(config_in, MP_QSTR_miso, -1),
        .cs = fn_lcd_config_int(config_in, MP_QSTR_cs),
        .dc = fn_lcd_config_int(config_in, MP_QSTR_dc),
        .rst = fn_lcd_config_int(config_in, MP_QSTR_rst),
        .backlight = fn_lcd_config_int(config_in, MP_QSTR_backlight),
        .baudrate = fn_lcd_config_int(config_in, MP_QSTR_baudrate),
        .dma_chunk_size = fn_lcd_config_int_default(
            config_in, MP_QSTR_dma_chunk_size, FN_LCD_DMA_MAX_CHUNK_SIZE),
        .sync_visible_frame_to_second = fn_lcd_config_bool_default(
            config_in, MP_QSTR_sync_visible_frame_to_second, false),
    };
    size_t actual_chunk_size = 0;
    check_esp_err(fn_lcd_dma_init(
        &fn_lcd_context, &config, &actual_chunk_size));
    return mp_obj_new_int_from_uint(actual_chunk_size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(fn_lcd_init_obj, fn_lcd_init);

/** 设置后续可见帧是否在原生 DMA 层自动对齐下一整秒。 */
static mp_obj_t fn_lcd_set_visible_frame_second_sync(mp_obj_t enabled_in) {
    if (!fn_lcd_dma_is_initialized(&fn_lcd_context)) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("fn_lcd is not initialized"));
    }
    return mp_obj_new_bool(fn_lcd_dma_set_visible_frame_second_sync(
        &fn_lcd_context, mp_obj_is_true(enabled_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(
    fn_lcd_set_visible_frame_second_sync_obj,
    fn_lcd_set_visible_frame_second_sync);

/** 释放 fn_lcd 占用的全部固件侧缓冲。 */
static mp_obj_t fn_lcd_deinit(void) {
    fn_lcd_dma_deinit(&fn_lcd_context);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_deinit_obj, fn_lcd_deinit);

/** 保留旧局部刷新能力，通过 DMA 双缓冲发送一块连续像素数据。 */
static mp_obj_t fn_lcd_write(mp_obj_t spi_in, mp_obj_t pixels_in) {
    if (!fn_lcd_dma_is_initialized(&fn_lcd_context)) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("fn_lcd is not initialized"));
    }
    mp_buffer_info_t pixels;
    mp_get_buffer_raise(pixels_in, &pixels, MP_BUFFER_READ);
    uint32_t completed_transactions = 0;
    check_esp_err(fn_lcd_dma_write(
        &fn_lcd_context,
        machine_hw_spi_get_device(spi_in),
        pixels.buf,
        pixels.len,
        &completed_transactions));
    return mp_obj_new_int_from_uint(pixels.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(fn_lcd_write_obj, fn_lcd_write);

/** 检测完整 RGB565 画布并返回由 C 固件记录的脏矩形列表。 */
static mp_obj_t fn_lcd_dirty_regions(size_t n_args, const mp_obj_t *args) {
    if (!fn_lcd_dma_is_initialized(&fn_lcd_context)) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("fn_lcd is not initialized"));
    }
    mp_buffer_info_t frame;
    mp_get_buffer_raise(args[0], &frame, MP_BUFFER_READ);
    const bool force = n_args > 1 && mp_obj_is_true(args[1]);
    size_t region_count = 0;
    check_esp_err(fn_lcd_dma_scan_dirty(
        &fn_lcd_context, frame.buf, frame.len, force, &region_count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t index = 0; index < region_count; ++index) {
        const fn_lcd_region_t *region = fn_lcd_dma_get_dirty_region(
            &fn_lcd_context, index);
        mp_obj_t values[4] = {
            mp_obj_new_int_from_uint(region->x),
            mp_obj_new_int_from_uint(region->y),
            mp_obj_new_int_from_uint(region->width),
            mp_obj_new_int_from_uint(region->height),
        };
        mp_obj_list_append(result, mp_obj_new_tuple(4, values));
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_lcd_dirty_regions_obj, 1, 2, fn_lcd_dirty_regions);

/** 从完整画布提取一个脏矩形并通过内部双条带、双 DMA 缓冲发送。 */
static mp_obj_t fn_lcd_write_region(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    if (!fn_lcd_dma_is_initialized(&fn_lcd_context)) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("fn_lcd is not initialized"));
    }
    mp_buffer_info_t frame;
    mp_get_buffer_raise(args[1], &frame, MP_BUFFER_READ);
    fn_lcd_region_t region = {
        .x = mp_obj_get_int(args[2]),
        .y = mp_obj_get_int(args[3]),
        .width = mp_obj_get_int(args[4]),
        .height = mp_obj_get_int(args[5]),
    };
    uint32_t completed_transactions = 0;
    spi_device_handle_t spi = machine_hw_spi_get_device(args[0]);
    esp_err_t result;
    MP_THREAD_GIL_EXIT();
    result = fn_lcd_dma_write_region(
        &fn_lcd_context,
        spi,
        frame.buf,
        frame.len,
        &region,
        &completed_transactions);
    MP_THREAD_GIL_ENTER();
    check_esp_err(result);
    return mp_obj_new_int_from_uint(
        (size_t)region.width * region.height * 2U);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    fn_lcd_write_region_obj, 6, 6, fn_lcd_write_region);

/** 提交已经完整发送的画布，令其成为下一帧脏区比较基线。 */
static mp_obj_t fn_lcd_commit_frame(void) {
    check_esp_err(fn_lcd_dma_commit_frame(&fn_lcd_context));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_commit_frame_obj, fn_lcd_commit_frame);

/** 放弃发送失败或被更新画布覆盖的待提交帧。 */
static mp_obj_t fn_lcd_discard_frame(void) {
    fn_lcd_dma_discard_frame(&fn_lcd_context);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_discard_frame_obj, fn_lcd_discard_frame);

/** 返回缓冲容量、屏幕方案和脏区发送累计统计。 */
static mp_obj_t fn_lcd_stats(void) {
    mp_obj_t result = mp_obj_new_dict(28);
    #define FN_LCD_STORE_UINT(name, value) mp_obj_dict_store(result, \
        MP_OBJ_NEW_QSTR(MP_QSTR_##name), mp_obj_new_int_from_ull(value))
    FN_LCD_STORE_UINT(chunk_size, fn_lcd_context.chunk_size);
    FN_LCD_STORE_UINT(strip_buffer_size, fn_lcd_context.strip_buffer_size);
    FN_LCD_STORE_UINT(width, fn_lcd_context.config.width);
    FN_LCD_STORE_UINT(height, fn_lcd_context.config.height);
    FN_LCD_STORE_UINT(strip_height, fn_lcd_context.config.strip_height);
    FN_LCD_STORE_UINT(tile_width, fn_lcd_context.config.tile_width);
    FN_LCD_STORE_UINT(tile_height, fn_lcd_context.config.tile_height);
    FN_LCD_STORE_UINT(spi_id, fn_lcd_context.config.spi_id);
    FN_LCD_STORE_UINT(sck, fn_lcd_context.config.sck);
    FN_LCD_STORE_UINT(mosi, fn_lcd_context.config.mosi);
    FN_LCD_STORE_UINT(cs, fn_lcd_context.config.cs);
    FN_LCD_STORE_UINT(dc, fn_lcd_context.config.dc);
    FN_LCD_STORE_UINT(rst, fn_lcd_context.config.rst);
    FN_LCD_STORE_UINT(backlight, fn_lcd_context.config.backlight);
    FN_LCD_STORE_UINT(baudrate, fn_lcd_context.config.baudrate);
    FN_LCD_STORE_UINT(write_count, fn_lcd_context.write_count);
    FN_LCD_STORE_UINT(byte_count, fn_lcd_context.byte_count);
    FN_LCD_STORE_UINT(transaction_count, fn_lcd_context.transaction_count);
    FN_LCD_STORE_UINT(scanned_frame_count, fn_lcd_context.scanned_frame_count);
    FN_LCD_STORE_UINT(committed_frame_count, fn_lcd_context.committed_frame_count);
    FN_LCD_STORE_UINT(unchanged_frame_count, fn_lcd_context.unchanged_frame_count);
    FN_LCD_STORE_UINT(dropped_frame_count, fn_lcd_context.dropped_frame_count);
    FN_LCD_STORE_UINT(dirty_region_count, fn_lcd_context.dirty_region_count);
    FN_LCD_STORE_UINT(sync_visible_frame_to_second,
        fn_lcd_context.config.sync_visible_frame_to_second);
    FN_LCD_STORE_UINT(synchronized_frame_count,
        fn_lcd_context.synchronized_frame_count);
    FN_LCD_STORE_UINT(last_sync_target_us,
        fn_lcd_context.last_sync_target_us);
    FN_LCD_STORE_UINT(last_sync_error_us,
        fn_lcd_context.last_sync_error_us);
    #undef FN_LCD_STORE_UINT
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_lcd_stats_obj, fn_lcd_stats);

static const mp_rom_map_elem_t fn_lcd_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_lcd) },
    { MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_lcd_api_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&fn_lcd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_visible_frame_second_sync),
        MP_ROM_PTR(&fn_lcd_set_visible_frame_second_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&fn_lcd_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&fn_lcd_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_dirty_regions), MP_ROM_PTR(&fn_lcd_dirty_regions_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_region), MP_ROM_PTR(&fn_lcd_write_region_obj) },
    { MP_ROM_QSTR(MP_QSTR_commit_frame), MP_ROM_PTR(&fn_lcd_commit_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_discard_frame), MP_ROM_PTR(&fn_lcd_discard_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&fn_lcd_stats_obj) },
};
static MP_DEFINE_CONST_DICT(fn_lcd_module_globals, fn_lcd_module_globals_table);

const mp_obj_module_t fn_lcd_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&fn_lcd_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_lcd, fn_lcd_user_cmodule);
