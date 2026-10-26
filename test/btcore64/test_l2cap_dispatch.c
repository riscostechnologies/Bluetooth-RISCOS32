/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "l2cap_dispatch.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    printf("L2CAP dispatch line %u: %s\n", (unsigned int)__LINE__, #x); } } while (0)

static int inject(btv_l2cap_dispatch *dispatch, uint8_t code, uint8_t identifier,
                  const uint8_t *parameters, size_t bytes)
{
    uint8_t pdu[800];
    size_t length;
    length = btv_l2cap_signal_encode(pdu, sizeof(pdu), code, identifier, parameters, bytes);
    CHECK(length != 0);
    return btv_l2cap_dispatch_receive(dispatch, pdu, length, 100);
}

static size_t peek(btv_l2cap_dispatch *dispatch, uint8_t *packet,
                   uint8_t code, uint8_t identifier, unsigned int *token)
{
    uint8_t repeated[800];
    unsigned int second;
    size_t a, b;
    a = btv_l2cap_dispatch_output(dispatch, packet, 800, token, 100);
    b = btv_l2cap_dispatch_output(dispatch, repeated, sizeof(repeated), &second, 100);
    CHECK(a >= 8 && a == b && *token == second);
    CHECK(memcmp(packet, repeated, a) == 0);
    CHECK(packet[4] == code && packet[5] == identifier);
    return a;
}

