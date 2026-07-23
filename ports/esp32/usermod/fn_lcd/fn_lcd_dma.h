/*
 * fn-vision LCD 完整画布脏区检测与内部 SRAM 双缓冲传输引擎。
 */

#ifndef FN_LCD_DMA_H
#define FN_LCD_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FN_LCD_DMA_BUFFER_COUNT (2)
#define FN_LCD_STRIP_BUFFER_COUNT (2)
#define FN_LCD_ASYNC_FRAME_BUFFER_COUNT (2)
#define FN_LCD_DMA_MAX_CHUNK_SIZE (4092)
#define FN_LCD_DEFAULT_STRIP_HEIGHT (40)
#define FN_LCD_DEFAULT_TILE_WIDTH (16)
#define FN_LCD_DEFAULT_TILE_HEIGHT (8)
#define FN_LCD_SECOND_US (1000000LL)
#define FN_LCD_ASYNC_PREPARE_LEAD_US (30000LL)
#define FN_LCD_ASYNC_TASK_STACK_SIZE (6144)

typedef struct _fn_lcd_region_t {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} fn_lcd_region_t;

typedef struct _fn_lcd_config_t {
    uint16_t width;
    uint16_t height;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t strip_height;
    uint16_t tile_width;
    uint16_t tile_height;
    int16_t spi_id;
    int16_t sck;
    int16_t mosi;
    int16_t miso;
    int16_t cs;
    int16_t dc;
    int16_t rst;
    int16_t backlight;
    uint32_t baudrate;
    size_t dma_chunk_size;
    bool sync_visible_frame_to_second;
} fn_lcd_config_t;

typedef struct _fn_lcd_dma_context_t {
    uint8_t *dma_buffers[FN_LCD_DMA_BUFFER_COUNT];
    uint8_t *strip_buffers[FN_LCD_STRIP_BUFFER_COUNT];
    uint8_t *async_frame_buffers[FN_LCD_ASYNC_FRAME_BUFFER_COUNT];
    uint32_t *displayed_tile_hashes;
    uint32_t *pending_tile_hashes;
    fn_lcd_region_t *dirty_regions;
    fn_lcd_config_t config;
    size_t chunk_size;
    size_t strip_buffer_size;
    size_t frame_buffer_size;
    size_t tile_columns;
    size_t tile_rows;
    size_t tile_count;
    size_t dirty_region_count;
    uint8_t next_strip_buffer;
    int8_t async_pending_frame_index;
    int8_t async_active_frame_index;
    bool displayed_frame_valid;
    bool pending_frame_valid;
    bool async_pending_force;
    volatile bool async_stop_requested;
    volatile bool async_task_running;
    uint32_t write_count;
    uint64_t byte_count;
    uint32_t transaction_count;
    uint32_t scanned_frame_count;
    uint32_t committed_frame_count;
    uint32_t unchanged_frame_count;
    uint32_t dropped_frame_count;
    uint32_t synchronized_frame_count;
    uint32_t async_queued_frame_count;
    uint32_t async_replaced_frame_count;
    uint32_t async_error_count;
    int64_t last_sync_target_us;
    int32_t last_sync_error_us;
    spi_device_handle_t async_pending_spi;
    SemaphoreHandle_t async_state_mutex;
    TaskHandle_t async_task;
} fn_lcd_dma_context_t;

/** 按屏幕、脚位和分块方案初始化全部固件侧显示缓冲。 */
esp_err_t fn_lcd_dma_init(fn_lcd_dma_context_t *context,
    const fn_lcd_config_t *config, size_t *actual_chunk_size);

/** 释放固件侧 DMA、条带、哈希和脏区记录缓冲。 */
void fn_lcd_dma_deinit(fn_lcd_dma_context_t *context);

/** 判断完整画布传输引擎是否已经完成初始化。 */
bool fn_lcd_dma_is_initialized(const fn_lcd_dma_context_t *context);

/** 更新后续可见帧是否由原生 DMA 层自动对齐下一整秒。 */
bool fn_lcd_dma_set_visible_frame_second_sync(
    fn_lcd_dma_context_t *context, bool enabled);

/** 把最新完整画布复制进原生单槽邮箱，等待整秒任务自动提交。 */
esp_err_t fn_lcd_dma_queue_synchronized_frame(
    fn_lcd_dma_context_t *context, spi_device_handle_t spi,
    const uint8_t *frame, size_t frame_length, bool force,
    uint32_t *queued_sequence);

/** 兼容旧局部刷新接口，直接通过 DMA 双缓冲发送连续像素。 */
esp_err_t fn_lcd_dma_write(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *source, size_t length,
    uint32_t *completed_transactions);

/** 检测完整画布变化并记录合并后的脏矩形。 */
esp_err_t fn_lcd_dma_scan_dirty(fn_lcd_dma_context_t *context,
    const uint8_t *frame, size_t frame_length, bool force,
    size_t *region_count);

/** 返回指定序号的待提交脏矩形。 */
const fn_lcd_region_t *fn_lcd_dma_get_dirty_region(
    const fn_lcd_dma_context_t *context, size_t index);

/** 使用双条带缓冲交替整理矩形像素，再经 DMA 双缓冲发送。 */
esp_err_t fn_lcd_dma_write_region(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *frame, size_t frame_length,
    const fn_lcd_region_t *region, uint32_t *completed_transactions);

/** 在全部脏区成功发送后提交本帧哈希基线。 */
esp_err_t fn_lcd_dma_commit_frame(fn_lcd_dma_context_t *context);

/** 放弃尚未完整发送的帧，并累计丢帧统计。 */
void fn_lcd_dma_discard_frame(fn_lcd_dma_context_t *context);

#endif
