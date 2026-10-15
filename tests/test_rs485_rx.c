/*
 * OpenCFS - offline RS485 envelope decoder regressions.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "opencfs/rs485_rx.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned count;
    uint8_t address;
    uint8_t command;
    uint8_t qualifier;
    size_t payload_size;
    uint8_t payload[OCFS_RS485_MAX_PAYLOAD_LEN];
} received_t;

static void copy_frame(void *context, const ocfs_rs485_frame_t *frame)
{
    received_t *out = (received_t *)context;
    assert(out != NULL && frame != NULL);
    assert(frame->payload_size <= sizeof(out->payload));
    ++out->count;
    out->address = frame->address;
    out->qualifier = frame->qualifier;
    out->command = frame->command;
    out->payload_size = frame->payload_size;
    if (frame->payload_size != 0u) {
        memcpy(out->payload, frame->payload, frame->payload_size);
    }
}

/* Test-only synthetic frame constructor. OpenCFS runtime has no transmitter. */
static size_t construct(uint8_t *out, uint8_t address, uint8_t qualifier,
                        uint8_t command, const uint8_t *payload, size_t size)
{
    assert(size <= OCFS_RS485_MAX_PAYLOAD_LEN);
    assert(out != NULL);
    out[0] = OCFS_RS485_SYNC;
    out[1] = address;
    out[2] = (uint8_t)(size + 3u);
    out[3] = qualifier;
    out[4] = command;
    if (size != 0u) {
        assert(payload != NULL);
        memcpy(out + 5, payload, size);
    }
    out[5 + size] = ocfs_rs485_crc8(out + 2, size + 3u);
    return size + 6u;
}

static void test_documented_reference_frames(void)
{
    /* Two independently documented, read-only protocol examples. */
    static const uint8_t stock_version_query[] =
        {0xF7, 0x01, 0x04, 0x00, 0xF0, 0x00, 0x4C};
    static const uint8_t version_query[] =
        {0xF7, 0x01, 0x03, 0xFF, 0x14, 0x06};
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    assert(ocfs_rs485_crc8(stock_version_query + 2, 4u) == 0x4Cu);
    assert(ocfs_rs485_crc8(version_query + 2, 3u) == 0x06u);
    ocfs_rs485_rx_feed(&rx, stock_version_query,
                       sizeof(stock_version_query), copy_frame, &out);
    assert(out.count == 1u && out.address == 1u && out.command == 0xF0u);
    assert(out.qualifier == 0u && out.payload_size == 1u &&
           out.payload[0] == 0u);
    ocfs_rs485_rx_feed(&rx, version_query, sizeof(version_query), copy_frame, &out);
    assert(out.count == 2u && out.command == 0x14u &&
           out.qualifier == 0xFFu && out.payload_size == 0u);
    assert(rx.valid_frames == 2u && rx.buffered == 0u);
}

static void test_split_noise_and_embedded_sync(void)
{
    uint8_t packet[OCFS_RS485_MAX_WIRE_LEN];
    const uint8_t payload[] = {0x11, 0xF7, 0x00, 0xF7, 0xAB};
    const uint8_t junk[] = {0x02, 0x03, 0x04, 0x55};
    const size_t count = construct(packet, 2u, 0x80u, 0x24u,
                                   payload, sizeof(payload));
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(&rx, junk, sizeof(junk), copy_frame, &out);
    for (size_t i = 0; i < count; ++i) {
        ocfs_rs485_rx_feed(&rx, packet + i, 1u, copy_frame, &out);
    }
    assert(out.count == 1u && out.command == 0x24u &&
           out.payload_size == sizeof(payload));
    assert(memcmp(out.payload, payload, sizeof(payload)) == 0);
    assert(rx.dropped_bytes == sizeof(junk) &&
           rx.crc_failures == 0u && rx.buffered == 0u);
}

static void test_crc_failure_and_recovery(void)
{
    uint8_t bad[OCFS_RS485_MAX_WIRE_LEN];
    uint8_t good[OCFS_RS485_MAX_WIRE_LEN];
    uint8_t input[2u * OCFS_RS485_MAX_WIRE_LEN];
    const uint8_t wrong_payload[] = {0x12, 0x34};
    const size_t n_bad = construct(bad, 3u, 0u, 0x10u,
                                   wrong_payload, sizeof(wrong_payload));
    const size_t n_good = construct(good, 3u, 0u, 0x14u, NULL, 0u);
    bad[n_bad - 1u] ^= 0x01u;
    memcpy(input, bad, n_bad);
    memcpy(input + n_bad, good, n_good);
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(&rx, input, n_bad + n_good, copy_frame, &out);
    assert(out.count == 1u && out.command == 0x14u &&
           rx.crc_failures == 1u && rx.valid_frames == 1u);
}

