/*
 * ESP32-S3 固件内置数据 CDC 的 MicroPython 绑定。
 * 独立 FreeRTOS 任务持续推进 TinyUSB，并在 C 层把字节流组装为完整 PV1 帧。
 */

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "esp_heap_caps.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "shared/tinyusb/mp_usbd.h"
#include "shared/tinyusb/mp_usbd_cdc.h"

// usermod 可能在 TinyUSB 头文件首次展开后才注入板级 CDC 宏，因此在入口显式声明跨编译单元 API。
extern void mp_usbd_task_lock_enable(void);

#if MICROPY_HW_USB_CDC_DATA

#define USB_CDC_FRAME_QUEUE_DEPTH (4)
#define USB_CDC_MAX_PAYLOAD_SIZE (16 * 1024)
#define USB_CDC_MAX_FRAME_SIZE (USB_CDC_MAX_PAYLOAD_SIZE + 64)
#define USB_CDC_FRAME_WORK_SIZE (USB_CDC_MAX_FRAME_SIZE + 1)
#define USB_CDC_TASK_STACK_SIZE (4096)
#define USB_CDC_TASK_PRIORITY (ESP_TASK_PRIO_MIN + 2)
#define USB_CDC_PARTIAL_FRAME_TIMEOUT_MS (1000)

static const uint8_t usb_cdc_frame_magic[] = {'P', 'V', '1', ':'};

static uint8_t *usb_cdc_data_rx_buffer;
static uint8_t *usb_cdc_frame_work_buffer;
static uint8_t *usb_cdc_frame_queue_storage;
static size_t usb_cdc_frame_queue_lengths[USB_CDC_FRAME_QUEUE_DEPTH];
static size_t usb_cdc_frame_length;
static size_t usb_cdc_frame_expected_length;
static size_t usb_cdc_frame_separators[4];
static size_t usb_cdc_frame_sync_length;
static uint8_t usb_cdc_frame_separator_count;
static bool usb_cdc_frame_binary;
static bool usb_cdc_frame_header_complete;
static size_t usb_cdc_frame_queue_head;
static size_t usb_cdc_frame_queue_tail;
static size_t usb_cdc_frame_queue_count;
static TickType_t usb_cdc_frame_last_tick;
static size_t usb_cdc_session_generation;
static bool usb_cdc_frame_timeout_pending;
static volatile bool usb_cdc_data_ready;
static TaskHandle_t usb_cdc_rx_task_handle;
static StaticSemaphore_t usb_cdc_state_mutex_storage;
static SemaphoreHandle_t usb_cdc_state_mutex;

// 优先从 PSRAM 分配长期存活的 CDC 缓冲区，失败时回退到内部 RAM。
static void *usb_cdc_allocate(size_t size) {
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buffer;
}

// 重置当前帧的同步和头部解析状态。调用时必须持有状态互斥量。
static void usb_cdc_frame_reset_locked(void) {
    usb_cdc_frame_length = 0;
    usb_cdc_frame_expected_length = 0;
    usb_cdc_frame_sync_length = 0;
    usb_cdc_frame_separator_count = 0;
    usb_cdc_frame_binary = false;
    usb_cdc_frame_header_complete = false;
    usb_cdc_frame_last_tick = 0;
}

// 解析十进制载荷长度，拒绝空字段、非数字和超过固件上限的数值。
static bool usb_cdc_parse_payload_length(size_t start, size_t end, size_t *value) {
    if (start >= end) {
        return false;
    }
    size_t parsed = 0;
    for (size_t index = start; index < end; ++index) {
        uint8_t character = usb_cdc_frame_work_buffer[index];
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10 + (character - '0');
        if (parsed > USB_CDC_MAX_PAYLOAD_SIZE) {
            return false;
        }
    }
    *value = parsed;
    return true;
}