unsigned int test_l2cap_dispatch(void)
{
    btv_l2cap_dispatch dispatch;
    uint8_t packet[800], batch[800], other[800], zeros[700];
    uint8_t connect[8] = { 0x80, 0, 0x40, 0, 0, 0, 0, 0 };
    uint8_t config[8] = { 0x40, 0, 0, 0, 1, 2, 0, 2 };
    uint8_t accepted[6] = { 0x40, 0, 0, 0, 0, 0 };
    uint8_t info[2] = { 2, 0 };
    uint8_t close[4] = { 0x80, 0, 0x40, 0 };
    unsigned int token, i;
    size_t a, b;
    failures = 0;
    memset(zeros, 0, sizeof(zeros));
    btv_l2cap_dispatch_init(&dispatch);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0x40);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 3, 672, 100) == 0x41);
    peek(&dispatch, packet, 2, 1, &token);
    CHECK(packet[8] == 1 && packet[10] == 0x40);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    peek(&dispatch, packet, 2, 3, &token);
    CHECK(packet[8] == 3 && packet[10] == 0x41);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    /* Responses arrive in reverse order, with distinct remote CIDs. */
    connect[0] = 0x81;
    connect[2] = 0x41;
    CHECK(inject(&dispatch, 3, 3, connect, 8) == 1);
    connect[0] = 0x80;
    connect[2] = 0x40;
    CHECK(inject(&dispatch, 3, 1, connect, 8) == 1);
    peek(&dispatch, packet, 4, 2, &token);
    CHECK(packet[8] == 0x80);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    peek(&dispatch, packet, 4, 4, &token);
    CHECK(packet[8] == 0x81);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    /* Two peer configuration commands share one PDU and independent slots. */
    a = btv_l2cap_signal_encode(batch, sizeof(batch), 4, 70, config, 8);
    config[0] = 0x41;
    b = btv_l2cap_signal_encode(other, sizeof(other), 4, 71, config, 8);
    memcpy(batch + a, other + 4, b - 4);
    batch[0] = (uint8_t)(a + b - 8);
    CHECK(btv_l2cap_dispatch_receive(&dispatch, batch, a + b - 4, 100) == 1);
    CHECK(inject(&dispatch, 5, 2, accepted, 6) == 1);
    accepted[0] = 0x41;
    CHECK(inject(&dispatch, 5, 4, accepted, 6) == 1);
    peek(&dispatch, packet, 5, 70, &token);
    CHECK(packet[8] == 0x80);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    peek(&dispatch, packet, 5, 71, &token);
    CHECK(packet[8] == 0x81);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(btv_l2cap_dispatch_channel(&dispatch, 0x40)->state == BTV_CHANNEL_OPEN);
    CHECK(btv_l2cap_dispatch_channel(&dispatch, 0x41)->state == BTV_CHANNEL_OPEN);
    CHECK(btv_l2cap_dispatch_channel(&dispatch, 0x42) == NULL);
    CHECK(btv_l2cap_dispatch_disconnect(&dispatch, 0x40, 100));
    peek(&dispatch, packet, 6, 5, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 7, 5, close, 4) == 1);
    CHECK(btv_l2cap_dispatch_channel(&dispatch, 0x40)->state == BTV_CHANNEL_CLOSED);
    CHECK(btv_l2cap_dispatch_channel(&dispatch, 0x41)->state == BTV_CHANNEL_OPEN);

    /* Generic information and unsupported PSM/CID handling. */
    CHECK(inject(&dispatch, 10, 80, info, 2) == 1);
    CHECK(peek(&dispatch, packet, 11, 80, &token) == 16);
    CHECK(packet[12] == 0x80 && packet[10] == 0);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    info[0] = 3;
    CHECK(inject(&dispatch, 10, 81, info, 2) == 1);
    CHECK(peek(&dispatch, packet, 11, 81, &token) == 20);
    CHECK(packet[12] == 2);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    info[0] = 1;
    CHECK(inject(&dispatch, 10, 82, info, 2) == 1);
    CHECK(peek(&dispatch, packet, 11, 82, &token) == 12);
    CHECK(packet[10] == 1);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    config[0] = 0x7f;
    CHECK(inject(&dispatch, 4, 83, config, 8) == 1);
    CHECK(peek(&dispatch, packet, 1, 83, &token) == 14);
    CHECK(packet[8] == 2 && packet[10] == 0 && packet[12] == 0x7f);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 6, 84, close, 4) == 1);
    peek(&dispatch, packet, 1, 84, &token);
    CHECK(packet[10] == 0x80 && packet[12] == 0x40);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 2, 85, close, 4) == 1);
    peek(&dispatch, packet, 3, 85, &token);
    CHECK(packet[8] == 0 && packet[10] == 0x40 && packet[12] == 2);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 0xff, 86, NULL, 0) == 1);
    CHECK(peek(&dispatch, packet, 1, 86, &token) == 10 && packet[8] == 0);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 9, 86, NULL, 0) == 1);
    CHECK(btv_l2cap_dispatch_output(&dispatch, packet, sizeof(packet), &token, 100) == 0);

    /* Copied input batch stalls at its second generic response, then resumes. */
    a = btv_l2cap_signal_encode(batch, sizeof(batch), 8, 90, NULL, 0);
    b = btv_l2cap_signal_encode(other, sizeof(other), 8, 91, NULL, 0);
    memcpy(batch + a, other + 4, b - 4);
    batch[0] = 8;
    CHECK(btv_l2cap_dispatch_receive(&dispatch, batch, 12, 100) == 1);
    memset(batch, 0xa5, 12);
    CHECK(inject(&dispatch, 8, 92, NULL, 0) == 2);
    peek(&dispatch, packet, 9, 90, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    peek(&dispatch, packet, 9, 91, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 8, 92, NULL, 0) == 1);
    peek(&dispatch, packet, 9, 92, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    /* A bad trailing command prevents dispatch of the otherwise valid first. */
    a = btv_l2cap_signal_encode(batch, sizeof(batch), 8, 90, NULL, 0);
    memcpy(batch + a, other + 4, b - 4);
    batch[0] = 8;
    batch[9] = 0;
    CHECK(btv_l2cap_dispatch_receive(&dispatch, batch, 12, 100) == -1);
    CHECK(btv_l2cap_dispatch_output(&dispatch, packet, sizeof(packet), &token, 100) == 0);
    CHECK(!dispatch.failed);
    CHECK(inject(&dispatch, 8, 93, zeros, 669) == 1);
    peek(&dispatch, packet, 1, 93, &token);
    CHECK(packet[8] == 1 && packet[10] == 0xa0 && packet[11] == 2);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));

    /* Duplicate active remote CID makes routing ambiguous: abort the link. */
    btv_l2cap_dispatch_init(&dispatch);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0x40);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 3, 672, 100) == 0x41);
    peek(&dispatch, packet, 2, 1, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    peek(&dispatch, packet, 2, 3, &token);
    CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
    CHECK(inject(&dispatch, 3, 1, connect, 8) == 1);
    connect[2] = 0x41;
    CHECK(inject(&dispatch, 3, 3, connect, 8) == -1);
    CHECK(dispatch.failed);
    CHECK(btv_l2cap_dispatch_output(&dispatch, packet, sizeof(packet), &token, 100) == 0);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0);
    btv_l2cap_dispatch_init(&dispatch);
    for (i = 0; i < BTV_L2CAP_CHANNELS; ++i)
        CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0x40 + i);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0);
    btv_l2cap_dispatch_init(&dispatch);
    dispatch.next_identifier = 254;
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0x40);
    CHECK(dispatch.next_identifier == 256);
    CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0);
    /* The dispatcher supplies the retry ID and never reuses the previous one. */
    {
        uint8_t offer[10] = { 0x40, 0, 0, 0, 1, 0, 1, 2, 0, 2 };
        btv_l2cap_dispatch_init(&dispatch);
        CHECK(btv_l2cap_dispatch_connect(&dispatch, 1, 672, 100) == 0x40);
        peek(&dispatch, packet, 2, 1, &token);
        CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
        connect[2] = 0x40;
        CHECK(inject(&dispatch, 3, 1, connect, 8) == 1);
        peek(&dispatch, packet, 4, 2, &token);
        CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
        CHECK(inject(&dispatch, 5, 2, offer, 10) == 1);
        peek(&dispatch, packet, 4, 3, &token);
        CHECK(packet[14] == 0 && packet[15] == 2 && dispatch.next_identifier == 4);
        CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
        accepted[0] = 0x40;
        CHECK(inject(&dispatch, 5, 2, accepted, 6) == 1);
        CHECK(!dispatch.channels[0].local_configured);
        CHECK(inject(&dispatch, 5, 3, accepted, 6) == 1);
        config[0] = 0x40;
        CHECK(inject(&dispatch, 4, 99, config, 8) == 1);
        peek(&dispatch, packet, 5, 99, &token);
        CHECK(btv_l2cap_dispatch_sent(&dispatch, token));
        CHECK(dispatch.channels[0].state == BTV_CHANNEL_OPEN);
        CHECK(dispatch.channels[0].receive_mtu == 512);
    }
    if (failures == 0)
        puts("PASS: L2CAP shared dispatch, two-channel routing, copied batches and peer responses");
    return failures;
}