static void test_invalid_lengths_resynchronize(void)
{
    const uint8_t bad_lengths[] = {
        0xF7, 0x02, 0x00,
        0xF7, 0x01, 0x02
    };
    uint8_t good[OCFS_RS485_MAX_WIRE_LEN];
    const size_t n = construct(good, 4u, 0u, 0x14u, NULL, 0u);
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(&rx, bad_lengths, sizeof(bad_lengths),
                       copy_frame, &out);
    ocfs_rs485_rx_feed(&rx, good, n, copy_frame, &out);
    assert(out.count == 1u && rx.bad_lengths == 2u &&
           rx.buffered == 0u);
}

static void test_timeout_partial_frame_and_restart(void)
{
    uint8_t packet[OCFS_RS485_MAX_WIRE_LEN];
    const size_t n = construct(packet, 5u, 0u, 0x14u, NULL, 0u);
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(&rx, packet, n - 2u, copy_frame, &out);
    assert(rx.buffered != 0u && out.count == 0u);
    ocfs_rs485_rx_timeout_reset(&rx);
    assert(rx.buffered == 0u && rx.timeout_resets == 1u);
    ocfs_rs485_rx_feed(&rx, packet, n, copy_frame, &out);
    assert(out.count == 1u && rx.valid_frames == 1u);
}

static void test_full_length_payload(void)
{
    uint8_t payload[OCFS_RS485_MAX_PAYLOAD_LEN];
    uint8_t packet[OCFS_RS485_MAX_WIRE_LEN];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 17u + 9u);
    }
    const size_t n = construct(packet, 8u, 1u, 0x14u,
                               payload, sizeof(payload));
    assert(n == OCFS_RS485_MAX_WIRE_LEN);
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(&rx, packet, n, copy_frame, &out);
    assert(out.count == 1u && out.payload_size == sizeof(payload) &&
           memcmp(out.payload, payload, sizeof(payload)) == 0);
    assert(rx.buffered == 0u && rx.crc_failures == 0u);
}

static void test_deterministic_noise_and_chunk_boundaries(void)
{
    uint8_t packet[OCFS_RS485_MAX_WIRE_LEN];
    ocfs_rs485_rx_t rx;
    received_t out = {0};
    ocfs_rs485_rx_init(&rx);
    uint32_t value = 0xDEADBEEFu;
    for (size_t k = 0; k < 2000u; ++k) {
        value = value * 1664525u + 1013904223u;
        if ((k % 9u) == 0u) {
            const uint8_t garbage[] = {0x00, 0x55, 0x02};
            ocfs_rs485_rx_feed(&rx, garbage, sizeof(garbage),
                               copy_frame, &out);
        }
        const uint8_t synthetic[] = {
            (uint8_t)value,
            (uint8_t)(value >> 8),
            (uint8_t)(value >> 16)
        };
        const size_t n = construct(packet, 0x22u, 0u, 0x14u,
                                   synthetic, sizeof(synthetic));
        const size_t cut = 1u + (value % (n - 1u));
        ocfs_rs485_rx_feed(&rx, packet, cut, copy_frame, &out);
        ocfs_rs485_rx_feed(&rx, packet + cut, n - cut, copy_frame, &out);
    }
    assert(out.count == 2000u && rx.crc_failures == 0u &&
           rx.buffered == 0u && rx.valid_frames == 2000u);
}

static void test_null_guard_and_empty_callback(void)
{
    ocfs_rs485_rx_t rx;
    ocfs_rs485_rx_init(&rx);
    ocfs_rs485_rx_feed(NULL, NULL, 8u, NULL, NULL);
    ocfs_rs485_rx_feed(&rx, NULL, 3u, NULL, NULL);
    const uint8_t message[] = {0xF7, 0x01, 0x03, 0xFF, 0x14, 0x06};
    ocfs_rs485_rx_feed(&rx, message, sizeof(message), NULL, NULL);
    assert(rx.valid_frames == 1u && rx.buffered == 0u);
    ocfs_rs485_rx_timeout_reset(NULL);
}

int main(void)
{
    test_documented_reference_frames();
    test_split_noise_and_embedded_sync();
    test_crc_failure_and_recovery();
    test_invalid_lengths_resynchronize();
    test_timeout_partial_frame_and_restart();
    test_full_length_payload();
    test_deterministic_noise_and_chunk_boundaries();
    test_null_guard_and_empty_callback();
    puts("OpenCFS RS485 RX: PASS (offline, receive-only)");
    return 0;
}