// 在第四个分隔符到达后解析 PV1 头，二进制帧据此计算 64 字节物理帧边界。
static void usb_cdc_parse_header_locked(void) {
    size_t payload_length = 0;
    size_t type_start = usb_cdc_frame_separators[0] + 1;
    size_t type_end = usb_cdc_frame_separators[1];
    usb_cdc_frame_binary = type_end - type_start == 5
        && memcmp(usb_cdc_frame_work_buffer + type_start, "JSONB", 5) == 0;
    usb_cdc_frame_header_complete = true;
    if (!usb_cdc_frame_binary || !usb_cdc_parse_payload_length(
        usb_cdc_frame_separators[1] + 1,
        usb_cdc_frame_separators[2],
        &payload_length
    )) {
        return;
    }
    size_t logical_length = usb_cdc_frame_separators[3] + 1 + payload_length + 1;
    size_t physical_length = (logical_length + 63) & ~(size_t)63;
    if (physical_length <= USB_CDC_MAX_FRAME_SIZE) {
        usb_cdc_frame_expected_length = physical_length;
    }
}

// 将已完成帧复制到有界队列。队列满后上游停止读取，由 USB NAK 提供无丢包背压。
static bool usb_cdc_enqueue_frame_locked(void) {
    if (usb_cdc_frame_queue_count >= USB_CDC_FRAME_QUEUE_DEPTH) {
        return false;
    }
    uint8_t *destination = usb_cdc_frame_queue_storage
        + usb_cdc_frame_queue_tail * USB_CDC_FRAME_WORK_SIZE;
    memcpy(destination, usb_cdc_frame_work_buffer, usb_cdc_frame_length);
    usb_cdc_frame_queue_lengths[usb_cdc_frame_queue_tail] = usb_cdc_frame_length;
    usb_cdc_frame_queue_tail = (usb_cdc_frame_queue_tail + 1) % USB_CDC_FRAME_QUEUE_DEPTH;
    ++usb_cdc_frame_queue_count;
    usb_cdc_frame_reset_locked();
    return true;
}

// 向 C 层 PV1 组帧器投递一个字节。文本帧按换行结束，JSONB 按头部长度和 64 字节边界结束。
static void usb_cdc_feed_byte_locked(uint8_t value) {
    if (usb_cdc_frame_length == 0) {
        if (value == usb_cdc_frame_magic[usb_cdc_frame_sync_length]) {
            ++usb_cdc_frame_sync_length;
        } else {
            usb_cdc_frame_sync_length = value == usb_cdc_frame_magic[0] ? 1 : 0;
        }
        if (usb_cdc_frame_sync_length == sizeof(usb_cdc_frame_magic)) {
            memcpy(usb_cdc_frame_work_buffer, usb_cdc_frame_magic, sizeof(usb_cdc_frame_magic));
            usb_cdc_frame_length = sizeof(usb_cdc_frame_magic);
            usb_cdc_frame_separators[0] = sizeof(usb_cdc_frame_magic) - 1;
            usb_cdc_frame_separator_count = 1;
            usb_cdc_frame_sync_length = 0;
        }
        if (usb_cdc_frame_sync_length > 0 || usb_cdc_frame_length > 0) {
            usb_cdc_frame_last_tick = xTaskGetTickCount();
        }
        return;
    }

    if (usb_cdc_frame_length >= USB_CDC_MAX_FRAME_SIZE) {
        usb_cdc_frame_reset_locked();
        return;
    }
    usb_cdc_frame_work_buffer[usb_cdc_frame_length++] = value;
    usb_cdc_frame_last_tick = xTaskGetTickCount();
    if (!usb_cdc_frame_header_complete && value == ':' && usb_cdc_frame_separator_count < 4) {
        usb_cdc_frame_separators[usb_cdc_frame_separator_count++] = usb_cdc_frame_length - 1;
        if (usb_cdc_frame_separator_count == 4) {
            usb_cdc_parse_header_locked();
        }
    }

    if (usb_cdc_frame_header_complete && usb_cdc_frame_binary
        && usb_cdc_frame_expected_length > 0
        && usb_cdc_frame_length == usb_cdc_frame_expected_length) {
        if (value != '\n') {
            // 伪造换行只用于把错误帧交给现有解析器，原帧尾仍会触发 BAD_FRAME_TRAILER。
            usb_cdc_frame_work_buffer[usb_cdc_frame_length++] = '\n';
        }
        usb_cdc_enqueue_frame_locked();
        return;
    }
    if (value == '\n' && (!usb_cdc_frame_binary || usb_cdc_frame_expected_length == 0)) {
        usb_cdc_enqueue_frame_locked();
    }
}

