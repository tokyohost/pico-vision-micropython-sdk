/*
 * fn-vision LCD 完整画布脏区检测与内部 SRAM 双缓冲传输引擎。
 */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fn_lcd_dma.h"

/** 把 DMA 分块容量限制到 machine.SPI 当前允许的单笔事务范围。 */
static size_t fn_lcd_dma_normalize_chunk_size(size_t requested_chunk_size) {
    size_t normalized = requested_chunk_size == 0
        ? FN_LCD_DMA_MAX_CHUNK_SIZE : requested_chunk_size;
    if (normalized > FN_LCD_DMA_MAX_CHUNK_SIZE) {
        normalized = FN_LCD_DMA_MAX_CHUNK_SIZE;
    }
    normalized &= ~(size_t)3U;
    return normalized < 4 ? 4 : normalized;
}

/** 使用确定性的三十二位 FNV-1a 计算一个非连续像素瓦片的内容摘要。 */
static uint32_t fn_lcd_hash_tile(const uint8_t *frame,
    const fn_lcd_dma_context_t *context, size_t tile_x, size_t tile_y) {
    const size_t x = tile_x * context->config.tile_width;
    const size_t y = tile_y * context->config.tile_height;
    size_t width = context->config.width - x;
    size_t height = context->config.height - y;
    if (width > context->config.tile_width) {
        width = context->config.tile_width;
    }
    if (height > context->config.tile_height) {
        height = context->config.tile_height;
    }
    uint32_t hash = 2166136261U;
    const size_t frame_stride = (size_t)context->config.width * 2U;
    for (size_t row = 0; row < height; ++row) {
        const uint8_t *pixel = frame + (y + row) * frame_stride + x * 2U;
        for (size_t byte_index = 0; byte_index < width * 2U; ++byte_index) {
            hash ^= pixel[byte_index];
            hash *= 16777619U;
        }
    }
    return hash;
}

/** 等待最早完成的 SPI 事务，并把对应 DMA 缓冲标记为空闲。 */
static esp_err_t fn_lcd_dma_wait_one(spi_device_handle_t spi,
    bool in_flight[FN_LCD_DMA_BUFFER_COUNT], uint32_t *queued_count) {
    spi_transaction_t *completed = NULL;
    esp_err_t result = spi_device_get_trans_result(spi, &completed, portMAX_DELAY);
    if (result != ESP_OK) {
        return result;
    }
    const size_t index = (size_t)(uintptr_t)completed->user;
    if (index >= FN_LCD_DMA_BUFFER_COUNT || !in_flight[index]) {
        return ESP_ERR_INVALID_STATE;
    }
    in_flight[index] = false;
    --(*queued_count);
    return ESP_OK;
}

/** 查找当前未被 SPI DMA 使用的内部缓冲区编号。 */
static size_t fn_lcd_dma_find_free_buffer(
    const bool in_flight[FN_LCD_DMA_BUFFER_COUNT]) {
    for (size_t index = 0; index < FN_LCD_DMA_BUFFER_COUNT; ++index) {
        if (!in_flight[index]) {
            return index;
        }
    }
    return FN_LCD_DMA_BUFFER_COUNT;
}

/** 使用 FreeRTOS 粗等待和微秒忙等待对齐下一单调时钟整秒。 */
static void fn_lcd_dma_wait_next_second(fn_lcd_dma_context_t *context) {
    const int64_t now_us = esp_timer_get_time();
    const int64_t target_us = (
        now_us / FN_LCD_SECOND_US + 1LL
    ) * FN_LCD_SECOND_US;
    int64_t remaining_us = target_us - now_us;
    while (remaining_us > 2000LL) {
        const TickType_t delay_ticks = pdMS_TO_TICKS(
            (uint32_t)((remaining_us - 1000LL) / 1000LL)
        );
        if (delay_ticks > 0) {
            vTaskDelay(delay_ticks);
        }
        remaining_us = target_us - esp_timer_get_time();
    }
    while ((remaining_us = target_us - esp_timer_get_time()) > 0) {
        esp_rom_delay_us((uint32_t)(
            remaining_us > 100LL ? 100LL : remaining_us
        ));
    }
    context->pending_frame_sync_started = true;
    context->last_sync_target_us = target_us;
}

