/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for the pure protocol halves of BluetoothVoice: HCI inquiry
 * and pairing state machines, L2CAP signalling, channels and dispatch, and
 * SDP. These are the stack's own unit tests, carried over from where it was
 * written; each file returns its count of failures.
 */

#include <stdio.h>

unsigned int test_hci_inquiry(void);
unsigned int test_hci_pairing(void);
unsigned int test_hci_security(void);
unsigned int test_l2cap_signal(void);
unsigned int test_l2cap_channel(void);
unsigned int test_l2cap_dispatch(void);
unsigned int test_sdp(void);
unsigned int test_sdp_record(void);

int main(void)
{
	unsigned int failures = 0;

	failures += test_hci_inquiry();
	failures += test_hci_pairing();
	failures += test_hci_security();
	failures += test_l2cap_signal();
	failures += test_l2cap_channel();
	failures += test_l2cap_dispatch();
	failures += test_sdp();
	failures += test_sdp_record();
	if (failures == 0)
		printf("btpure: all protocol checks passed\n");
	else
		printf("btpure: %u FAILED\n", failures);
	return failures != 0;
}
