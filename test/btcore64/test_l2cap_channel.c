/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
#include "l2cap_channel.h"
#include "acl_transmit.h"
#include "acl_receive.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    printf("L2CAP channel line %u: %s\n", (unsigned int)__LINE__, #x); } } while (0)

/* Exercise the real ACL credit/fragmentation/reassembly boundary with a tiny
 * MTU. No complete signaling header fits in the first fragment. */
static size_t acl_round_trip(const uint8_t *pdu, size_t bytes, uint8_t *output,
                            btv_l2cap_channel *channel, unsigned int token)
{
    btv_acl_transmit tx;
    btv_acl_receive rx;
    btv_acl_completed completed;
    uint8_t tx_storage[2048], rx_storage[2048], packet[9];
    uint32_t ticket;
    size_t length, received;
    const uint8_t *result;
    unsigned int fragments;
    CHECK(btv_acl_transmit_init(&tx, 5, 1, sizeof(packet)) == BTV_TX_OK);
    CHECK(btv_acl_transmit_bind(&tx, 0x123, tx_storage, sizeof(tx_storage)) == BTV_TX_OK);
    CHECK(btv_acl_receive_init(&rx, 0x123, rx_storage, sizeof(rx_storage)) == 1);
    CHECK(btv_acl_transmit_queue(&tx, 0x123, pdu, bytes) == BTV_TX_OK);
    if (channel != NULL)
        CHECK(btv_l2cap_channel_sent(channel, token) == 1);
    fragments = 0;
    completed.handle = 0x123;
    completed.packets = 1;
    while ((length = btv_acl_transmit_peek(&tx, packet, sizeof(packet), &ticket)) != 0)
    {
        CHECK(btv_acl_transmit_sent(&tx, ticket) == BTV_TX_OK);
        CHECK(btv_acl_receive_feed(&rx, packet, length) >= BTV_ACL_MORE);
        if (fragments & 1)
        {
            CHECK(btv_acl_transmit_completed(&tx, &completed, 1) == BTV_TX_OK);
            CHECK(btv_acl_transmit_usb_done(&tx, ticket, 1) == BTV_TX_OK);
        }
        else
        {
            CHECK(btv_acl_transmit_usb_done(&tx, ticket, 1) == BTV_TX_OK);
            CHECK(btv_acl_transmit_completed(&tx, &completed, 1) == BTV_TX_OK);
        }
        ++fragments;
    }
    CHECK(fragments > 1);
    result = btv_acl_receive_packet(&rx, &received);
    CHECK(result != NULL && received == bytes);
    if (result == NULL || received != bytes)
        return 0;
    memcpy(output, result, received);
    CHECK(btv_acl_transmit_take(&tx, 0x123) == BTV_TX_OK);
    CHECK(btv_acl_receive_take(&rx) == 1);
    return received;
}

static int deliver(btv_l2cap_channel *channel, uint8_t code, uint8_t identifier,
                   const uint8_t *parameters, size_t bytes, uint32_t now)
{
    uint8_t pdu[100], received[100];
    size_t length;
    btv_l2cap_signal signal;
    btv_l2cap_command command;
    length = btv_l2cap_signal_encode(pdu, sizeof(pdu), code, identifier, parameters, bytes);
    CHECK(length != 0);
    CHECK(acl_round_trip(pdu, length, received, NULL, 0) == length);
    CHECK(btv_l2cap_signal_open(&signal, received, length) == 1);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 1);
    return btv_l2cap_channel_receive(channel, &command, now);
}

static void transmit(btv_l2cap_channel *channel, uint8_t code, uint8_t id)
{
    uint8_t first[80], second[80];
    size_t a, b;
    unsigned int token, other;
    a = btv_l2cap_channel_output(channel, first, sizeof(first), &token, 100);
    b = btv_l2cap_channel_output(channel, second, sizeof(second), &other, 100);
    CHECK(a >= 8 && a == b && token == other);
    CHECK(memcmp(first, second, a) == 0);
    CHECK(first[4] == code && first[5] == id);
    CHECK(acl_round_trip(first, a, second, channel, token) == a);
    CHECK(memcmp(first, second, a) == 0);
}

