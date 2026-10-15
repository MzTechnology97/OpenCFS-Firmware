/*
 * OpenCFS - standalone, receive-only CFS RS485 wire-envelope decoder.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Research evidence is linked from docs/RS485_RX_ENVELOPE.md.
 * This module NEVER sends bytes or dispatches a command to hardware.
 */
#ifndef OPEN_CFS_RS485_RX_H
#define OPEN_CFS_RS485_RX_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OCFS_RS485_SYNC = 0xF7,
    OCFS_RS485_MIN_DECLARED_LEN = 3,
    OCFS_RS485_MAX_DECLARED_LEN = 255,
    OCFS_RS485_MAX_WIRE_LEN = OCFS_RS485_MAX_DECLARED_LEN + 3,
    OCFS_RS485_MAX_PAYLOAD_LEN = OCFS_RS485_MAX_DECLARED_LEN - 3
};

/* Bytes are borrowed from the decoder's buffer and are only valid in callback. */
typedef struct {
    uint8_t address;
    uint8_t qualifier; /* Request header or response status, not interpreted. */
    uint8_t command;
    const uint8_t *payload;
    size_t payload_size;
} ocfs_rs485_frame_t;

typedef void (*ocfs_rs485_rx_callback_t)(void *context,
                                         const ocfs_rs485_frame_t *frame);

typedef struct {
    uint8_t buffer[OCFS_RS485_MAX_WIRE_LEN];
    size_t buffered;
    uint32_t valid_frames;
    uint32_t crc_failures;
    uint32_t bad_lengths;
    uint32_t dropped_bytes;
    uint32_t timeout_resets;
} ocfs_rs485_rx_t;

void ocfs_rs485_rx_init(ocfs_rs485_rx_t *rx);

/* Call only with already-received bytes, never with a device handle. */
void ocfs_rs485_rx_feed(ocfs_rs485_rx_t *rx, const uint8_t *input,
                       size_t size, ocfs_rs485_rx_callback_t callback,
                       void *context);

/* Explicitly discard partial data at a transport silence/timeout boundary. */
void ocfs_rs485_rx_timeout_reset(ocfs_rs485_rx_t *rx);

/* Polynomial 0x07, initial 0x00, no reflection, no final xor. */
uint8_t ocfs_rs485_crc8(const uint8_t *bytes, size_t size);

#ifdef __cplusplus
}
#endif
#endif
