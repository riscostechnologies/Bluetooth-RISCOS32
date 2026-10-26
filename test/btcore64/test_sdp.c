/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "sdp_query.h"
#include "sdp_record.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    printf("SDP line %u: %s\n", (unsigned int)__LINE__, #x); } } while (0)

/* One Hands-Free service, with L2CAP then RFCOMM server channel 7. */
static const uint8_t attributes[] = {
    0x35, 27, 0x35, 25,
    0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x1e,
    0x09, 0, 4, 0x35, 12,
    0x35, 3, 0x19, 1, 0,
    0x35, 5, 0x19, 0, 3, 0x08, 7
};

static size_t response(uint8_t *pdu, uint16_t transaction,
                       const uint8_t *data, size_t bytes, int more)
{
    pdu[0] = 7;
    pdu[1] = (uint8_t)(transaction >> 8);
    pdu[2] = (uint8_t)transaction;
    pdu[3] = 0;
    pdu[4] = (uint8_t)(bytes + 3 + (more ? 3 : 0));
    pdu[5] = 0;
    pdu[6] = (uint8_t)bytes;
    memcpy(pdu + 7, data, bytes);
    pdu[7 + bytes] = more ? 3 : 0;
    if (more)
    {
        pdu[8 + bytes] = 0xaa;
        pdu[9 + bytes] = 0xbb;
        pdu[10 + bytes] = 0xcc;
    }
    return bytes + 8 + (more ? 3 : 0);
}

static void begin(btv_sdp_query *query, uint8_t *storage, size_t capacity,
                   uint16_t transaction, uint16_t mtu)
{
    uint8_t pdu[64], second[64];
    size_t length;
    btv_sdp_query_init(query);
    CHECK(btv_sdp_query_start(query, 0x111e, transaction, mtu, storage, capacity, 100));
    memset(pdu, 0xa5, sizeof(pdu));
    memcpy(second, pdu, sizeof(pdu));
    CHECK(btv_sdp_query_request(query, pdu, 26, 100) == 0);
    CHECK(memcmp(pdu, second, sizeof(pdu)) == 0);
    length = btv_sdp_query_request(query, pdu, sizeof(pdu), 100);
    CHECK(length == 27 && pdu[0] == 6 && pdu[3] == 0 && pdu[4] == 22);
    CHECK(pdu[8] == 0x11 && pdu[9] == 0x1e && pdu[26] == 0);
    CHECK(pdu[12] == 0x35 && pdu[13] == 12 && pdu[24] == 3 && pdu[25] == 0x11);
    CHECK(btv_sdp_query_request(query, second, sizeof(second), 100) == length);
    CHECK(memcmp(pdu, second, length) == 0);
    CHECK(btv_sdp_query_sent(query));
    CHECK(!btv_sdp_query_sent(query));
}

