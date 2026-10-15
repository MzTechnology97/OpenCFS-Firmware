/*
 * OpenCFS receive-only, bounded streaming RS485 envelope.
 * SPDX-License-Identifier: Apache-2.0
 * This source has no transport, GPIO, flash or CFS command execution.
 */
#include "opencfs/rs485_rx.h"
#include <string.h>

uint8_t ocfs_rs485_crc8(const uint8_t *bytes, size_t size)
{
    uint8_t crc = 0;
    if (bytes == NULL) {
        return 0; /* Callers must separately validate non-NULL buffers. */
    }
    for (size_t i = 0; i < size; ++i) {
        crc = (uint8_t)(crc ^ bytes[i]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint8_t)((crc & 0x80u)
                    ? ((uint8_t)(crc << 1) ^ 0x07u)
                    : ((uint8_t)(crc << 1)));
        }
    }
    return crc;
}

void ocfs_rs485_rx_init(ocfs_rs485_rx_t *rx)
{
    if (rx != NULL) {
        memset(rx, 0, sizeof(*rx));
    }
}

/*
 * Source-agnostic synchronization. On a malformed envelope, search its
 * buffered bytes for the next 0xF7. A valid frame may contain 0xF7 in
 * payload; resynchronization occurs ONLY after a CRC/length failure.
 */
static void resync_after_error(ocfs_rs485_rx_t *rx)
{
    size_t next = 1;
    while (next < rx->buffered && rx->buffer[next] != OCFS_RS485_SYNC) {
        ++next;
    }
    if (next == rx->buffered) {
        rx->dropped_bytes += (uint32_t)rx->buffered;
        rx->buffered = 0;
        return;
    }
    rx->dropped_bytes += (uint32_t)next;
    memmove(rx->buffer, rx->buffer + next, rx->buffered - next);
    rx->buffered -= next;
}

static void consume_buffer(ocfs_rs485_rx_t *rx,
                           ocfs_rs485_rx_callback_t callback, void *context)
{
    while (rx->buffered >= 3u) {
        const size_t declared = rx->buffer[2];
        if (declared < OCFS_RS485_MIN_DECLARED_LEN) {
            ++rx->bad_lengths;
            resync_after_error(rx);
            continue;
        }
        const size_t target = declared + 3u;
        if (target > OCFS_RS485_MAX_WIRE_LEN) {
            ++rx->bad_lengths;
            resync_after_error(rx);
            continue;
        }
        if (rx->buffered < target) {
            return;
        }
        const uint8_t expected_crc = ocfs_rs485_crc8(
            rx->buffer + 2, target - 3u);
        if (expected_crc != rx->buffer[target - 1u]) {
            ++rx->crc_failures;
            resync_after_error(rx);
            continue;
        }
        ++rx->valid_frames;
        if (callback != NULL) {
            const ocfs_rs485_frame_t frame = {
                .address = rx->buffer[1],
                .qualifier = rx->buffer[3],
                .command = rx->buffer[4],
                .payload = rx->buffer + 5,
                .payload_size = declared - 3u
            };
            callback(context, &frame);
        }
        const size_t leftover = rx->buffered - target;
        if (leftover != 0u) {
            memmove(rx->buffer, rx->buffer + target, leftover);
        }
        rx->buffered = leftover;
    }
}

void ocfs_rs485_rx_feed(ocfs_rs485_rx_t *rx, const uint8_t *input,
                       size_t size, ocfs_rs485_rx_callback_t callback,
                       void *context)
{
    if (rx == NULL || (input == NULL && size != 0u)) {
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        const uint8_t byte = input[i];
        if (rx->buffered == 0u && byte != OCFS_RS485_SYNC) {
            ++rx->dropped_bytes;
            continue;
        }
        if (rx->buffered >= OCFS_RS485_MAX_WIRE_LEN) {
            /* Defensive guard; cannot occur for a valid envelope. */
            ++rx->bad_lengths;
            resync_after_error(rx);
            if (rx->buffered >= OCFS_RS485_MAX_WIRE_LEN) {
                rx->buffered = 0u;
            }
        }
        rx->buffer[rx->buffered++] = byte;
        consume_buffer(rx, callback, context);
    }
}

void ocfs_rs485_rx_timeout_reset(ocfs_rs485_rx_t *rx)
{
    if (rx != NULL) {
        if (rx->buffered != 0u) {
            rx->dropped_bytes += (uint32_t)rx->buffered;
            rx->buffered = 0u;
        }
        ++rx->timeout_resets;
    }
}
