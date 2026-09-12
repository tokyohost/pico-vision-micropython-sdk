/**
 * @content ESP32-S3 WebSocket 原生收发数据面
 * @author xuehui_li
 * @version 1.0
 * @date 2026-09-12 22:00
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "py/runtime.h"

#define FN_WEBSOCKET_API_VERSION (1)
#define FN_WEBSOCKET_FRAME_QUEUE_DEPTH (4)
#define FN_WEBSOCKET_MAX_MESSAGE_SIZE (16 * 1024 + 64)
#define FN_WEBSOCKET_RX_CHUNK_SIZE (2048)
#define FN_WEBSOCKET_TASK_STACK_SIZE (4096)
#define FN_WEBSOCKET_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define FN_WEBSOCKET_SELECT_TIMEOUT_US (20000)
#define FN_WEBSOCKET_CONTROL_PAYLOAD_SIZE (125)

typedef enum {
    FN_WS_HEADER_FIRST,
    FN_WS_HEADER_SECOND,
    FN_WS_EXTENDED_LENGTH,
    FN_WS_MASK,
    FN_WS_PAYLOAD,
} fn_websocket_parse_state_t;

static uint8_t *fn_websocket_message_buffer;
static uint8_t *fn_websocket_queue_storage;
static size_t fn_websocket_queue_lengths[FN_WEBSOCKET_FRAME_QUEUE_DEPTH];
static size_t fn_websocket_queue_head;
static size_t fn_websocket_queue_tail;
static size_t fn_websocket_queue_count;
static size_t fn_websocket_message_length;
static size_t fn_websocket_payload_length;
static size_t fn_websocket_payload_received;
static uint8_t fn_websocket_mask[4];
static uint8_t fn_websocket_mask_received;
static uint8_t fn_websocket_extended_length_bytes;
static uint8_t fn_websocket_extended_length_received;
static uint8_t fn_websocket_opcode;
static uint8_t fn_websocket_fragment_opcode;
static bool fn_websocket_final;
static bool fn_websocket_masked;
static fn_websocket_parse_state_t fn_websocket_parse_state;
static uint8_t fn_websocket_control_payload[FN_WEBSOCKET_CONTROL_PAYLOAD_SIZE];
static size_t fn_websocket_control_length;
static uint8_t fn_websocket_pending_control_opcode;
static uint8_t fn_websocket_pending_control_payload[FN_WEBSOCKET_CONTROL_PAYLOAD_SIZE];
static size_t fn_websocket_pending_control_length;
static int fn_websocket_fd = -1;
static uint32_t fn_websocket_generation;
static uint32_t fn_websocket_receive_activity;
static bool fn_websocket_error_pending;
static char fn_websocket_error[32];
static TaskHandle_t fn_websocket_task_handle;
static StaticSemaphore_t fn_websocket_state_mutex_storage;
static StaticSemaphore_t fn_websocket_send_mutex_storage;
static SemaphoreHandle_t fn_websocket_state_mutex;
static SemaphoreHandle_t fn_websocket_send_mutex;

/** 优先从 PSRAM 分配长期帧缓冲，失败时回退到内部 SRAM。 */
static void *fn_websocket_allocate(size_t size) {
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buffer;
}

/** 重置当前 WebSocket 帧解析状态，保留已开始的分片消息。 */
static void fn_websocket_reset_frame_locked(void) {
    fn_websocket_parse_state = FN_WS_HEADER_FIRST;
    fn_websocket_payload_length = 0;
    fn_websocket_payload_received = 0;
    fn_websocket_mask_received = 0;
    fn_websocket_extended_length_bytes = 0;
    fn_websocket_extended_length_received = 0;
    fn_websocket_opcode = 0;
    fn_websocket_final = false;
    fn_websocket_masked = false;
    fn_websocket_control_length = 0;
}

/** 重置整个连接的解析、队列和错误状态。 */
static void fn_websocket_reset_session_locked(void) {
    fn_websocket_queue_head = 0;
    fn_websocket_queue_tail = 0;
    fn_websocket_queue_count = 0;
    fn_websocket_message_length = 0;
    fn_websocket_fragment_opcode = 0;
    fn_websocket_pending_control_opcode = 0;
    fn_websocket_pending_control_length = 0;
    fn_websocket_error_pending = false;
    fn_websocket_error[0] = '\0';
    fn_websocket_receive_activity = 0;
    fn_websocket_reset_frame_locked();
}

