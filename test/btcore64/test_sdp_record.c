/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "sdp_record.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    printf("SDP record line %u: %s\n", (unsigned int)__LINE__, #x); } } while (0)

unsigned int test_sdp_record(void)
{
    static const uint8_t base[] = {
        0x35, 27, 0x35, 25,
        0x09, 0, 1, 0x35, 3, 0x19, 0x11, 0x1e,
        0x09, 0, 4, 0x35, 12,
        0x35, 3, 0x19, 1, 0,
        0x35, 5, 0x19, 0, 3, 0x08, 7
    };
    static const uint8_t metadata[] = {
        0x09, 0, 9, 0x35, 8, 0x35, 6, 0x19, 0x11, 0x1e, 0x09, 1, 7,
        0x09, 3, 0x11, 0x09, 0, 0x21
    };
    static const uint8_t uuid128[] = {
        0x1c, 0, 0, 0x11, 0x1e, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb
    };
    btv_sdp_service result, sentinel;
    uint8_t buffer[128];
    size_t i;
    failures = 0;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    result = sentinel;
    CHECK(btv_sdp_record_find(base, sizeof(base), 0x111e, &result) == 1);
    CHECK(result.service == 0x111e && result.rfcomm_channel == 7);
    CHECK(!result.has_profile_version && !result.has_features);
    result = sentinel;
    CHECK(btv_sdp_record_find(base, sizeof(base), 0x1108, &result) == 0);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    for (i = 0; i < sizeof(base); ++i)
    {
        CHECK(btv_sdp_record_find(base, i, 0x111e, &result) == -1);
        CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    }
    memcpy(buffer, base, sizeof(base));
    memcpy(buffer + sizeof(base), metadata, sizeof(metadata));
    buffer[1] += (uint8_t)sizeof(metadata);
    buffer[3] += (uint8_t)sizeof(metadata);
    CHECK(btv_sdp_record_find(buffer, sizeof(base) + sizeof(metadata), 0x111e, &result) == 1);
    CHECK(result.has_profile_version && result.profile_version == 0x0107);
    CHECK(result.has_features && result.features == 0x21);
    /* UUID32 and full Bluetooth-base UUID128 aliases identify the same class. */
    memcpy(buffer, base, 9);
    buffer[9] = 0x1a; buffer[10] = 0; buffer[11] = 0; buffer[12] = 0x11; buffer[13] = 0x1e;
    memcpy(buffer + 14, base + 12, sizeof(base) - 12);
    buffer[1] += 2; buffer[3] += 2; buffer[8] = 5;
    CHECK(btv_sdp_record_find(buffer, sizeof(base) + 2, 0x111e, &result) == 1);
    memcpy(buffer, base, 9);
    memcpy(buffer + 9, uuid128, sizeof(uuid128));
    memcpy(buffer + 26, base + 12, sizeof(base) - 12);
    buffer[1] += 14; buffer[3] += 14; buffer[8] = 17;
    CHECK(btv_sdp_record_find(buffer, sizeof(base) + 14, 0x111e, &result) == 1);
    result = sentinel;
    buffer[25] ^= 1;
    CHECK(btv_sdp_record_find(buffer, sizeof(base) + 14, 0x111e, &result) == 0);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    memcpy(buffer, base, sizeof(base));
    buffer[28] = 0;
    CHECK(btv_sdp_record_find(buffer, sizeof(base), 0x111e, &result) == -1);
    buffer[28] = 31;
    CHECK(btv_sdp_record_find(buffer, sizeof(base), 0x111e, &result) == -1);
    buffer[28] = 30;
    CHECK(btv_sdp_record_find(buffer, sizeof(base), 0x111e, &result) == 1);
    memcpy(buffer, base, sizeof(base));
    buffer[14] = 1; /* Duplicate attribute ID. */
    result = sentinel;
    CHECK(btv_sdp_record_find(buffer, sizeof(base), 0x111e, &result) == -1);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    /* Preserve all candidate ambiguity instead of choosing an arbitrary port. */
    memcpy(buffer, base, sizeof(base));
    memcpy(buffer + sizeof(base), base + 2, sizeof(base) - 2);
    buffer[1] = 54;
    CHECK(btv_sdp_record_find(buffer, 56, 0x111e, &result) == -2);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    buffer[38] = 0x08; /* Second record is Headset, not Hands-Free. */
    CHECK(btv_sdp_record_find(buffer, 56, 0x111e, &result) == 1);
    CHECK(result.rfcomm_channel == 7);
    result = sentinel;
    buffer[31] = 0x08; /* Malformed later record must not expose the first. */
    CHECK(btv_sdp_record_find(buffer, 56, 0x111e, &result) == -1);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    buffer[0] = 0x35; buffer[1] = 0;
    CHECK(btv_sdp_record_find(buffer, 2, 0x111e, &result) == 0);
    if (failures == 0)
        puts("PASS: SDP service identity, RFCOMM channel, UUID aliases, metadata and ambiguity");
    return failures;
}