unsigned int test_sdp(void)
{
    btv_sdp_query query;
    btv_sdp_element element, saved;
    btv_sdp_service service;
    uint8_t storage[256], pdu[256], request[64], nested[40];
    uint8_t huge[] = { 0x37, 0xff, 0xff, 0xff, 0xff };
    uint8_t bad_uuid[] = { 0x18, 0 };
    uint8_t empty[] = { 0x35, 0 };
    size_t split, length, size, i;
    failures = 0;
    CHECK(btv_sdp_data_valid(attributes, sizeof(attributes)));
    memset(&element, 0, sizeof(element));
    saved = element;
    for (i = 0; i < sizeof(attributes); ++i)
    {
        CHECK(!btv_sdp_data_valid(attributes, i));
        CHECK(!btv_sdp_element_read(attributes, i, &element));
        CHECK(memcmp(&element, &saved, sizeof(element)) == 0);
    }
    CHECK(!btv_sdp_data_valid(huge, sizeof(huge)));
    CHECK(!btv_sdp_data_valid(bad_uuid, sizeof(bad_uuid)));
    CHECK(btv_sdp_data_valid(empty, sizeof(empty)));
    CHECK(!btv_sdp_data_valid(NULL, 0));
    for (i = 0; i < 17; ++i)
    {
        nested[i * 2] = 0x35;
        nested[i * 2 + 1] = (uint8_t)(32 - i * 2);
    }
    CHECK(!btv_sdp_data_valid(nested, 34));
    CHECK(btv_sdp_data_valid(nested + 2, 32));
    /* Split the attribute stream at every byte, even inside element headers. */
    for (split = 1; split < sizeof(attributes); ++split)
    {
        begin(&query, storage, sizeof(storage), 0x1234, 672);
        length = response(pdu, 0x1234, attributes, split, 1);
        CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == 1);
        CHECK(query.state == BTV_SDP_READY && query.transaction == 0x1235);
        memset(pdu, 0xa5, length);
        size = 99;
        CHECK(btv_sdp_query_result(&query, &size) == NULL && size == 99);
        CHECK(btv_sdp_query_request(&query, request, sizeof(request), 100) == 30);
        CHECK(request[1] == 0x12 && request[2] == 0x35 && request[26] == 3);
        CHECK(request[27] == 0xaa && request[28] == 0xbb && request[29] == 0xcc);
        CHECK(btv_sdp_query_sent(&query));
        length = response(pdu, 0x1234, attributes + split, sizeof(attributes) - split, 0);
        CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == 0);
        CHECK(query.state == BTV_SDP_WAIT && query.used == split);
        pdu[2] = 0x35;
        CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == 1);
        CHECK(query.state == BTV_SDP_DONE);
        CHECK(btv_sdp_query_result(&query, &size) == storage && size == sizeof(attributes));
        CHECK(memcmp(storage, attributes, size) == 0);
        CHECK(btv_sdp_record_find(storage, size, query.service, &service) == 1);
        CHECK(service.rfcomm_channel == 7 && service.service == 0x111e);
    }
    begin(&query, storage, sizeof(storage), 1, 672);
    length = response(pdu, 1, empty, sizeof(empty), 0);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == 1 && query.state == BTV_SDP_DONE);
    /* Capacity and advertised per-response limits are distinct. */
    begin(&query, storage, sizeof(attributes) - 1, 1, 672);
    length = response(pdu, 1, attributes, sizeof(attributes), 0);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1);
    CHECK(query.used == 0);
    begin(&query, storage, sizeof(storage), 1, 48);
    CHECK(query.maximum == 24);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1);
    begin(&query, storage, sizeof(storage), 65535, 672);
    length = response(pdu, 65535, attributes, 1, 1);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1 && query.used == 0);
    begin(&query, storage, sizeof(storage), 1, 672);
    length = response(pdu, 1, attributes, 1, 1);
    pdu[length - 4] = 17;
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1 && query.used == 0);
    begin(&query, storage, sizeof(storage), 1, 672);
    length = response(pdu, 1, attributes, sizeof(attributes), 0);
    pdu[7 + 3] = 255; /* Corrupted inner record length. */
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1);
    size = 99;
    CHECK(btv_sdp_query_result(&query, &size) == NULL && size == 99);
    /* Remote SDP error is preserved but never exposed as a service result. */
    begin(&query, storage, sizeof(storage), 1, 672);
    pdu[0] = 1; pdu[1] = 0; pdu[2] = 1; pdu[3] = 0; pdu[4] = 2;
    pdu[5] = 0; pdu[6] = 3;
    CHECK(btv_sdp_query_receive(&query, pdu, 7, 100) == -1 && query.error == 3);
    begin(&query, storage, sizeof(storage), 1, 672);
    query.pages = 63;
    length = response(pdu, 1, attributes, 1, 1);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 100) == -1);
    btv_sdp_query_init(&query);
    CHECK(btv_sdp_query_start(&query, 0x1108, 0, 48, storage, sizeof(storage), 0xfffffff0UL));
    btv_sdp_query_tick(&query, 59983);
    CHECK(query.state == BTV_SDP_READY);
    btv_sdp_query_tick(&query, 59984);
    CHECK(query.state == BTV_SDP_FAILED && query.error == 0xfffe);
    CHECK(btv_sdp_query_receive(&query, pdu, length, 59984) == -1);
    if (failures == 0)
        puts("PASS: SDP service query, every continuation split, element bounds and transaction lifetime");
    return failures;
}