/** 记录一次原生接收错误，并使 Python 层在下一轮关闭会话。 */
static void fn_websocket_fail_locked(const char *error) {
    strncpy(fn_websocket_error, error, sizeof(fn_websocket_error) - 1);
    fn_websocket_error[sizeof(fn_websocket_error) - 1] = '\0';
    fn_websocket_error_pending = true;
    fn_websocket_fd = -1;
    ++fn_websocket_generation;
}

/** 将完整业务消息复制到固定深度队列。 */
static bool fn_websocket_enqueue_message_locked(void) {
    if (fn_websocket_queue_count >= FN_WEBSOCKET_FRAME_QUEUE_DEPTH) {
        return false;
    }
    uint8_t *destination = fn_websocket_queue_storage
        + fn_websocket_queue_tail * FN_WEBSOCKET_MAX_MESSAGE_SIZE;
    memcpy(destination, fn_websocket_message_buffer, fn_websocket_message_length);
    fn_websocket_queue_lengths[fn_websocket_queue_tail] = fn_websocket_message_length;
    fn_websocket_queue_tail =
        (fn_websocket_queue_tail + 1) % FN_WEBSOCKET_FRAME_QUEUE_DEPTH;
    ++fn_websocket_queue_count;
    fn_websocket_message_length = 0;
    fn_websocket_fragment_opcode = 0;
    return true;
}

/** 判断当前帧是否属于控制帧。 */
static bool fn_websocket_is_control_frame(void) {
    return (fn_websocket_opcode & 0x08U) != 0;
}

/** 完成当前帧并投递消息或安排控制帧响应。 */
static bool fn_websocket_finish_frame_locked(void) {
    if (fn_websocket_is_control_frame()) {
        if (fn_websocket_opcode == 0x08U) {
            fn_websocket_pending_control_opcode = 0x08U;
            fn_websocket_pending_control_length = fn_websocket_control_length;
            memcpy(fn_websocket_pending_control_payload,
                fn_websocket_control_payload, fn_websocket_control_length);
        } else if (fn_websocket_opcode == 0x09U) {
            fn_websocket_pending_control_opcode = 0x0AU;
            fn_websocket_pending_control_length = fn_websocket_control_length;
            memcpy(fn_websocket_pending_control_payload,
                fn_websocket_control_payload, fn_websocket_control_length);
        }
        fn_websocket_reset_frame_locked();
        return true;
    }

    if (fn_websocket_opcode != 0x00U) {
        fn_websocket_fragment_opcode = fn_websocket_opcode;
    }
    if (fn_websocket_final) {
        if (!fn_websocket_enqueue_message_locked()) {
            return false;
        }
    }
    fn_websocket_reset_frame_locked();
    return true;
}

/** 校验帧头并进入掩码或载荷读取阶段。 */
static bool fn_websocket_prepare_payload_locked(void) {
    if (!fn_websocket_masked) {
        fn_websocket_fail_locked("WEBSOCKET_UNMASKED_FRAME");
        return false;
    }
    if (fn_websocket_is_control_frame()
        && (!fn_websocket_final
            || fn_websocket_payload_length > FN_WEBSOCKET_CONTROL_PAYLOAD_SIZE)) {
        fn_websocket_fail_locked("WEBSOCKET_BAD_CONTROL");
        return false;
    }
    if (!fn_websocket_is_control_frame()) {
        if (fn_websocket_opcode == 0x00U) {
            if (fn_websocket_fragment_opcode == 0) {
                fn_websocket_fail_locked("WEBSOCKET_BAD_CONTINUATION");
                return false;
            }
        } else if (fn_websocket_opcode != 0x01U && fn_websocket_opcode != 0x02U) {
            fn_websocket_fail_locked("WEBSOCKET_BAD_OPCODE");
            return false;
        } else if (fn_websocket_fragment_opcode != 0) {
            fn_websocket_fail_locked("WEBSOCKET_FRAGMENT_ACTIVE");
            return false;
        }
        if (fn_websocket_payload_length
            > FN_WEBSOCKET_MAX_MESSAGE_SIZE - fn_websocket_message_length) {
            fn_websocket_fail_locked("WEBSOCKET_FRAME_TOO_LARGE");
            return false;
        }
    }
    fn_websocket_parse_state = FN_WS_MASK;
    return true;
}

