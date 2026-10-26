/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
#include "hci_security.h"
#include <stdio.h>
#include <string.h>
static unsigned int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL security line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void status(uint8_t event[6], uint16_t opcode, uint8_t value)
{
    event[0] = 0x0f; event[1] = 4; event[2] = value; event[3] = 1;
    event[4] = (uint8_t)opcode; event[5] = (uint8_t)(opcode >> 8);
}

unsigned int test_hci_security(void)
{
    btv_hci_security s;
    uint8_t command[6], event[6];
    failures = 0;
    btv_hci_security_init(&s);
    CHECK(btv_hci_security_start(&s, 0x42, 0));
    CHECK(btv_hci_security_command(&s, command, 4, 0) == 0);
    CHECK(btv_hci_security_command(&s, command, sizeof(command), 0) == 5);
    CHECK(command[0] == 0x11 && command[1] == 4 && command[3] == 0x42);
    CHECK(btv_hci_security_sent(&s, 0));
    status(event, 0x0411, 0);
    CHECK(btv_hci_security_event(&s, event, 6, 1) == 1);
    event[0] = 6; event[1] = 3; event[2] = 0; event[3] = 0x42; event[4] = 0;
    CHECK(btv_hci_security_event(&s, event, 5, 2) == 1);
    CHECK(btv_hci_security_usb_done(&s, 1));
    CHECK(s.state == BTV_SEC_ENCRYPT_QUEUED);
    CHECK(btv_hci_security_command(&s, command, sizeof(command), 3) == 6);
    CHECK(command[0] == 0x13 && command[1] == 4 && command[5] == 1);
    CHECK(btv_hci_security_sent(&s, 3));
    CHECK(btv_hci_security_usb_done(&s, 1));
    status(event, 0x0413, 0);
    CHECK(btv_hci_security_event(&s, event, 6, 4) == 1);
    event[0] = 8; event[1] = 4; event[2] = 0;
    event[3] = 0x42; event[4] = 0; event[5] = 1;
    CHECK(btv_hci_security_event(&s, event, 6, 5) == 1);
    CHECK(btv_hci_security_ready(&s));

    /* Newer BR/EDR controllers can report AES-CCM with value 2. */
    btv_hci_security_init(&s); CHECK(btv_hci_security_start(&s, 0x42, 0));
    CHECK(btv_hci_security_sent(&s, 0)); status(event, 0x0411, 0);
    CHECK(btv_hci_security_event(&s, event, 6, 1) == 1);
    CHECK(btv_hci_security_usb_done(&s, 1));
    event[0] = 6; event[1] = 3; event[2] = 0; event[3] = 0x42; event[4] = 0;
    CHECK(btv_hci_security_event(&s, event, 5, 2) == 1);
    CHECK(btv_hci_security_sent(&s, 3)); CHECK(btv_hci_security_usb_done(&s, 1));
    status(event, 0x0413, 0); CHECK(btv_hci_security_event(&s, event, 6, 4) == 1);
    event[0] = 8; event[1] = 4; event[2] = 0; event[3] = 0x42;
    event[4] = 0; event[5] = 2;
    CHECK(btv_hci_security_event(&s, event, 6, 5) == 1);
    CHECK(btv_hci_security_ready(&s));

    btv_hci_security_init(&s); CHECK(btv_hci_security_start(&s, 1, 0));
    CHECK(btv_hci_security_sent(&s, 0));
    status(event, 0x0411, 5);
    CHECK(btv_hci_security_event(&s, event, 6, 1) == 1);
    CHECK(btv_hci_security_usb_done(&s, 1));
    CHECK(s.state == BTV_SEC_REJECTED && s.error == BTV_SEC_COMMAND_ERROR);
    btv_hci_security_init(&s); CHECK(btv_hci_security_start(&s, 1, 0));
    CHECK(btv_hci_security_sent(&s, 0));
    CHECK(btv_hci_security_usb_done(&s, 0) == -1);
    CHECK(s.state == BTV_SEC_RECOVER && s.error == BTV_SEC_USB_ERROR);
    btv_hci_security_init(&s); CHECK(btv_hci_security_start(&s, 1, 0));
    btv_hci_security_tick(&s, 29999); CHECK(s.state == BTV_SEC_AUTH_QUEUED);
    btv_hci_security_tick(&s, 30000); CHECK(s.error == BTV_SEC_TIMEOUT);
    if (!failures) puts("PASS: ACL authentication and encryption evidence, order permutations, rejection and recovery");
    return failures;
}
