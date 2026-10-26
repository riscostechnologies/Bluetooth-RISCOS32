/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "hci_pairing.h"
#include <stdio.h>
#include <string.h>
static unsigned int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL pairing line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void request(uint8_t *event, uint8_t type, const uint8_t address[6])
{
    event[0] = type; event[1] = type == 0x33 ? 10 : 6;
    memcpy(event + 2, address, 6);
    if (type == 0x33) memset(event + 8, 0, 4);
}

unsigned int test_hci_pairing(void)
{
    btv_hci_pairing p;
    uint8_t address[6] = {1,2,3,4,5,6}, other[6] = {6,5,4,3,2,1};
    uint8_t key[16], event[25], output[32], sentinel[32];
    unsigned int i;
    int bytes;
    failures = 0;
    for (i = 0; i < 16; ++i) key[i] = (uint8_t)(0x80 + i);
    CHECK(btv_hci_pairing_init(&p, address, (const uint8_t *)"0000", 4,
                               key, 4, 1));
    memset(output, 0xa5, sizeof(output)); memcpy(sentinel, output, sizeof(output));
    request(event, 0x17, other);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == 0);
    CHECK(memcmp(output, sentinel, sizeof(output)) == 0);
    request(event, 0x17, address);
    bytes = btv_hci_pairing_event(&p, event, 8, output, sizeof(output));
    CHECK(bytes == 25 && output[0] == 0x0b && output[1] == 4);
    CHECK(memcmp(output + 3, address, 6) == 0 && memcmp(output + 9, key, 16) == 0);
    request(event, 0x16, address);
    bytes = btv_hci_pairing_event(&p, event, 8, output, sizeof(output));
    CHECK(bytes == 26 && output[0] == 0x0d && output[9] == 4);
    CHECK(memcmp(output + 10, "0000", 4) == 0);
    request(event, 0x31, address);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == 12);
    CHECK(output[0] == 0x2b && output[9] == 3 && output[11] == 2);
    request(event, 0x33, address);
    CHECK(btv_hci_pairing_event(&p, event, 12, output, sizeof(output)) == 9);
    CHECK(output[0] == 0x2c && output[1] == 4);
    request(event, 0x34, address);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == 9);
    CHECK(output[0] == 0x2f && output[1] == 4);
    memset(event, 0, sizeof(event)); event[0] = 0x18; event[1] = 23;
    memcpy(event + 2, address, 6); memcpy(event + 8, key, 16); event[24] = 5;
    CHECK(btv_hci_pairing_event(&p, event, 25, output, sizeof(output)) == 0);
    CHECK(p.have_link_key && p.link_key_type == 5 && memcmp(p.link_key, key, 16) == 0);
    memset(event, 0, 9); event[0] = 0x36; event[1] = 7; event[2] = 0;
    memcpy(event + 3, address, 6);
    CHECK(btv_hci_pairing_event(&p, event, 9, output, sizeof(output)) == 0);
    CHECK(p.pairing_complete && p.pairing_status == 0);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == -1);
    request(event, 0x17, address);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, 8) == -1);
    CHECK(btv_hci_pairing_init(&p, address, NULL, 0, NULL, 0, 0));
    request(event, 0x17, address);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == 9 && output[0] == 0x0c);
    request(event, 0x16, address);
    CHECK(btv_hci_pairing_event(&p, event, 8, output, sizeof(output)) == 9 && output[0] == 0x0e);
    request(event, 0x33, address);
    CHECK(btv_hci_pairing_event(&p, event, 12, output, sizeof(output)) == 9 && output[0] == 0x2d);
    if (!failures) puts("PASS: bounded link-key, PIN, SSP capability/confirmation policy and key capture");
    return failures;
}