/** 向解析器投递一个网络字节；队列满或协议错误时返回 false。 */
static bool fn_websocket_feed_byte_locked(uint8_t value) {
    switch (fn_websocket_parse_state) {
        case FN_WS_HEADER_FIRST:
            if ((value & 0x70U) != 0) {
                fn_websocket_fail_locked("WEBSOCKET_RSV_UNSUPPORTED");
                return false;
            }
            fn_websocket_final = (value & 0x80U) != 0;
            fn_websocket_opcode = value & 0x0FU;
            fn_websocket_parse_state = FN_WS_HEADER_SECOND;
            return true;
        case FN_WS_HEADER_SECOND: {
            fn_websocket_masked = (value & 0x80U) != 0;
            uint8_t short_length = value & 0x7FU;
            if (short_length < 126U) {
                fn_websocket_payload_length = short_length;
                return fn_websocket_prepare_payload_locked();
            }
            fn_websocket_extended_length_bytes = short_length == 126U ? 2U : 8U;
            fn_websocket_payload_length = 0;
            fn_websocket_parse_state = FN_WS_EXTENDED_LENGTH;
            return true;
        }
        case FN_WS_EXTENDED_LENGTH:
            if (fn_websocket_extended_length_received == 0
                && fn_websocket_extended_length_bytes == 8U && (value & 0x80U) != 0) {
                fn_websocket_fail_locked("WEBSOCKET_BAD_LENGTH");
                return false;
            }
            if (fn_websocket_payload_length > (SIZE_MAX >> 8U)) {
                fn_websocket_fail_locked("WEBSOCKET_BAD_LENGTH");
                return false;
            }
            fn_websocket_payload_length = (fn_websocket_payload_length << 8U) | value;
            ++fn_websocket_extended_length_received;
            if (fn_websocket_extended_length_received
                == fn_websocket_extended_length_bytes) {
                return fn_websocket_prepare_payload_locked();
            }
            return true;
        case FN_WS_MASK:
            fn_websocket_mask[fn_websocket_mask_received++] = value;
            if (fn_websocket_mask_received == sizeof(fn_websocket_mask)) {
                fn_websocket_parse_state = FN_WS_PAYLOAD;
                if (fn_websocket_payload_length == 0) {
                    return fn_websocket_finish_frame_locked();
                }
            }
            return true;
        case FN_WS_PAYLOAD: {
            uint8_t decoded = value
                ^ fn_websocket_mask[fn_websocket_payload_received & 3U];
            if (fn_websocket_is_control_frame()) {
                fn_websocket_control_payload[fn_websocket_control_length++] = decoded;
            } else {
                fn_websocket_message_buffer[fn_websocket_message_length++] = decoded;
            }
            ++fn_websocket_payload_received;
            if (fn_websocket_payload_received == fn_websocket_payload_length) {
                return fn_websocket_finish_frame_locked();
            }
            return true;
        }
        default:
            fn_websocket_fail_locked("WEBSOCKET_PARSER_STATE");
            return false;
    }
}

/** 在发送互斥保护下完整写出一段 WebSocket 线数据。 */
static bool fn_websocket_send_all(int fd, const uint8_t *data, size_t length) {
    size_t offset = 0;
    size_t retry_count = 0;
    while (offset < length) {
        int written = send(fd, data + offset, length - offset, 0);
        if (written > 0) {
            offset += (size_t)written;
            retry_count = 0;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
            && retry_count++ < 13U) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(fd, &write_set);
            struct timeval timeout = {
                .tv_sec = 0,
                .tv_usec = FN_WEBSOCKET_SELECT_TIMEOUT_US,
            };
            if (select(fd + 1, NULL, &write_set, NULL, &timeout) >= 0) {
                continue;
            }
        }
        return false;
    }
    return true;
}

/** 发送服务端未掩码 WebSocket 帧，所有调用共享同一发送锁。 */
static bool fn_websocket_send_frame(int fd, uint8_t opcode,
        const uint8_t *payload, size_t length) {
    uint8_t header[4];
    size_t header_length;
    header[0] = 0x80U | (opcode & 0x0FU);
    if (length < 126U) {
        header[1] = (uint8_t)length;
        header_length = 2;
    } else if (length <= UINT16_MAX) {
        header[1] = 126U;
        header[2] = (uint8_t)(length >> 8U);
        header[3] = (uint8_t)length;
        header_length = 4;
    } else {
        return false;
    }
    xSemaphoreTake(fn_websocket_send_mutex, portMAX_DELAY);
    bool result = fn_websocket_send_all(fd, header, header_length)
        && fn_websocket_send_all(fd, payload, length);
    xSemaphoreGive(fn_websocket_send_mutex);
    return result;
}

