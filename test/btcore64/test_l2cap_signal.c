/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "l2cap_signal.h"
#include <stdio.h>
#include <string.h>

static unsigned int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    printf("L2CAP signal line %u: %s\n", (unsigned int)__LINE__, #x); } } while (0)

unsigned int test_l2cap_signal(void)
{
    uint8_t pdu[80], original[80], pair[80];
    uint8_t connect[4] = { 3, 0, 0x40, 0 };
    uint8_t config[8] = { 0x41, 0, 0, 0, 1, 2, 0xa0, 2 };
    uint8_t malformed[7] = { 0x41, 0, 0, 0, 1, 2, 0xa0 };
    btv_l2cap_signal signal, saved;
    btv_l2cap_command command, untouched;
    size_t n, i;
    failures = 0;
    memset(&signal, 0, sizeof(signal));
    memset(&command, 0, sizeof(command));
    n = btv_l2cap_signal_encode(pdu, sizeof(pdu), 2, 17, connect, 4);
    CHECK(n == 12 && pdu[0] == 8 && pdu[2] == 1);
    CHECK(btv_l2cap_signal_open(&signal, pdu, n) == 1);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 1);
    CHECK(command.code == 2 && command.identifier == 17 && command.bytes == 4);
    CHECK(memcmp(command.parameters, connect, 4) == 0);
    untouched = command;
    CHECK(btv_l2cap_signal_next(&signal, &command) == 0);
    CHECK(memcmp(&command, &untouched, sizeof(command)) == 0);
    saved = signal;
    for (i = 0; i < n; ++i)
    {
        CHECK(btv_l2cap_signal_open(&signal, pdu, i) == -1);
        CHECK(memcmp(&signal, &saved, sizeof(signal)) == 0);
    }
    pdu[2] = 0x40;
    CHECK(btv_l2cap_signal_open(&signal, pdu, n) == 0);
    CHECK(memcmp(&signal, &saved, sizeof(signal)) == 0);
    pdu[2] = 1;
    memcpy(pair, pdu, n);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 4, 18, config, 8) == 16);
    memcpy(pair + n, pdu + 4, 12);
    pair[0] = 20;
    CHECK(btv_l2cap_signal_open(&signal, pair, 24) == 1);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 1 && command.code == 2);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 1 && command.code == 4);
    CHECK(command.bytes == 8 && command.identifier == 18);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 0);
    saved = signal;
    /* Validate the entire batch before dispatching its valid first command. */
    pair[13] = 0;
    CHECK(btv_l2cap_signal_open(&signal, pair, 24) == -1);
    CHECK(memcmp(&signal, &saved, sizeof(signal)) == 0);
    pair[13] = 18;
    pair[14] = 13;
    CHECK(btv_l2cap_signal_open(&signal, pair, 24) == -1);
    pair[14] = 8;
    pair[21] = 3;
    CHECK(btv_l2cap_signal_open(&signal, pair, 24) == -1);
    memset(pdu, 0xa5, sizeof(pdu));
    memcpy(original, pdu, sizeof(pdu));
    for (i = 0; i < 12; ++i)
    {
        CHECK(btv_l2cap_signal_encode(pdu, i, 2, 1, connect, 4) == 0);
        CHECK(memcmp(pdu, original, sizeof(pdu)) == 0);
    }
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 4, 1, malformed, 7) == 0);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 2, 0, connect, 4) == 0);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 2, 1, NULL, 4) == 0);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 8, 1, connect, 65532U) == 0);
    CHECK(memcmp(pdu, original, sizeof(pdu)) == 0);
    /* Unknown codes/options survive for the negotiator to reject or ignore. */
    config[4] = 0x7f;
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 4, 1, config, 8) == 16);
    CHECK(btv_l2cap_signal_open(&signal, pdu, 16) == 1);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 0xff, 255, NULL, 0) == 8);
    CHECK(btv_l2cap_signal_open(&signal, pdu, 8) == 1);
    CHECK(btv_l2cap_signal_next(&signal, &command) == 1 && command.code == 255);
    CHECK(btv_l2cap_signal_encode(pdu, sizeof(pdu), 8, 1, NULL, 0) == 8);
    CHECK(btv_l2cap_signal_open(&signal, pdu, 8) == 1);
    pdu[0] = 0;
    CHECK(btv_l2cap_signal_open(&signal, pdu, 4) == -1);
    if (failures == 0)
        puts("PASS: L2CAP signaling batches, bounds, options and transactional encoding");
    return failures;
}
