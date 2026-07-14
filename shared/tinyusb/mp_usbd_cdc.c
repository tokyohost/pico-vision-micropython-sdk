/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Ibrahim Abdelkader <iabdalkader@openmv.io>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "extmod/modmachine.h"

#include "mp_usbd.h"
#include "mp_usbd_cdc.h"

#if MICROPY_HW_USB_CDC && MICROPY_HW_ENABLE_USBDEV && !MICROPY_EXCLUDE_SHARED_TINYUSB_USBD_CDC

// TinyUSB has no public API for endpoint stall detection/clearing; this
// private header is the intended interface for class drivers (all built-in
// TinyUSB class drivers include it for the same purpose).
#include "device/usbd_pvt.h"

static uint8_t cdc_itf_pending; // keep track of cdc interfaces which need attention to poll
static int8_t cdc_connected_flush_delay = 0;

#if MICROPY_HW_USB_CDC_DATA
#define MP_USBD_CDC_DATA_ITF (1)
#if CFG_TUD_CDC < 2
#error "The built-in data CDC requires at least two TinyUSB CDC interfaces"
#endif
#if MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE < 64 || MICROPY_HW_USB_CDC_DATA_RX_BUFSIZE >= UINT16_MAX
#error "The data CDC RX buffer size must be between 64 and 65534 bytes"
#endif
static ringbuf_t cdc_data_rx_ringbuf;

// 返回数据 CDC 的运行时接收缓冲区是否已经就绪。
static bool mp_usbd_cdc_data_rx_ready(void) {
    return cdc_data_rx_ringbuf.buf != NULL && cdc_data_rx_ringbuf.size > 1;
}
#endif

