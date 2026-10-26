/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "hci_inquiry.h"
#include <stdio.h>
#include <string.h>
static unsigned int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL inquiry line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void begin(btv_hci_inquiry *q)
{
    uint8_t command[8];
    static const uint8_t status[] = {0x0f, 4, 0, 1, 1, 4};
    btv_hci_inquiry_init(q);
    CHECK(btv_hci_inquiry_start(q, 8, 0, 100));
    CHECK(btv_hci_inquiry_command(q, command, 7, 100) == 0);
    CHECK(btv_hci_inquiry_command(q, command, sizeof(command), 100) == 8);
    CHECK(command[0] == 1 && command[1] == 4 && command[2] == 5);
    CHECK(command[3] == 0x33 && command[4] == 0x8b && command[5] == 0x9e);
    CHECK(command[6] == 8 && command[7] == 0);
    CHECK(btv_hci_inquiry_sent(q, 100));
    CHECK(btv_hci_inquiry_event(q, status, sizeof(status), 101) == 1);
    CHECK(q->state == BTV_IQ_ACTIVE);
}

unsigned int test_hci_inquiry(void)
{
    btv_hci_inquiry q;
    const btv_inquiry_device *d;
    uint8_t basic[31], rssi[17], extended[257], damaged[257];
    static const uint8_t complete[] = {1, 1, 0};
    static const uint8_t rejected[] = {0x0f, 4, 0x0c, 1, 1, 4};
    unsigned int i;
    failures = 0;
    btv_hci_inquiry_init(&q);
    CHECK(!btv_hci_inquiry_start(&q, 0, 0, 0));
    CHECK(!btv_hci_inquiry_start(&q, 0x31, 0, 0));
    begin(&q);
    memset(basic, 0, sizeof(basic));
    basic[0] = 2; basic[1] = 29; basic[2] = 2;
    for (i = 0; i < 6; ++i) basic[3 + i] = (uint8_t)(i + 1);
    basic[9] = 2; basic[12] = 4; basic[13] = 4;
    basic[15] = 0x34; basic[16] = 0x12;
    memcpy(basic + 17, basic + 3, 14); /* duplicate is updated, not appended */
    basic[17 + 9] = 8;
    CHECK(btv_hci_inquiry_event(&q, basic, sizeof(basic), 102) == 1);
    CHECK(q.count == 1 && q.devices[0].audio_class);
    CHECK(q.devices[0].clock_offset == 0x1234);
    memset(rssi, 0, sizeof(rssi));
    rssi[0] = 0x22; rssi[1] = 15; rssi[2] = 1;
    for (i = 0; i < 6; ++i) rssi[3 + i] = (uint8_t)(10 + i);
    rssi[11] = 4; rssi[16] = (uint8_t)-42;
    CHECK(btv_hci_inquiry_event(&q, rssi, sizeof(rssi), 103) == 1);
    CHECK(q.count == 2 && q.devices[1].has_rssi && q.devices[1].rssi == -42);
    memset(extended, 0, sizeof(extended));
    extended[0] = 0x2f; extended[1] = 255; extended[2] = 1;
    for (i = 0; i < 6; ++i) extended[3 + i] = (uint8_t)(20 + i);
    extended[12] = 4; extended[16] = (uint8_t)-10;
    extended[17] = 6; extended[18] = 0x09;
    memcpy(extended + 19, "Voice", 5);
    extended[24] = 5; extended[25] = 0x03;
    extended[26] = 0x1e; extended[27] = 0x11;
    extended[28] = 0x08; extended[29] = 0x11;
    CHECK(btv_hci_inquiry_event(&q, extended, sizeof(extended), 104) == 1);
    CHECK(q.count == 3 && q.devices[2].extended && q.devices[2].voice_service &&
          q.devices[2].audio_class);
    CHECK(strcmp(q.devices[2].name, "Voice") == 0);
    CHECK(btv_hci_inquiry_usb_done(&q, 1));
    CHECK(btv_hci_inquiry_event(&q, complete, sizeof(complete), 105) == 1);
    CHECK(q.state == BTV_IQ_DONE);
    d = btv_hci_inquiry_result(&q, 2);
    CHECK(d != NULL && d->address[0] == 20);
    CHECK(btv_hci_inquiry_result(&q, 3) == NULL);

    begin(&q);
    CHECK(btv_hci_inquiry_event(&q, complete, sizeof(complete), 102) == 1);
    CHECK(q.state == BTV_IQ_ACTIVE);
    CHECK(btv_hci_inquiry_usb_done(&q, 1) && q.state == BTV_IQ_DONE);
    btv_hci_inquiry_init(&q); CHECK(btv_hci_inquiry_start(&q, 1, 0, 0));
    CHECK(btv_hci_inquiry_sent(&q, 0));
    CHECK(btv_hci_inquiry_event(&q, rejected, sizeof(rejected), 1) == 1);
    CHECK(q.state == BTV_IQ_REJECTED && q.error == BTV_IQ_COMMAND_ERROR);
    begin(&q); memcpy(damaged, extended, sizeof(damaged)); damaged[1] = 254;
    CHECK(btv_hci_inquiry_event(&q, damaged, sizeof(damaged), 102) == -1);
    CHECK(q.state == BTV_IQ_RECOVER && q.error == BTV_IQ_BAD_EVENT);
    btv_hci_inquiry_init(&q); CHECK(btv_hci_inquiry_start(&q, 1, 0, 0));
    btv_hci_inquiry_tick(&q, 6279); CHECK(q.state == BTV_IQ_QUEUED);
    btv_hci_inquiry_tick(&q, 6280); CHECK(q.error == BTV_IQ_TIMEOUT);
    if (!failures) puts("PASS: bounded HCI inquiry, deduplication, RSSI, EIR voice metadata and event ordering");
    return failures;
}
