/*
 * fn-vision LCD 内部 DMA 双缓冲传输引擎。
 */

#ifndef FN_LCD_DMA_H
#define FN_LCD_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#define FN_LCD_DMA_BUFFER_COUNT (2)
#define FN_LCD_DMA_MAX_CHUNK_SIZE (4092)

typedef struct _fn_lcd_dma_context_t {
    uint8_t *buffers[FN_LCD_DMA_BUFFER_COUNT];
    size_t chunk_size;
    uint32_t write_count;
    uint64_t byte_count;
    uint32_t transaction_count;
} fn_lcd_dma_context_t;

/** 初始化内部 DMA 双缓冲，并返回规范化后的单块容量。 */
esp_err_t fn_lcd_dma_init(fn_lcd_dma_context_t *context,
    size_t requested_chunk_size, size_t *actual_chunk_size);

/** 释放内部 DMA 双缓冲及其累计状态。 */
void fn_lcd_dma_deinit(fn_lcd_dma_context_t *context);

/** 判断内部 DMA 双缓冲是否已经完成初始化。 */
bool fn_lcd_dma_is_initialized(const fn_lcd_dma_context_t *context);

/** 把像素数据分块复制到内部 RAM，并通过同一 SPI 设备排队发送。 */
esp_err_t fn_lcd_dma_write(fn_lcd_dma_context_t *context,
    spi_device_handle_t spi, const uint8_t *source, size_t length,
    uint32_t *completed_transactions);

#endif