uintptr_t mp_usbd_cdc_poll_interfaces(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    if (!cdc_itf_pending) {
        // Explicitly run the USB stack as the scheduler may be locked (eg we are in
        // an interrupt handler) while there is data pending.
        mp_usbd_task();
    }

    // any CDC interfaces left to poll?
    if (cdc_itf_pending) {
        for (uint8_t itf = 0; itf < 8; ++itf) {
            if (cdc_itf_pending & (1 << itf)) {
                tud_cdc_rx_cb(itf);
                if (!cdc_itf_pending) {
                    break;
                }
            }
        }
    }
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if ((poll_flags & MP_STREAM_POLL_WR) &&
        (!tud_cdc_connected() || (tud_cdc_connected() && tud_cdc_write_available() > 0))) {
        // Always allow write when not connected, fifo will retain latest.
        // When connected operate as blocking, only allow if space is available.
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

void MICROPY_WRAP_TUD_CDC_RX_CB(tud_cdc_rx_cb)(uint8_t itf) {
    // consume pending USB data immediately to free usb buffer and keep the endpoint from stalling.
    // in case the ringbuffer is full, mark the CDC interface that need attention later on for polling
    cdc_itf_pending &= ~(1 << itf);
    #if MICROPY_HW_USB_CDC_DATA
    if (itf == MP_USBD_CDC_DATA_ITF) {
        if (!mp_usbd_cdc_data_rx_ready()) {
            // Python 后端尚未初始化时不取走 TinyUSB FIFO 数据，通过 NAK 保持主机背压。
            cdc_itf_pending |= (1 << itf);
            return;
        }
        uint8_t chunk[64];
        while (tud_cdc_n_available(itf) > 0) {
            size_t free = ringbuf_free(&cdc_data_rx_ringbuf);
            if (!free) {
                cdc_itf_pending |= (1 << itf);
                return;
            }
            uint32_t count = MIN(sizeof(chunk), free);
            count = MIN(count, tud_cdc_n_available(itf));
            count = tud_cdc_n_read(itf, chunk, count);
            if (!count) {
                return;
            }
            ringbuf_memcpy_put_internal(&cdc_data_rx_ringbuf, chunk, count);
        }
        return;
    }
    #endif
    for (uint32_t bytes_avail = tud_cdc_n_available(itf); bytes_avail > 0; --bytes_avail) {
        if (ringbuf_free(&stdin_ringbuf)) {
            uint8_t data_char;
            if (tud_cdc_n_read(itf, &data_char, 1) != 1) {
                return;
            }
            #if MICROPY_KBD_EXCEPTION
            if (data_char == mp_interrupt_char) {
                // Clear the ring buffer
                stdin_ringbuf.iget = stdin_ringbuf.iput = 0;
                // and stop
                mp_sched_keyboard_interrupt();
            } else {
                ringbuf_put(&stdin_ringbuf, data_char);
            }
            #else
            ringbuf_put(&stdin_ringbuf, data_char);
            #endif
        } else {
            cdc_itf_pending |= (1 << itf);
            return;
        }
    }
}

#if MICROPY_HW_USB_CDC_DATA
void mp_usbd_cdc_data_rx_configure(uint8_t *buffer, size_t length) {
    cdc_data_rx_ringbuf.buf = buffer;
    cdc_data_rx_ringbuf.size = (uint16_t)length;
    ringbuf_reset(&cdc_data_rx_ringbuf);
    if (cdc_itf_pending & (1 << MP_USBD_CDC_DATA_ITF)) {
        tud_cdc_rx_cb(MP_USBD_CDC_DATA_ITF);
    }
}

size_t mp_usbd_cdc_data_rx_any(void) {
    if (!mp_usbd_cdc_data_rx_ready()) {
        return 0;
    }
    mp_usbd_task();
    if (cdc_itf_pending & (1 << MP_USBD_CDC_DATA_ITF)) {
        tud_cdc_rx_cb(MP_USBD_CDC_DATA_ITF);
    }
    return ringbuf_avail(&cdc_data_rx_ringbuf);
}

size_t mp_usbd_cdc_data_rx_read(uint8_t *buffer, size_t length) {
    if (!mp_usbd_cdc_data_rx_ready()) {
        return 0;
    }
    size_t available = mp_usbd_cdc_data_rx_any();
    size_t count = MIN(length, available);
    if (count > 0) {
        ringbuf_memcpy_get_internal(&cdc_data_rx_ringbuf, buffer, count);
        if (cdc_itf_pending & (1 << MP_USBD_CDC_DATA_ITF)) {
            tud_cdc_rx_cb(MP_USBD_CDC_DATA_ITF);
        }
    }
    return count;
}

size_t mp_usbd_cdc_data_tx_write(const uint8_t *buffer, size_t length) {
    if (!tusb_inited()) {
        return 0;
    }
    size_t offset = 0;
    mp_uint_t last_write = mp_hal_ticks_ms();
    while (offset < length) {
        uint32_t available = tud_cdc_n_write_available(MP_USBD_CDC_DATA_ITF);
        uint32_t count = MIN(length - offset, available);
        uint32_t written = tud_cdc_n_write(
            MP_USBD_CDC_DATA_ITF,
            buffer + offset,
            count
        );
        tud_cdc_n_write_flush(MP_USBD_CDC_DATA_ITF);
        offset += written;
        if (offset >= length) {
            break;
        }
        if (written > 0) {
            last_write = mp_hal_ticks_ms();
        } else if ((mp_uint_t)(mp_hal_ticks_ms() - last_write) >= MICROPY_HW_USB_CDC_DATA_TX_TIMEOUT) {
            break;
        }
        mp_usbd_task();
        mp_event_wait_ms(1);
    }
    return offset;
}

bool mp_usbd_cdc_data_connected(void) {
    mp_usbd_task();
    return tud_cdc_n_connected(MP_USBD_CDC_DATA_ITF);
}

void mp_usbd_cdc_data_tx_flush(void) {
    tud_cdc_n_write_flush(MP_USBD_CDC_DATA_ITF);
    mp_usbd_task();
}
#endif

mp_uint_t mp_usbd_cdc_tx_strn(const char *str, mp_uint_t len) {
    if (!tusb_inited()) {
        return 0;
    }
    mp_uint_t last_write = mp_hal_ticks_ms();
    size_t i = 0;
    while (i < len) {
        uint32_t n = len - i;

        if (tud_cdc_connected()) {
            // Limit write to available space in tx buffer when connected.
            //
            // (If not connected then we write everything to the fifo, expecting
            // it to overwrite old data so it will have latest data buffered
            // when host connects.)
            n = MIN(n, tud_cdc_write_available());
        }

        uint32_t n2 = tud_cdc_write(str + i, n);
        tud_cdc_write_flush();
        i += n2;

        if (i < len) {
            if (n2 > 0) {
                // reset the timeout each time we successfully write to the FIFO
                last_write = mp_hal_ticks_ms();
            } else {
                if ((mp_uint_t)(mp_hal_ticks_ms() - last_write) >= MICROPY_HW_USB_CDC_TX_TIMEOUT) {
                    break; // Timeout
                }

                if (tud_cdc_connected()) {
                    // If we know we're connected then we can wait for host to make
                    // more space
                    mp_event_wait_ms(1);
                }
            }

            // Always explicitly run the USB stack as the scheduler may be
            // locked (eg we are in an interrupt handler), while there is data
            // or a state change pending.
            mp_usbd_task();
        }
    }
    return i;
}

void MICROPY_WRAP_TUD_SOF_CB(tud_sof_cb)(uint32_t frame_count) {
    if (--cdc_connected_flush_delay < 0) {
        // Finished on-connection delay, disable SOF interrupt again.
        tud_sof_cb_enable(false);
        tud_cdc_write_flush();
    }
}

#endif

#if MICROPY_HW_ENABLE_USBDEV && ( \
    MICROPY_HW_USB_CDC_1200BPS_TOUCH || \
    MICROPY_HW_USB_CDC || \
    MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER)

#if MICROPY_HW_USB_CDC_1200BPS_TOUCH || MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER
static mp_sched_node_t mp_bootloader_sched_node;

static void usbd_cdc_run_bootloader_task(mp_sched_node_t *node) {
    mp_hal_delay_ms(250);
    machine_bootloader(0, NULL);
}
#endif

#if MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER
static struct {
    bool dtr : 1;
    bool rts : 1;
} prev_line_state = {0};
#endif

void MICROPY_WRAP_TUD_CDC_LINE_STATE_CB(tud_cdc_line_state_cb)(uint8_t itf, bool dtr, bool rts) {
    #if MICROPY_HW_USB_CDC && !MICROPY_EXCLUDE_SHARED_TINYUSB_USBD_CDC
    #if MICROPY_HW_USB_CDC_DATA
    if (itf == MP_USBD_CDC_DATA_ITF && !dtr) {
        ringbuf_reset(&cdc_data_rx_ringbuf);
        cdc_itf_pending &= ~(1 << itf);
    }
    #endif
    if (dtr) {
        // A host application has started to open the cdc serial port.
        // USBD_CDC_EP_IN is the IN endpoint for itf 0; only clear stall for itf 0.
        if (itf == 0 && usbd_edpt_stalled(TUD_OPT_RHPORT, USBD_CDC_EP_IN)) {
            usbd_edpt_clear_stall(TUD_OPT_RHPORT, USBD_CDC_EP_IN);
        }
        // Wait a few ms for host to be ready then send tx buffer.
        // High speed connection SOF fires at 125us, full speed at 1ms.
        cdc_connected_flush_delay = (tud_speed_get() == TUSB_SPEED_HIGH) ? 128 : 16;
        tud_sof_cb_enable(true);
    } else {
        // Host has closed the cdc serial port. Discard pending TX data to
        // avoid a full FIFO blocking writes on the next connection.
        tud_cdc_n_write_clear(itf);
    }
    #endif
    #if MICROPY_HW_USB_CDC_DTR_RTS_BOOTLOADER
    if (dtr && !rts) {
        if (prev_line_state.rts && !prev_line_state.dtr) {
            mp_sched_schedule_node(&mp_bootloader_sched_node, usbd_cdc_run_bootloader_task);
        }
    }
    prev_line_state.rts = rts;
    prev_line_state.dtr = dtr;
    #endif
    #if MICROPY_HW_USB_CDC_1200BPS_TOUCH
    if (dtr == false && rts == false) {
        // Device is disconnected.
        cdc_line_coding_t line_coding;
        tud_cdc_n_get_line_coding(itf, &line_coding);
        if (line_coding.bit_rate == 1200) {
            // Delay bootloader jump to allow the USB stack to service endpoints.
            mp_sched_schedule_node(&mp_bootloader_sched_node, usbd_cdc_run_bootloader_task);
        }
    }
    #endif
}

#endif