/** 取出解析器安排的 Pong 或 Close 响应并发送。 */
static bool fn_websocket_send_pending_control(int fd, uint32_t generation) {
    uint8_t opcode = 0;
    uint8_t payload[FN_WEBSOCKET_CONTROL_PAYLOAD_SIZE];
    size_t length = 0;
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
        opcode = fn_websocket_pending_control_opcode;
        length = fn_websocket_pending_control_length;
        if (length > 0) {
            memcpy(payload, fn_websocket_pending_control_payload, length);
        }
        fn_websocket_pending_control_opcode = 0;
        fn_websocket_pending_control_length = 0;
    }
    xSemaphoreGive(fn_websocket_state_mutex);
    if (opcode == 0) {
        return true;
    }
    bool sent = fn_websocket_send_frame(fd, opcode, payload, length);
    if (opcode == 0x08U) {
        shutdown(fd, SHUT_RDWR);
        return false;
    }
    return sent;
}

/** 独立接收任务持续推进 select/recv、解帧和完整消息排队。 */
static void fn_websocket_rx_task(void *argument) {
    (void)argument;
    uint8_t chunk[FN_WEBSOCKET_RX_CHUNK_SIZE];
    size_t chunk_offset = 0;
    size_t chunk_length = 0;
    int observed_fd = -1;
    uint32_t observed_generation = 0;
    for (;;) {
        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        int fd = fn_websocket_fd;
        uint32_t generation = fn_websocket_generation;
        bool queue_full = fn_websocket_queue_count >= FN_WEBSOCKET_FRAME_QUEUE_DEPTH;
        xSemaphoreGive(fn_websocket_state_mutex);

        if (fd < 0) {
            chunk_offset = 0;
            chunk_length = 0;
            observed_fd = -1;
            vTaskDelay(1);
            continue;
        }
        if (fd != observed_fd || generation != observed_generation) {
            chunk_offset = 0;
            chunk_length = 0;
            observed_fd = fd;
            observed_generation = generation;
        }
        if (queue_full) {
            vTaskDelay(1);
            continue;
        }

        if (chunk_offset >= chunk_length) {
            fd_set read_set;
            fd_set error_set;
            FD_ZERO(&read_set);
            FD_ZERO(&error_set);
            FD_SET(fd, &read_set);
            FD_SET(fd, &error_set);
            struct timeval timeout = {
                .tv_sec = 0,
                .tv_usec = FN_WEBSOCKET_SELECT_TIMEOUT_US,
            };
            int selected = select(fd + 1, &read_set, NULL, &error_set, &timeout);
            if (selected < 0 && errno == EINTR) {
                continue;
            }
            if (selected < 0 || FD_ISSET(fd, &error_set)) {
                xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
                if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
                    fn_websocket_fail_locked("WEBSOCKET_SELECT_ERROR");
                }
                xSemaphoreGive(fn_websocket_state_mutex);
                continue;
            }
            if (selected == 0 || !FD_ISSET(fd, &read_set)) {
                continue;
            }
            int received = recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
                if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
                    fn_websocket_fail_locked(
                        received == 0 ? "WEBSOCKET_CLOSED" : "WEBSOCKET_RECV_ERROR");
                }
                xSemaphoreGive(fn_websocket_state_mutex);
                continue;
            }
            chunk_offset = 0;
            chunk_length = (size_t)received;
        }

        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
            ++fn_websocket_receive_activity;
        }
        while (chunk_offset < chunk_length
            && fn_websocket_queue_count < FN_WEBSOCKET_FRAME_QUEUE_DEPTH
            && fn_websocket_pending_control_opcode == 0
            && fd == fn_websocket_fd && generation == fn_websocket_generation) {
            if (!fn_websocket_feed_byte_locked(chunk[chunk_offset++])) {
                break;
            }
        }
        bool session_valid = fd == fn_websocket_fd
            && generation == fn_websocket_generation;
        xSemaphoreGive(fn_websocket_state_mutex);
        if (session_valid && !fn_websocket_send_pending_control(fd, generation)) {
            xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
            if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
                fn_websocket_fail_locked("WEBSOCKET_CONTROL_CLOSED");
            }
            xSemaphoreGive(fn_websocket_state_mutex);
        }
    }
}