/** 把一块连续条带通过内部 DMA 双缓冲排队发送。 */
static esp_err_t fn_lcd_dma_write_contiguous(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *source, size_t length,
    uint32_t *completed_transactions, bool synchronize_frame_start) {
    spi_transaction_t transactions[FN_LCD_DMA_BUFFER_COUNT];
    bool in_flight[FN_LCD_DMA_BUFFER_COUNT] = {false, false};
    uint32_t queued_count = 0;
    size_t offset = 0;
    esp_err_t result = spi_device_acquire_bus(spi, portMAX_DELAY);
    if (result != ESP_OK) {
        return result;
    }
    while (offset < length && result == ESP_OK) {
        if (queued_count == FN_LCD_DMA_BUFFER_COUNT) {
            result = fn_lcd_dma_wait_one(spi, in_flight, &queued_count);
            if (result != ESP_OK) {
                break;
            }
            ++(*completed_transactions);
        }
        const size_t index = fn_lcd_dma_find_free_buffer(in_flight);
        if (index >= FN_LCD_DMA_BUFFER_COUNT) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t chunk_length = length - offset;
        if (chunk_length > context->chunk_size) {
            chunk_length = context->chunk_size;
        }
        memcpy(context->dma_buffers[index], source + offset, chunk_length);
        memset(&transactions[index], 0, sizeof(spi_transaction_t));
        transactions[index].length = chunk_length * 8U;
        transactions[index].tx_buffer = context->dma_buffers[index];
        transactions[index].user = (void *)(uintptr_t)index;
        if (synchronize_frame_start && offset == 0) {
            fn_lcd_dma_wait_next_second(context);
        }
        result = spi_device_queue_trans(spi, &transactions[index], portMAX_DELAY);
        if (result != ESP_OK) {
            break;
        }
        if (synchronize_frame_start && offset == 0) {
            ++context->synchronized_frame_count;
            const int64_t error_us = esp_timer_get_time()
                - context->last_sync_target_us;
            context->last_sync_error_us = error_us > INT32_MAX
                ? INT32_MAX : (int32_t)error_us;
        }
        in_flight[index] = true;
        ++queued_count;
        offset += chunk_length;
    }
    while (queued_count > 0) {
        const esp_err_t wait_result = fn_lcd_dma_wait_one(
            spi, in_flight, &queued_count);
        if (wait_result != ESP_OK) {
            if (result == ESP_OK) {
                result = wait_result;
            }
            break;
        }
        ++(*completed_transactions);
    }
    spi_device_release_bus(spi);
    return result;
}