static void begin(btv_l2cap_channel *channel)
{
    uint8_t response[8] = { 0x41, 0, 0x40, 0, 0, 0, 0, 0 };
    btv_l2cap_channel_init(channel);
    CHECK(btv_l2cap_channel_connect(channel, 3, 0x40, 672, 1, 2, 100) == 1);
    /* Response before the request was accepted must not advance the channel. */
    CHECK(deliver(channel, 3, 1, response, 8, 100) == 0);
    CHECK(channel->state == BTV_CHANNEL_CONNECTING);
    transmit(channel, 2, 1);
    CHECK(deliver(channel, 3, 3, response, 8, 100) == 0);
    response[2] = 0x42;
    CHECK(deliver(channel, 3, 1, response, 8, 100) == 0);
    response[2] = 0x40;
    response[4] = 1;
    CHECK(deliver(channel, 3, 1, response, 8, 100) == 1);
    CHECK(channel->state == BTV_CHANNEL_CONNECTING);
    response[4] = 0;
    CHECK(deliver(channel, 3, 1, response, 8, 100) == 1);
    CHECK(channel->state == BTV_CHANNEL_CONFIGURING && channel->remote_cid == 0x41);
}

unsigned int test_l2cap_channel(void)
{
    btv_l2cap_channel channel;
    uint8_t request[8] = { 0x40, 0, 0, 0, 1, 2, 0, 4 };
    uint8_t response[6] = { 0x40, 0, 0, 0, 0, 0 };
    uint8_t close_request[4] = { 0x40, 0, 0x41, 0 };
    uint8_t close_response[4] = { 0x41, 0, 0x40, 0 };
    uint8_t packet[80], before[80];
    uint8_t payload[1025], data_pdu[1030];
    const uint8_t *received;
    unsigned int token, order;
    size_t length;
    failures = 0;
    for (order = 0; order < 2; ++order)
    {
        begin(&channel);
        transmit(&channel, 4, 2);
        if (order == 0)
        {
            CHECK(deliver(&channel, 5, 2, response, 6, 100) == 1);
            CHECK(channel.state == BTV_CHANNEL_CONFIGURING);
        }
        CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
        CHECK(channel.transmit_mtu == 1024 && channel.receive_mtu == 672);
        CHECK(deliver(&channel, 4, 9, request, 8, 100) == 2);
        CHECK(channel.state == BTV_CHANNEL_CONFIGURING);
        length = btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 100);
        CHECK(length == 14 && token == 2 && packet[8] == 0x41 && packet[12] == 0);
        transmit(&channel, 5, 9);
        if (order == 1)
        {
            CHECK(channel.state == BTV_CHANNEL_CONFIGURING);
            CHECK(deliver(&channel, 5, 2, response, 6, 100) == 1);
        }
        CHECK(channel.state == BTV_CHANNEL_OPEN);
        memset(payload, 0x69, sizeof(payload));
        CHECK(btv_l2cap_channel_write(&channel, data_pdu, sizeof(data_pdu), payload, 1024) == 1028);
        CHECK(data_pdu[2] == 0x41 && memcmp(data_pdu + 4, payload, 1024) == 0);
        CHECK(btv_l2cap_channel_write(&channel, data_pdu, sizeof(data_pdu), payload, 1025) == 0);
        received = NULL;
        length = 999;
        CHECK(btv_l2cap_channel_read(&channel, data_pdu, 1028, &received, &length) == 0);
        data_pdu[2] = 0x40;
        CHECK(btv_l2cap_channel_read(&channel, data_pdu, 1028, &received, &length) == -1);
        CHECK(received == NULL && length == 999);
        data_pdu[0] = 0xa0;
        data_pdu[1] = 2;
        CHECK(btv_l2cap_channel_read(&channel, data_pdu, 676, &received, &length) == 1);
        CHECK(length == 672 && received == data_pdu + 4);
        /* Lost responses can trigger a peer configuration retransmission. */
        CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
        CHECK(btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 100) == 14);
        CHECK(packet[12] == 0);
        transmit(&channel, 5, 9);
        CHECK(channel.state == BTV_CHANNEL_OPEN);
        /* Remote disconnect can be received without a locally queued close. */
        if (order == 0)
        {
            CHECK(deliver(&channel, 6, 10, close_request, 4, 100) == 1);
            CHECK(channel.state == BTV_CHANNEL_CLOSING);
            transmit(&channel, 7, 10);
        }
        else
        {
            CHECK(btv_l2cap_channel_disconnect(&channel, 3, 100) == 1);
            CHECK(deliver(&channel, 7, 3, close_response, 4, 100) == 0);
            transmit(&channel, 6, 3);
            CHECK(deliver(&channel, 7, 3, close_response, 4, 100) == 1);
        }
        CHECK(channel.state == BTV_CHANNEL_CLOSED);
        CHECK(btv_l2cap_channel_write(&channel, data_pdu, sizeof(data_pdu), payload, 1) == 0);
        CHECK(btv_l2cap_channel_read(&channel, data_pdu, 676, &received, &length) == -1);
        CHECK(deliver(&channel, 5, 2, response, 6, 100) == 0);
    }
    /* Peer configuration may arrive before our own config request is sent. */
    begin(&channel);
    CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
    CHECK(btv_l2cap_channel_sent(&channel, 1) == 0);
    transmit(&channel, 5, 9);
    transmit(&channel, 4, 2);
    CHECK(deliver(&channel, 5, 2, response, 6, 100) == 1);
    CHECK(channel.state == BTV_CHANNEL_OPEN);

    begin(&channel);
    request[6] = 16;
    request[7] = 0;
    CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
    length = btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 100);
    CHECK(length == 18 && packet[12] == 1 && packet[16] == 48);
    transmit(&channel, 5, 9);
    CHECK(!channel.peer_configured && channel.transmit_mtu == 672);
    request[4] = 0x7f;
    CHECK(deliver(&channel, 4, 10, request, 8, 100) == 1);
    length = btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 100);
    CHECK(length == 16 && packet[12] == 3 && packet[14] == 0x7f && packet[15] == 0);
    transmit(&channel, 5, 10);
    request[4] = 0xff; /* Unknown hint is optional. */
    CHECK(deliver(&channel, 4, 11, request, 8, 100) == 1);
    transmit(&channel, 5, 11);
    CHECK(channel.peer_configured && channel.transmit_mtu == 672);

    begin(&channel);
    request[4] = 1;
    request[6] = 0;
    request[7] = 4;
    request[2] = 1; /* Unsupported continuation is rejected, never opened. */
    CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
    CHECK(btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 100) == 14);
    CHECK(packet[12] == 2);
    transmit(&channel, 5, 9);
    CHECK(!channel.peer_configured);
    request[2] = 0;
    transmit(&channel, 4, 2);
    response[4] = 1;
    CHECK(deliver(&channel, 5, 2, response, 6, 100) == -1);
    CHECK(channel.state == BTV_CHANNEL_FAILED);
    CHECK(btv_l2cap_channel_disconnect(&channel, 3, 100) == 1);
    transmit(&channel, 6, 3);
    CHECK(deliver(&channel, 7, 3, close_response, 4, 100) == 1);
    response[4] = 0;

    btv_l2cap_channel_init(&channel);
    CHECK(!btv_l2cap_channel_connect(&channel, 2, 0x40, 672, 1, 2, 0));
    CHECK(!btv_l2cap_channel_connect(&channel, 3, 1, 672, 1, 2, 0));
    CHECK(!btv_l2cap_channel_connect(&channel, 3, 0x40, 47, 1, 2, 0));
    CHECK(!btv_l2cap_channel_connect(&channel, 3, 0x40, 672, 1, 1, 0));
    CHECK(btv_l2cap_channel_connect(&channel, 1, 0x40, 672, 1, 2, 0xfffffff0UL));
    memset(packet, 0xa5, sizeof(packet));
    memcpy(before, packet, sizeof(packet));
    token = 99;
    CHECK(btv_l2cap_channel_output(&channel, packet, 11, &token, 0xfffffff0UL) == 0);
    CHECK(token == 99 && memcmp(packet, before, sizeof(packet)) == 0);
    btv_l2cap_channel_tick(&channel, 59983);
    CHECK(channel.state == BTV_CHANNEL_CONNECTING);
    btv_l2cap_channel_tick(&channel, 59984);
    CHECK(channel.state == BTV_CHANNEL_FAILED && channel.result == 0xeeee);
    CHECK(btv_l2cap_channel_output(&channel, packet, sizeof(packet), &token, 59984) == 0);
    begin(&channel);
    btv_l2cap_channel_abort(&channel);
    CHECK(channel.state == BTV_CHANNEL_FAILED);
    /* An MTU counteroffer needs a fresh identifier from the ACL owner. */
    {
        uint8_t offer[10] = { 0x40, 0, 0, 0, 1, 0, 1, 2, 0, 2 };
        unsigned int attempt;
        begin(&channel);
        transmit(&channel, 4, 2);
        CHECK(deliver(&channel, 5, 2, offer, 10, 100) == 1);
        CHECK(channel.retry_pending && channel.receive_mtu == 512 && channel.receive_limit == 672);
        CHECK(!channel.local_configured && channel.state == BTV_CHANNEL_CONFIGURING);
        CHECK(!btv_l2cap_channel_retry(&channel, 1, 100));
        CHECK(!btv_l2cap_channel_retry(&channel, 2, 100));
        CHECK(btv_l2cap_channel_retry(&channel, 3, 100));
        CHECK(deliver(&channel, 5, 2, response, 6, 100) == 0);
        transmit(&channel, 4, 3);
        CHECK(deliver(&channel, 5, 3, response, 6, 100) == 1);
        CHECK(deliver(&channel, 4, 9, request, 8, 100) == 1);
        transmit(&channel, 5, 9);
        CHECK(channel.state == BTV_CHANNEL_OPEN && channel.receive_mtu == 512);
        CHECK(!btv_l2cap_channel_disconnect(&channel, 2, 100));
        CHECK(btv_l2cap_channel_disconnect(&channel, 4, 100));
        transmit(&channel, 6, 4);
        CHECK(deliver(&channel, 7, 4, close_response, 4, 100) == 1);

        begin(&channel);
        transmit(&channel, 4, 2);
        offer[9] = 4; /* 1024 exceeds the original receive storage contract. */
        CHECK(deliver(&channel, 5, 2, offer, 10, 100) == -1);
        CHECK(channel.receive_mtu == 672 && channel.state == BTV_CHANNEL_FAILED);
        begin(&channel);
        transmit(&channel, 4, 2);
        offer[9] = 0;
        offer[8] = 47;
        CHECK(deliver(&channel, 5, 2, offer, 10, 100) == -1);
        begin(&channel);
        transmit(&channel, 4, 2);
        for (attempt = 0; attempt < 3; ++attempt)
        {
            offer[8] = (uint8_t)(200 - attempt);
            CHECK(deliver(&channel, 5, (uint8_t)(2 + attempt), offer, 10, 100) == 1);
            CHECK(btv_l2cap_channel_retry(&channel, (uint8_t)(3 + attempt), 100));
            transmit(&channel, 4, (uint8_t)(3 + attempt));
        }
        offer[8] = 100;
        CHECK(deliver(&channel, 5, 5, offer, 10, 100) == -1);
        CHECK(channel.state == BTV_CHANNEL_FAILED && channel.retries == 3);
    }
    if (failures == 0)
        puts("PASS: L2CAP channel setup over ACL, bounded MTU retries, crossed configuration and close");
    return failures;
}