/** 初始化缓冲、互斥量和常驻原生接收任务。 */
static void fn_websocket_ensure_initialized(void) {
    if (fn_websocket_state_mutex == NULL) {
        fn_websocket_state_mutex = xSemaphoreCreateMutexStatic(
            &fn_websocket_state_mutex_storage);
    }
    if (fn_websocket_send_mutex == NULL) {
        fn_websocket_send_mutex = xSemaphoreCreateMutexStatic(
            &fn_websocket_send_mutex_storage);
    }
    if (fn_websocket_message_buffer == NULL) {
        fn_websocket_message_buffer = fn_websocket_allocate(
            FN_WEBSOCKET_MAX_MESSAGE_SIZE);
    }
    if (fn_websocket_queue_storage == NULL) {
        fn_websocket_queue_storage = fn_websocket_allocate(
            FN_WEBSOCKET_FRAME_QUEUE_DEPTH * FN_WEBSOCKET_MAX_MESSAGE_SIZE);
    }
    if (fn_websocket_message_buffer == NULL || fn_websocket_queue_storage == NULL) {
        mp_raise_msg(&mp_type_MemoryError,
            MP_ERROR_TEXT("WebSocket frame buffer allocation failed"));
    }
    if (fn_websocket_task_handle == NULL) {
        BaseType_t created = xTaskCreate(
            fn_websocket_rx_task,
            "fn_ws_rx",
            FN_WEBSOCKET_TASK_STACK_SIZE,
            NULL,
            FN_WEBSOCKET_TASK_PRIORITY,
            &fn_websocket_task_handle);
        if (created != pdPASS) {
            fn_websocket_task_handle = NULL;
            mp_raise_msg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("WebSocket RX task creation failed"));
        }
    }
}

