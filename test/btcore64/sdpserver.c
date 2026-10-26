/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for the SDP server (c/bt_sdp_server.c): the three records it
 * publishes (A2DP Audio Source, AVRCP Controller, AVRCP Target), and the
 * three request kinds a device uses to find them.  Every response is
 * checked to be a well formed data element tree by the pure validator
 * (c/sdp_data.c), which the SDP client of the older stack uses.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt_core.h"
#include "check.h"
#include "sdp_data.h"

static uint8_t last[800];
static size_t last_bytes;

int btc_l2cap_send(btc_core *core, int channel, const uint8_t *data, size_t bytes)
{
	(void)core; (void)channel;
	memcpy(last, data, bytes);
	last_bytes = bytes;
	return 1;
}

static btc_core *core_for_test;

static void ask(const uint8_t *request, size_t bytes)
{
	last_bytes = 0;
	btc_sdp_server_request(core_for_test, 0, request, bytes);
}

/* ServiceSearchAttributeRequest for one UUID16, all attributes. */
static size_t ssa(uint8_t *r, uint16_t uuid, uint16_t transaction)
{
	size_t n = 0;

	r[n++] = 0x06; r[n++] = (uint8_t)(transaction >> 8); r[n++] = (uint8_t)transaction;
	r[n++] = 0; r[n++] = 0;                          /* length, set below */
	r[n++] = 0x35; r[n++] = 3; r[n++] = 0x19; r[n++] = (uint8_t)(uuid >> 8);
	r[n++] = (uint8_t)uuid;
	r[n++] = 0x02; r[n++] = 0x00;                    /* max bytes */
	r[n++] = 0x35; r[n++] = 5; r[n++] = 0x0a;        /* attributes 0 to 0xFFFF */
	r[n++] = 0x00; r[n++] = 0x00; r[n++] = 0xff; r[n++] = 0xff;
	r[n++] = 0;                                      /* no continuation */
	r[3] = (uint8_t)((n - 5) >> 8); r[4] = (uint8_t)(n - 5);
	return n;
}

/* The attribute lists in a ServiceSearchAttribute response: how many
 * records, and whether the whole is well formed. */
static int records_in(unsigned int *count, const uint8_t **first)
{
	size_t list_bytes, off;
	btv_sdp_element e;

	if (last_bytes < 9 || last[0] != 0x07) return 0;
	list_bytes = (size_t)(last[5] << 8 | last[6]);
	if (!btv_sdp_data_valid(last + 7, list_bytes)) return 0;
	if (!btv_sdp_element_read(last + 7, list_bytes, &e)) return 0;
	*count = 0;
	*first = 0;
	for (off = 0; off < e.bytes;) {
		btv_sdp_element r;

		if (!btv_sdp_element_read(e.data + off, e.bytes - off, &r)) return 0;
		if (*count == 0) *first = e.data + off;
		++*count;
		off += r.total;
	}
	return 1;
}

static int contains(const uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
	size_t i;

	for (i = 0; i + m <= n; i++)
		if (memcmp(p + i, pat, m) == 0) return 1;
	return 0;
}

int main(void)
{
	static btc_core core;
	uint8_t r[64];
	size_t n;
	unsigned int count;
	const uint8_t *first;
	static const uint8_t avctp_psm[] = { 0x19, 0x01, 0x00, 0x09, 0x00, 0x17 };
	static const uint8_t avdtp_psm[] = { 0x19, 0x01, 0x00, 0x09, 0x00, 0x19 };

	core_for_test = &core;
	n = ssa(r, 0x110a, 1);
	ask(r, n);
	CHECK("Audio Source: one record, well formed", records_in(&count, &first) && count == 1);
	CHECK("on the AVDTP PSM (25)", contains(last, last_bytes, avdtp_psm, sizeof avdtp_psm));
	CHECK("and echoing the transaction", last[1] == 0 && last[2] == 1);

	n = ssa(r, 0x110e, 2);
	ask(r, n);
	CHECK("AV remote control profile: two records (controller and target)",
	      records_in(&count, &first) && count == 2);
	CHECK("on the AVCTP PSM (23)", contains(last, last_bytes, avctp_psm, sizeof avctp_psm));

	n = ssa(r, 0x110f, 3);
	ask(r, n);
	CHECK("Remote Control Controller class: one record",
	      records_in(&count, &first) && count == 1);
	n = ssa(r, 0x110c, 4);
	ask(r, n);
	CHECK("Remote Control Target class: one record",
	      records_in(&count, &first) && count == 1);
	n = ssa(r, 0x1002, 5);
	ask(r, n);
	CHECK("the public browse group: all three",
	      records_in(&count, &first) && count == 3);
	CHECK("a list over 255 octets uses the 16-bit sequence length", last[7] == 0x36);
	n = ssa(r, 0x110b, 6);
	ask(r, n);
	CHECK("an Audio Sink search finds nothing (empty list, still well formed)",
	      records_in(&count, &first) && count == 0);

	{
		/* ServiceSearchRequest [0x110E]: the two handles. */
		static const uint8_t search[] = { 0x02, 0x00, 0x09, 0x00, 0x08, 0x35, 0x03,
						  0x19, 0x11, 0x0e, 0x00, 0x10, 0x00 };

		ask(search, sizeof search);
		CHECK("ServiceSearch: handles 0x10001 and 0x10002",
		      last[0] == 0x03 && last[6] == 2 &&
		      last[9] == 0x00 && last[10] == 0x01 && last[11] == 0x00 &&
		      last[12] == 0x01 && last[16] == 0x02);
	}
	{
		/* ServiceAttributeRequest, handle 0x10002, attribute 0x0001. */
		static const uint8_t attr[] = { 0x04, 0x00, 0x0a, 0x00, 0x0d, 0x00, 0x01, 0x00, 0x02,
						0x00, 0x20, 0x35, 0x03, 0x09, 0x00, 0x01, 0x00 };
		static const uint8_t cls[] = { 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0c };

		ask(attr, sizeof attr);
		CHECK("ServiceAttribute: the Target's class list", last[0] == 0x05 &&
		      contains(last, last_bytes, cls, sizeof cls));
	}
	{
		static const uint8_t bad[] = { 0x04, 0x00, 0x0b, 0x00, 0x0d, 0x00, 0x09, 0x99, 0x99,
					       0x00, 0x20, 0x35, 0x03, 0x09, 0x00, 0x01, 0x00 };

		ask(bad, sizeof bad);
		CHECK("a handle that is not ours: error response, invalid handle (2)",
		      last[0] == 0x01 && last[5] == 0x00 && last[6] == 0x02);
	}
	return check_summary("sdpserver");
}
