/*
 * fn-vision LCD 内部 DMA 双缓冲传输引擎。
 */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "fn_lcd_dma.h"

/** 把配置容量限制到 ESP32 machine.SPI 当前允许的单笔事务范围。 */
static size_t fn_lcd_dma_normalize_chunk_size(size_t requested_chunk_size) {
    size_t normalized = requested_chunk_size == 0
        ? FN_LCD_DMA_MAX_CHUNK_SIZE : requested_chunk_size;
    if (normalized > FN_LCD_DMA_MAX_CHUNK_SIZE) {
        normalized = FN_LCD_DMA_MAX_CHUNK_SIZE;
    }
    normalized &= ~(size_t)3U;
    return normalized < 4 ? 4 : normalized;
}

/** 等待最早完成的 SPI 事务，并把对应 DMA 缓冲标记为空闲。 */
static esp_err_t fn_lcd_dma_wait_one(spi_device_handle_t spi,
    bool in_flight[FN_LCD_DMA_BUFFER_COUNT], uint32_t *queued_count) {
    spi_transaction_t *completed = NULL;
    esp_err_t result = spi_device_get_trans_result(
        spi, &completed, portMAX_DELAY
    );
    if (result != ESP_OK) {
        return result;
    }
    size_t index = (size_t)(uintptr_t)completed->user;
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

/** 初始化内部 DMA 双缓冲，并返回规范化后的单块容量。 */
esp_err_t fn_lcd_dma_init(fn_lcd_dma_context_t *context,
    size_t requested_chunk_size, size_t *actual_chunk_size) {
    size_t chunk_size = fn_lcd_dma_normalize_chunk_size(
        requested_chunk_size
    );
    if (fn_lcd_dma_is_initialized(context)
        && context->chunk_size == chunk_size) {
        *actual_chunk_size = chunk_size;
        return ESP_OK;
    }
    fn_lcd_dma_deinit(context);
    for (size_t index = 0; index < FN_LCD_DMA_BUFFER_COUNT; ++index) {
        context->buffers[index] = heap_caps_malloc(
            chunk_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        );
        if (context->buffers[index] == NULL) {
            fn_lcd_dma_deinit(context);
            return ESP_ERR_NO_MEM;
        }
    }
    context->chunk_size = chunk_size;
    *actual_chunk_size = chunk_size;
    return ESP_OK;
}

/** 释放内部 DMA 双缓冲及其累计状态。 */
void fn_lcd_dma_deinit(fn_lcd_dma_context_t *context) {
    for (size_t index = 0; index < FN_LCD_DMA_BUFFER_COUNT; ++index) {
        if (context->buffers[index] != NULL) {
            heap_caps_free(context->buffers[index]);
            context->buffers[index] = NULL;
        }
    }
    context->chunk_size = 0;
    context->write_count = 0;
    context->byte_count = 0;
    context->transaction_count = 0;
}

/** 判断内部 DMA 双缓冲是否已经完成初始化。 */
bool fn_lcd_dma_is_initialized(const fn_lcd_dma_context_t *context) {
    return context->chunk_size > 0
        && context->buffers[0] != NULL
        && context->buffers[1] != NULL;
}

/** 把像素数据分块复制到内部 RAM，并通过同一 SPI 设备排队发送。 */
esp_err_t fn_lcd_dma_write(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *source, size_t length,
    uint32_t *completed_transactions) {
    if (!fn_lcd_dma_is_initialized(context) || spi == NULL
        || (source == NULL && length > 0)) {
        return ESP_ERR_INVALID_STATE;
    }
    *completed_transactions = 0;
    if (length == 0) {
        return ESP_OK;
    }

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
        size_t index = fn_lcd_dma_find_free_buffer(in_flight);
        if (index >= FN_LCD_DMA_BUFFER_COUNT) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t chunk_length = length - offset;
        if (chunk_length > context->chunk_size) {
            chunk_length = context->chunk_size;
        }
        memcpy(context->buffers[index], source + offset, chunk_length);
        memset(&transactions[index], 0, sizeof(spi_transaction_t));
        transactions[index].length = chunk_length * 8;
        transactions[index].tx_buffer = context->buffers[index];
        transactions[index].user = (void *)(uintptr_t)index;
        result = spi_device_queue_trans(
            spi, &transactions[index], portMAX_DELAY
        );
        if (result != ESP_OK) {
            break;
        }
        in_flight[index] = true;
        ++queued_count;
        offset += chunk_length;
    }

    while (queued_count > 0) {
        esp_err_t wait_result = fn_lcd_dma_wait_one(
            spi, in_flight, &queued_count
        );
        if (wait_result != ESP_OK) {
            if (result == ESP_OK) {
                result = wait_result;
            }
            break;
        }
        ++(*completed_transactions);
    }
    spi_device_release_bus(spi);

    if (result == ESP_OK) {
        ++context->write_count;
        context->byte_count += length;
        context->transaction_count += *completed_transactions;
    }
    return result;
}