/** 接管一个已经完成 HTTP Upgrade 的非阻塞 TCP socket。 */
static mp_obj_t fn_websocket_attach(mp_obj_t fd_in) {
    fn_websocket_ensure_initialized();
    int fd = mp_obj_get_int(fd_in);
    if (fd < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid WebSocket socket fd"));
    }
    int no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    int previous_fd = fn_websocket_fd;
    fn_websocket_fd = fd;
    ++fn_websocket_generation;
    fn_websocket_reset_session_locked();
    xSemaphoreGive(fn_websocket_state_mutex);
    // 防御直接重复 attach：立即唤醒仍在等待旧连接的接收任务。
    if (previous_fd >= 0 && previous_fd != fd) {
        shutdown(previous_fd, SHUT_RDWR);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(fn_websocket_attach_obj, fn_websocket_attach);

/** 停止原生任务使用当前 socket，并通过 shutdown 唤醒正在等待的 select。 */
static mp_obj_t fn_websocket_detach(void) {
    if (fn_websocket_state_mutex == NULL) {
        return mp_const_none;
    }
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    int fd = fn_websocket_fd;
    fn_websocket_fd = -1;
    ++fn_websocket_generation;
    fn_websocket_reset_session_locked();
    xSemaphoreGive(fn_websocket_state_mutex);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_websocket_detach_obj, fn_websocket_detach);

/** 返回原生数据面是否仍持有活动连接。 */
static mp_obj_t fn_websocket_connected(void) {
    bool connected = false;
    if (fn_websocket_state_mutex != NULL) {
        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        connected = fn_websocket_fd >= 0;
        xSemaphoreGive(fn_websocket_state_mutex);
    }
    return mp_obj_new_bool(connected);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_websocket_connected_obj, fn_websocket_connected);

/** 返回完整 WebSocket 消息队列中的待读数量。 */
static mp_obj_t fn_websocket_frames_available(void) {
    size_t count = 0;
    if (fn_websocket_state_mutex != NULL) {
        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        count = fn_websocket_queue_count + (fn_websocket_error_pending ? 1U : 0U);
        xSemaphoreGive(fn_websocket_state_mutex);
    }
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    fn_websocket_frames_available_obj, fn_websocket_frames_available);

/** 返回接收活动序号，Python 据此刷新会话心跳而无需读取原始 socket。 */
static mp_obj_t fn_websocket_receive_activity_value(void) {
    uint32_t activity = 0;
    if (fn_websocket_state_mutex != NULL) {
        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        activity = fn_websocket_receive_activity;
        xSemaphoreGive(fn_websocket_state_mutex);
    }
    return mp_obj_new_int_from_uint(activity);
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    fn_websocket_receive_activity_obj, fn_websocket_receive_activity_value);

/** 复制并移除队首完整 WebSocket 业务消息。 */
static mp_obj_t fn_websocket_read_frame(void) {
    if (fn_websocket_state_mutex == NULL) {
        return mp_const_none;
    }
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    if (fn_websocket_queue_count == 0) {
        xSemaphoreGive(fn_websocket_state_mutex);
        return mp_const_none;
    }
    size_t length = fn_websocket_queue_lengths[fn_websocket_queue_head];
    uint8_t *source = fn_websocket_queue_storage
        + fn_websocket_queue_head * FN_WEBSOCKET_MAX_MESSAGE_SIZE;
    xSemaphoreGive(fn_websocket_state_mutex);

    // 队首在正式出队前不会被接收任务覆盖，避免分配失败把互斥量永久锁住。
    mp_obj_t result = mp_obj_new_bytes(source, length);
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    fn_websocket_queue_head =
        (fn_websocket_queue_head + 1) % FN_WEBSOCKET_FRAME_QUEUE_DEPTH;
    --fn_websocket_queue_count;
    xSemaphoreGive(fn_websocket_state_mutex);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_websocket_read_frame_obj, fn_websocket_read_frame);

/** 取出一次原生接收错误。 */
static mp_obj_t fn_websocket_read_error(void) {
    if (fn_websocket_state_mutex == NULL) {
        return mp_const_none;
    }
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    if (!fn_websocket_error_pending) {
        xSemaphoreGive(fn_websocket_state_mutex);
        return mp_const_none;
    }
    char error[sizeof(fn_websocket_error)];
    strncpy(error, fn_websocket_error, sizeof(error));
    fn_websocket_error_pending = false;
    xSemaphoreGive(fn_websocket_state_mutex);
    return mp_obj_new_bytes((const byte *)error, strlen(error));
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_websocket_read_error_obj, fn_websocket_read_error);

/** 通过统一原生发送锁写出一个服务端 WebSocket 帧。 */
static mp_obj_t fn_websocket_send(mp_obj_t payload_in, mp_obj_t opcode_in) {
    mp_buffer_info_t payload;
    mp_get_buffer_raise(payload_in, &payload, MP_BUFFER_READ);
    uint8_t opcode = (uint8_t)mp_obj_get_int(opcode_in);
    xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
    int fd = fn_websocket_fd;
    uint32_t generation = fn_websocket_generation;
    xSemaphoreGive(fn_websocket_state_mutex);
    if (fd < 0) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    bool sent = fn_websocket_send_frame(fd, opcode, payload.buf, payload.len);
    if (!sent) {
        xSemaphoreTake(fn_websocket_state_mutex, portMAX_DELAY);
        if (fd == fn_websocket_fd && generation == fn_websocket_generation) {
            fn_websocket_fail_locked("WEBSOCKET_SEND_ERROR");
        }
        xSemaphoreGive(fn_websocket_state_mutex);
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    return mp_obj_new_int_from_uint(payload.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(fn_websocket_send_obj, fn_websocket_send);

/** 返回原生模块接口版本。 */
static mp_obj_t fn_websocket_api_version(void) {
    return MP_OBJ_NEW_SMALL_INT(FN_WEBSOCKET_API_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(fn_websocket_api_version_obj, fn_websocket_api_version);

static const mp_rom_map_elem_t fn_websocket_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fn_websocket) },
    { MP_ROM_QSTR(MP_QSTR_api_version), MP_ROM_PTR(&fn_websocket_api_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&fn_websocket_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&fn_websocket_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&fn_websocket_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_frames_available), MP_ROM_PTR(&fn_websocket_frames_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_receive_activity), MP_ROM_PTR(&fn_websocket_receive_activity_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_frame), MP_ROM_PTR(&fn_websocket_read_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_error), MP_ROM_PTR(&fn_websocket_read_error_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&fn_websocket_send_obj) },
};
static MP_DEFINE_CONST_DICT(
    fn_websocket_module_globals, fn_websocket_module_globals_table);

const mp_obj_module_t fn_websocket_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&fn_websocket_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fn_websocket, fn_websocket_user_cmodule);