/** 兼容旧局部刷新接口，直接通过 DMA 双缓冲发送连续像素。 */
esp_err_t fn_lcd_dma_write(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *source, size_t length,
    uint32_t *completed_transactions) {
    if (!fn_lcd_dma_is_initialized(context) || spi == NULL
        || (source == NULL && length > 0) || completed_transactions == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *completed_transactions = 0;
    const esp_err_t result = fn_lcd_dma_write_contiguous(
        context, spi, source, length, completed_transactions, false);
    if (result == ESP_OK) {
        ++context->write_count;
        context->byte_count += length;
        context->transaction_count += *completed_transactions;
    }
    return result;
}

/** 释放可能位于内部 SRAM 或 PSRAM 的单个能力分配缓冲。 */
static void fn_lcd_free_buffer(void **buffer) {
    if (*buffer != NULL) {
        heap_caps_free(*buffer);
        *buffer = NULL;
    }
}

/** 按屏幕、脚位和分块方案初始化全部固件侧显示缓冲。 */
esp_err_t fn_lcd_dma_init(fn_lcd_dma_context_t *context,
    const fn_lcd_config_t *config, size_t *actual_chunk_size) {
    if (config == NULL || config->width == 0 || config->height == 0
        || config->strip_height == 0 || config->tile_width == 0
        || config->tile_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    fn_lcd_dma_deinit(context);
    context->config = *config;
    context->chunk_size = fn_lcd_dma_normalize_chunk_size(
        config->dma_chunk_size);
    context->config.dma_chunk_size = context->chunk_size;
    context->strip_buffer_size = (size_t)config->width
        * config->strip_height * 2U;
    context->tile_columns = (config->width + config->tile_width - 1U)
        / config->tile_width;
    context->tile_rows = (config->height + config->tile_height - 1U)
        / config->tile_height;
    context->tile_count = context->tile_columns * context->tile_rows;
    for (size_t index = 0; index < FN_LCD_DMA_BUFFER_COUNT; ++index) {
        context->dma_buffers[index] = heap_caps_malloc(
            context->chunk_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (context->dma_buffers[index] == NULL) {
            fn_lcd_dma_deinit(context);
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t index = 0; index < FN_LCD_STRIP_BUFFER_COUNT; ++index) {
        context->strip_buffers[index] = heap_caps_malloc(
            context->strip_buffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (context->strip_buffers[index] == NULL) {
            fn_lcd_dma_deinit(context);
            return ESP_ERR_NO_MEM;
        }
    }
    const size_t hash_bytes = context->tile_count * sizeof(uint32_t);
    context->displayed_tile_hashes = heap_caps_calloc(
        1, hash_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    context->pending_tile_hashes = heap_caps_calloc(
        1, hash_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    context->dirty_regions = heap_caps_calloc(
        context->tile_count, sizeof(fn_lcd_region_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context->displayed_tile_hashes == NULL
        || context->pending_tile_hashes == NULL
        || context->dirty_regions == NULL) {
        fn_lcd_dma_deinit(context);
        return ESP_ERR_NO_MEM;
    }
    *actual_chunk_size = context->chunk_size;
    return ESP_OK;
}

/** 释放固件侧 DMA、条带、哈希和脏区记录缓冲。 */
void fn_lcd_dma_deinit(fn_lcd_dma_context_t *context) {
    for (size_t index = 0; index < FN_LCD_DMA_BUFFER_COUNT; ++index) {
        fn_lcd_free_buffer((void **)&context->dma_buffers[index]);
    }
    for (size_t index = 0; index < FN_LCD_STRIP_BUFFER_COUNT; ++index) {
        fn_lcd_free_buffer((void **)&context->strip_buffers[index]);
    }
    fn_lcd_free_buffer((void **)&context->displayed_tile_hashes);
    fn_lcd_free_buffer((void **)&context->pending_tile_hashes);
    fn_lcd_free_buffer((void **)&context->dirty_regions);
    memset(context, 0, sizeof(*context));
}

/** 判断完整画布传输引擎是否已经完成初始化。 */
bool fn_lcd_dma_is_initialized(const fn_lcd_dma_context_t *context) {
    return context->chunk_size > 0 && context->dma_buffers[0] != NULL
        && context->dma_buffers[1] != NULL && context->strip_buffers[0] != NULL
        && context->strip_buffers[1] != NULL
        && context->displayed_tile_hashes != NULL
        && context->pending_tile_hashes != NULL
        && context->dirty_regions != NULL;
}

/** 更新后续可见帧是否由原生 DMA 层自动对齐下一整秒。 */
bool fn_lcd_dma_set_visible_frame_second_sync(
    fn_lcd_dma_context_t *context, bool enabled) {
    const bool changed =
        context->config.sync_visible_frame_to_second != enabled;
    context->config.sync_visible_frame_to_second = enabled;
    return changed;
}

/** 尝试把当前瓦片行脏区与上一行同位置矩形进行纵向合并。 */
static bool fn_lcd_merge_vertical_region(fn_lcd_dma_context_t *context,
    const fn_lcd_region_t *region) {
    for (size_t index = context->dirty_region_count; index > 0; --index) {
        fn_lcd_region_t *previous = &context->dirty_regions[index - 1U];
        if (previous->y + previous->height < region->y) {
            break;
        }
        if (previous->x == region->x && previous->width == region->width
            && previous->y + previous->height == region->y) {
            previous->height += region->height;
            return true;
        }
    }
    return false;
}

/** 检测完整画布变化并记录合并后的脏矩形。 */
esp_err_t fn_lcd_dma_scan_dirty(fn_lcd_dma_context_t *context,
    const uint8_t *frame, size_t frame_length, bool force,
    size_t *region_count) {
    const size_t expected_length = (size_t)context->config.width
        * context->config.height * 2U;
    if (!fn_lcd_dma_is_initialized(context) || frame == NULL
        || frame_length < expected_length || region_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (context->pending_frame_valid) {
        fn_lcd_dma_discard_frame(context);
    }
    context->dirty_region_count = 0;
    ++context->scanned_frame_count;
    for (size_t tile_y = 0; tile_y < context->tile_rows; ++tile_y) {
        size_t run_start = context->tile_columns;
        for (size_t tile_x = 0; tile_x <= context->tile_columns; ++tile_x) {
            bool dirty = false;
            if (tile_x < context->tile_columns) {
                const size_t tile_index = tile_y * context->tile_columns + tile_x;
                const uint32_t hash = fn_lcd_hash_tile(
                    frame, context, tile_x, tile_y);
                context->pending_tile_hashes[tile_index] = hash;
                dirty = force || !context->displayed_frame_valid
                    || hash != context->displayed_tile_hashes[tile_index];
            }
            if (dirty && run_start == context->tile_columns) {
                run_start = tile_x;
                continue;
            }
            if (dirty || run_start == context->tile_columns) {
                continue;
            }
            fn_lcd_region_t region = {
                .x = run_start * context->config.tile_width,
                .y = tile_y * context->config.tile_height,
                .width = (tile_x - run_start) * context->config.tile_width,
                .height = context->config.tile_height,
            };
            if (region.x + region.width > context->config.width) {
                region.width = context->config.width - region.x;
            }
            if (region.y + region.height > context->config.height) {
                region.height = context->config.height - region.y;
            }
            if (!fn_lcd_merge_vertical_region(context, &region)) {
                context->dirty_regions[context->dirty_region_count++] = region;
            }
            run_start = context->tile_columns;
        }
    }
    context->pending_frame_valid = true;
    context->pending_frame_sync_started = false;
    if (context->dirty_region_count == 0) {
        ++context->unchanged_frame_count;
    }
    *region_count = context->dirty_region_count;
    return ESP_OK;
}

/** 返回指定序号的待提交脏矩形。 */
const fn_lcd_region_t *fn_lcd_dma_get_dirty_region(
    const fn_lcd_dma_context_t *context, size_t index) {
    if (!context->pending_frame_valid || index >= context->dirty_region_count) {
        return NULL;
    }
    return &context->dirty_regions[index];
}

/** 使用双条带缓冲交替整理矩形像素，再经 DMA 双缓冲发送。 */
esp_err_t fn_lcd_dma_write_region(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *frame, size_t frame_length,
    const fn_lcd_region_t *region, uint32_t *completed_transactions) {
    const size_t expected_length = (size_t)context->config.width
        * context->config.height * 2U;
    if (!fn_lcd_dma_is_initialized(context) || !context->pending_frame_valid
        || spi == NULL || frame == NULL || frame_length < expected_length
        || region == NULL || completed_transactions == NULL
        || region->width == 0 || region->height == 0
        || region->x + region->width > context->config.width
        || region->y + region->height > context->config.height) {
        return ESP_ERR_INVALID_ARG;
    }
    *completed_transactions = 0;
    const size_t frame_stride = (size_t)context->config.width * 2U;
    const size_t region_stride = (size_t)region->width * 2U;
    size_t row_offset = 0;
    while (row_offset < region->height) {
        size_t rows = region->height - row_offset;
        if (rows > context->config.strip_height) {
            rows = context->config.strip_height;
        }
        uint8_t *strip = context->strip_buffers[context->next_strip_buffer];
        context->next_strip_buffer = (context->next_strip_buffer + 1U)
            % FN_LCD_STRIP_BUFFER_COUNT;
        for (size_t row = 0; row < rows; ++row) {
            const uint8_t *source = frame
                + (region->y + row_offset + row) * frame_stride
                + (size_t)region->x * 2U;
            memcpy(strip + row * region_stride, source, region_stride);
        }
        const size_t byte_count = rows * region_stride;
        const bool synchronize_frame_start =
            context->config.sync_visible_frame_to_second
            && !context->pending_frame_sync_started;
        esp_err_t result = fn_lcd_dma_write_contiguous(
            context, spi, strip, byte_count, completed_transactions,
            synchronize_frame_start);
        if (result != ESP_OK) {
            return result;
        }
        ++context->write_count;
        context->byte_count += byte_count;
        row_offset += rows;
    }
    context->transaction_count += *completed_transactions;
    return ESP_OK;
}

/** 在全部脏区成功发送后提交本帧哈希基线。 */
esp_err_t fn_lcd_dma_commit_frame(fn_lcd_dma_context_t *context) {
    if (!fn_lcd_dma_is_initialized(context) || !context->pending_frame_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t *temporary = context->displayed_tile_hashes;
    context->displayed_tile_hashes = context->pending_tile_hashes;
    context->pending_tile_hashes = temporary;
    context->displayed_frame_valid = true;
    context->pending_frame_valid = false;
    context->pending_frame_sync_started = false;
    context->dirty_region_count = 0;
    ++context->committed_frame_count;
    return ESP_OK;
}

/** 放弃尚未完整发送的帧，并累计丢帧统计。 */
void fn_lcd_dma_discard_frame(fn_lcd_dma_context_t *context) {
    if (context->pending_frame_valid) {
        context->pending_frame_valid = false;
        context->pending_frame_sync_started = false;
        context->dirty_region_count = 0;
        ++context->dropped_frame_count;
    }
}