// 原始环形缓冲已排空且半帧超时时重新同步，并留下一次 Python 可读的错误事件。
static void usb_cdc_expire_partial_frame_locked(void) {
    if (usb_cdc_frame_length == 0 && usb_cdc_frame_sync_length == 0) {
        return;
    }
    TickType_t elapsed = xTaskGetTickCount() - usb_cdc_frame_last_tick;
    if (elapsed >= pdMS_TO_TICKS(USB_CDC_PARTIAL_FRAME_TIMEOUT_MS)) {
        usb_cdc_frame_reset_locked();
        usb_cdc_frame_timeout_pending = true;
    }
}

// 独立 CDC 接收任务：持续推进 TinyUSB，把原始环形缓冲数据转换为完整帧队列。
static void usb_cdc_rx_task(void *argument) {
    (void)argument;
    uint8_t chunk[64];
    size_t chunk_offset = 0;
    size_t chunk_length = 0;
    size_t observed_generation = 0;
    for (;;) {
        if (tusb_inited()) {
            mp_usbd_task();
        }
        if (usb_cdc_data_ready) {
            xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
            if (observed_generation != usb_cdc_session_generation) {
                chunk_offset = 0;
                chunk_length = 0;
                observed_generation = usb_cdc_session_generation;
            }
            bool raw_buffer_empty = false;
            while (usb_cdc_frame_queue_count < USB_CDC_FRAME_QUEUE_DEPTH) {
                if (chunk_offset >= chunk_length) {
                    chunk_length = mp_usbd_cdc_data_rx_read_buffered(chunk, sizeof(chunk));
                    chunk_offset = 0;
                    if (chunk_length == 0) {
                        raw_buffer_empty = true;
                        break;
                    }
                }
                usb_cdc_feed_byte_locked(chunk[chunk_offset++]);
            }
            if (raw_buffer_empty) {
                usb_cdc_expire_partial_frame_locked();
            }
            xSemaphoreGive(usb_cdc_state_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 初始化原始接收缓冲、完整帧队列和独立 FreeRTOS 任务。
static mp_obj_t usb_cdc_data_init(void) {
    if (usb_cdc_state_mutex == NULL) {
        usb_cdc_state_mutex = xSemaphoreCreateMutexStatic(&usb_cdc_state_mutex_storage);
    }
    if (usb_cdc_data_rx_buffer == NULL) {
        usb_cdc_data_rx_buffer = usb_cdc_allocate(MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE + 1);
    }
    if (usb_cdc_frame_work_buffer == NULL) {
        usb_cdc_frame_work_buffer = usb_cdc_allocate(USB_CDC_FRAME_WORK_SIZE);
    }
    if (usb_cdc_frame_queue_storage == NULL) {
        usb_cdc_frame_queue_storage = usb_cdc_allocate(
            USB_CDC_FRAME_QUEUE_DEPTH * USB_CDC_FRAME_WORK_SIZE
        );
    }
    if (usb_cdc_data_rx_buffer == NULL || usb_cdc_frame_work_buffer == NULL
        || usb_cdc_frame_queue_storage == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("USB CDC frame buffer allocation failed"));
    }

    usb_cdc_data_ready = false;
    xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
    usb_cdc_frame_queue_head = 0;
    usb_cdc_frame_queue_tail = 0;
    usb_cdc_frame_queue_count = 0;
    usb_cdc_frame_timeout_pending = false;
    ++usb_cdc_session_generation;
    usb_cdc_frame_reset_locked();
    mp_usbd_cdc_data_rx_configure(
        usb_cdc_data_rx_buffer,
        MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE + 1
    );
    xSemaphoreGive(usb_cdc_state_mutex);

    mp_usbd_task_lock_enable();
    usb_cdc_data_ready = true;
    if (usb_cdc_rx_task_handle == NULL) {
        BaseType_t created = xTaskCreatePinnedToCore(
            usb_cdc_rx_task,
            "fn_cdc_rx",
            USB_CDC_TASK_STACK_SIZE,
            NULL,
            USB_CDC_TASK_PRIORITY,
            &usb_cdc_rx_task_handle,
            // 与 MicroPython 固定在同一核，避免 TinyUSB 控制台回调跨核访问 VM 调度器。
            MP_TASK_COREID
        );
        if (created != pdPASS) {
            usb_cdc_rx_task_handle = NULL;
            usb_cdc_data_ready = false;
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("USB CDC RX task creation failed"));
        }
    }
    return MP_OBJ_NEW_SMALL_INT(MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_init_obj, usb_cdc_data_init);

// 返回 C 层完整帧队列中的待读帧数。
static mp_obj_t usb_cdc_data_frames_available(void) {
    size_t count;
    xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
    count = usb_cdc_frame_queue_count + (usb_cdc_frame_timeout_pending ? 1 : 0);
    xSemaphoreGive(usb_cdc_state_mutex);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_frames_available_obj, usb_cdc_data_frames_available);

// 取出 C 接收任务产生的异步协议错误，当前仅用于上报半帧超时。
static mp_obj_t usb_cdc_data_read_error(void) {
    bool timeout;
    xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
    timeout = usb_cdc_frame_timeout_pending;
    usb_cdc_frame_timeout_pending = false;
    xSemaphoreGive(usb_cdc_state_mutex);
    return timeout ? mp_obj_new_bytes((const byte *)"FRAME_TIMEOUT", 13) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_read_error_obj, usb_cdc_data_read_error);

// 复制并移除队首完整帧；队列为空时返回 None。
static mp_obj_t usb_cdc_data_read_frame(void) {
    size_t length = 0;
    uint8_t *source = NULL;
    xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
    if (usb_cdc_frame_queue_count > 0) {
        length = usb_cdc_frame_queue_lengths[usb_cdc_frame_queue_head];
        source = usb_cdc_frame_queue_storage
            + usb_cdc_frame_queue_head * USB_CDC_FRAME_WORK_SIZE;
    }
    xSemaphoreGive(usb_cdc_state_mutex);
    if (source == NULL) {
        return mp_const_none;
    }

    // 队首槽在移除前不会被后台任务覆盖，先创建 Python bytes 可避免 GC 异常遗留锁。
    mp_obj_t result = mp_obj_new_bytes(source, length);
    xSemaphoreTake(usb_cdc_state_mutex, portMAX_DELAY);
    if (usb_cdc_frame_queue_count > 0) {
        usb_cdc_frame_queue_head = (usb_cdc_frame_queue_head + 1) % USB_CDC_FRAME_QUEUE_DEPTH;
        --usb_cdc_frame_queue_count;
    }
    xSemaphoreGive(usb_cdc_state_mutex);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_read_frame_obj, usb_cdc_data_read_frame);

// 返回底层原始环形缓冲区字节数，仅供兼容诊断使用。
static mp_obj_t usb_cdc_data_any(void) {
    return mp_obj_new_int_from_uint(mp_usbd_cdc_data_rx_any());
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_any_obj, usb_cdc_data_any);

// 读取底层原始字节，新协议路径应使用 read_frame 避免与 C 组帧任务竞争。
static mp_obj_t usb_cdc_data_readinto(mp_obj_t buffer_in) {
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(buffer_in, &buffer, MP_BUFFER_WRITE);
    size_t count = mp_usbd_cdc_data_rx_read(buffer.buf, buffer.len);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(usb_cdc_data_readinto_obj, usb_cdc_data_readinto);

// 把调用方数据写入固件内置数据 CDC。
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

// 返回固件 CDC 绑定接口版本；版本 2 表示支持 C 层完整帧队列。
static mp_obj_t usb_cdc_data_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(2);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_cdc_data_api_version_obj, usb_cdc_data_api_version);

static const mp_rom_map_elem_t usb_cdc_data_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__usb_cdc_data) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&usb_cdc_data_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_frames_available), MP_ROM_PTR(&usb_cdc_data_frames_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_error), MP_ROM_PTR(&usb_cdc_data_read_error_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_frame), MP_ROM_PTR(&usb_cdc_data_read_frame_obj) },
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
