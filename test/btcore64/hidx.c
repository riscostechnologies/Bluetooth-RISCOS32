/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for what classic HID needs beyond boot reports: the SDP
 * client that fetches a device's record (with continuation), the record's
 * HID descriptor (hid_sdp.c), consumer keys and game controllers in a
 * report descriptor (hid_extras.c), and the controller's keys (hid_pad.c).
 *
 * The descriptors are written here from the USB HID 1.11 item encoding and
 * the HID Usage Tables (as mxmap.h's are), not copied from a device; the
 * SDP record from the Bluetooth HID Profile 1.1.1 attribute list.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt_sdpc.h"
#include "check.h"
#include "hid_extras.h"
#include "hid_pad.h"
#include "hid_sdp.h"
#include "hidtest.h"

/* ---- the controller's keys ---- */
struct keylog { int log[40]; int n; };
static void on_key(void *c, int down, unsigned int key)
{
	struct keylog *l = c;

	if (l->n < 40) l->log[l->n++] = down ? (int)key : -(int)key - 1;
}

static void parse(const uint8_t *d, size_t n, btv_hid_extras *x)
{
	btv_hid_extras_parser p;
	size_t i;

	btv_hid_extras_init(x, &p);
	/* In pieces of 22, as a Report Map is read. */
	for (i = 0; i < n; i += 22)
		btv_hid_extras_feed(x, &p, d + i, n - i < 22 ? n - i : 22);
}

static void consumer(void)
{
	btv_hid_extras x;
	btv_hid_extra_event e;
	static const uint8_t r_array[] = { 0xe9, 0x00 };          /* Volume Increment */
	static const uint8_t r_bits[] = { 0x0a };                 /* down and play/pause */

	printf("consumer keys\n");
	parse(consumer_array, sizeof consumer_array, &x);
	CHECK("an array of 16-bit usages is found (report 2)",
	      x.consumer_used == 1 && x.consumer[0].kind == BTV_XC_ARRAY &&
	      x.consumer[0].id == 2 && x.consumer[0].size == 16);
	CHECK("it is wanted for report 2 and not 1", btv_hid_extras_wants(&x, 2) &&
	      !btv_hid_extras_wants(&x, 1));
	CHECK("a report with Volume Increment (0xE9)",
	      btv_hid_extras_decode(&x, 2, r_array, sizeof r_array, &e) &&
	      e.consumer && e.consumer_count == 1 && e.consumer_usage[0] == 0xe9);
	CHECK("a report with no key down has none",
	      btv_hid_extras_decode(&x, 2, (const uint8_t *)"\0\0", 2, &e) &&
	      e.consumer_count == 0);
	parse(consumer_bits, sizeof consumer_bits, &x);
	CHECK("bits for four usages are found (report 3)",
	      x.consumer[0].kind == BTV_XC_BITS && x.consumer[0].count == 4 &&
	      x.consumer[0].usage[0] == 0xe9 && x.consumer[0].usage[3] == 0xcd);
	CHECK("bits 1 and 3 down are Volume Decrement and Play/Pause",
	      btv_hid_extras_decode(&x, 3, r_bits, sizeof r_bits, &e) &&
	      e.consumer_count == 2 && e.consumer_usage[0] == 0xea &&
	      e.consumer_usage[1] == 0xcd);
	CHECK("a pointer or keyboard map has none", ({
		btv_hid_extras y;
		static const uint8_t kbd[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07,
			0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
			0x81, 0x02, 0xc0 };
		parse(kbd, sizeof kbd, &y);
		y.consumer_used == 0 && !y.pad_used;
	}));
}

static void pad(void)
{
	btv_hid_extras x;
	btv_hid_extra_event e;
	btv_hid_pad p;
	btv_hid_sink sink;
	struct keylog log;
	/* id 1; X = 0xFFFF, Y = 0x8000 (centred); hat nibble 3 (right) + 4 spare;
	 * buttons 1 and 3 (bits 0 and 2), 6 spare. */
	static const uint8_t report[] = { 0xff, 0xff, 0x00, 0x80, 0x03, 0x05, 0x00 };
	static const uint8_t centred[] = { 0x00, 0x80, 0x00, 0x80, 0x0f, 0x00, 0x00 };

	printf("game controller\n");
	parse(gamepad, sizeof gamepad, &x);
	CHECK("a game pad is found: report 1, two axes, a hat, ten buttons",
	      x.pad_used && x.pad_id == 1 && x.axis[0].used && x.axis[1].used &&
	      x.hat.used && x.pad_buttons == 10 && x.pad_application == 5);
	CHECK("the axes are 16 bits wide, unsigned", x.axis[0].size == 16 &&
	      x.axis[0].minimum == 0 && x.axis[0].maximum == 65535);
	CHECK("report 1 is wanted", btv_hid_extras_wants(&x, 1));
	CHECK("a report decodes", btv_hid_extras_decode(&x, 1, report, sizeof report, &e) &&
	      e.pad);
	CHECK("X full right, Y centred", e.axis[0] == 32767 && e.axis[1] > -2 && e.axis[1] < 2);
	CHECK("the hat is right (3)", e.hat == 3);
	CHECK("buttons 1 and 3", e.pad_buttons == 5);
	CHECK("a centred report: axes near zero, no hat, no buttons",
	      btv_hid_extras_decode(&x, 1, centred, sizeof centred, &e) &&
	      e.axis[0] > -2 && e.axis[0] < 2 && e.hat == 0 && e.pad_buttons == 0);

	memset(&log, 0, sizeof log);
	memset(&sink, 0, sizeof sink);
	sink.context = &log;
	sink.key = on_key;
	btv_hid_pad_init(&p);
	(void)btv_hid_extras_decode(&x, 1, report, sizeof report, &e);
	btv_hid_pad_event(&p, &e, &sink);
	CHECK("stick right + hat right + buttons 1 and 3: cursor right, Return, Space",
	      log.n == 3 && log.log[0] == BTV_KEY_CursorRight && log.log[1] == BTV_KEY_Return &&
	      log.log[2] == BTV_KEY_Space);
	log.n = 0;
	btv_hid_pad_event(&p, &e, &sink);
	CHECK("the same report again: nothing new", log.n == 0);
	(void)btv_hid_extras_decode(&x, 1, centred, sizeof centred, &e);
	btv_hid_pad_event(&p, &e, &sink);
	CHECK("centred: all three released", log.n == 3 &&
	      log.log[0] == -BTV_KEY_CursorRight - 1);
	log.n = 0;
	{
		/* The stick a little to the left, within the dead zone, then past it. */
		btv_hid_extra_event s;

		memset(&s, 0, sizeof s);
		s.pad = 1;
		s.axis[0] = -9000;
		btv_hid_pad_event(&p, &s, &sink);
		CHECK("a small push is ignored", log.n == 0);
		s.axis[0] = -20000;
		btv_hid_pad_event(&p, &s, &sink);
		CHECK("a firm push is cursor left", log.n == 1 && log.log[0] == BTV_KEY_CursorLeft);
		s.axis[0] = -12000;
		btv_hid_pad_event(&p, &s, &sink);
		CHECK("easing off keeps it held (hysteresis)", log.n == 1);
		s.axis[0] = -5000;
		btv_hid_pad_event(&p, &s, &sink);
		CHECK("and letting go releases it", log.n == 2 && log.log[1] == -BTV_KEY_CursorLeft - 1);
		s.axis[1] = 30000;
		btv_hid_pad_event(&p, &s, &sink);
		CHECK("stick down is cursor down", log.log[log.n - 1] == BTV_KEY_CursorDown);
		btv_hid_pad_release_all(&p, &sink);
		CHECK("release_all lets everything go", log.log[log.n - 1] == -BTV_KEY_CursorDown - 1);
	}
}

static void sdp(void)
{
	btp_sdpc c;
	btv_hid_sdp_info info;
	uint8_t req[64], resp[300];
	static const uint16_t attrs[] = { 0x0202, 0x0205, 0x0206, 0x020e };
	size_t n, got, bytes = 0;
	const uint8_t *result;
	unsigned int rounds = 0;
	int r = 0;

	printf("SDP: the HID record, with continuation\n");
	{
		static uint8_t both[400];

		memcpy(both, consumer_array, sizeof consumer_array);
		memcpy(both + sizeof consumer_array, gamepad, sizeof gamepad);
		build_record_for(both, sizeof consumer_array + sizeof gamepad);
	}
	btp_sdpc_start(&c, 0x1124, attrs, 4, 0x0100);
	while (c.state == BTP_SDPC_SEND && rounds < 20) {
		n = btp_sdpc_request(&c, req, sizeof req, 1000 + rounds);
		CHECK("a request goes out", n > 0);
		if (rounds == 0)
			CHECK("the first is ServiceSearchAttribute for UUID 0x1124, with no continuation",
			      req[0] == 0x06 && req[5] == 0x35 && req[6] == 3 && req[8] == 0x11 &&
			      req[9] == 0x24 && req[n - 1] == 0);
		got = sdp_server_reply(req, 40, resp);
		r = btp_sdpc_response(&c, resp, got);
		rounds++;
	}
	CHECK("the record arrives in several pieces", rounds == 4 && r == 1 && c.state == BTP_SDPC_DONE);
	result = btp_sdpc_result(&c, &bytes);
	CHECK("whole and identical", result != 0 && bytes == record_bytes &&
	      memcmp(result, record, record_bytes) == 0);
	CHECK("the parser finds the descriptor", btv_hid_sdp_parse(result, bytes, &info) == 1 &&
	      info.descriptor_bytes == sizeof consumer_array + sizeof gamepad &&
	      memcmp(info.descriptor, consumer_array, sizeof consumer_array) == 0);
	CHECK("and: not a boot device, subclass 0x40, reconnects itself",
	      info.boot_known && !info.boot_device && info.subclass_known &&
	      info.subclass == 0x40 && info.reconnect_known && info.reconnect_initiate);
	{
		btv_hid_extras x;

		parse(info.descriptor, info.descriptor_bytes, &x);
		CHECK("and the descriptor read from it has both parts",
		      x.consumer_used == 1 && x.pad_used);
	}
	/* Damage. */
	CHECK("a truncated result is refused", btv_hid_sdp_parse(record, record_bytes - 3, &info) == -1);
	{
		uint8_t empty[] = { 0x35, 0x00 };

		CHECK("no records is not an error, and finds nothing",
		      btv_hid_sdp_parse(empty, 2, &info) == 0);
	}
	{
		btp_sdpc d;

		btp_sdpc_start(&d, 0x1124, attrs, 4, 7);
		(void)btp_sdpc_request(&d, req, sizeof req, 0);
		resp[0] = 0x01; resp[1] = 0; resp[2] = 7; resp[3] = 0; resp[4] = 2;
		resp[5] = 0; resp[6] = 3;
		CHECK("an error response fails the search", btp_sdpc_response(&d, resp, 7) == -1 &&
		      d.state == BTP_SDPC_FAILED);
		btp_sdpc_start(&d, 0x1124, attrs, 4, 7);
		(void)btp_sdpc_request(&d, req, sizeof req, 100);
		btp_sdpc_tick(&d, 100 + 1999, 2000);
		CHECK("a search waits for its answer", d.state == BTP_SDPC_WAIT);
		btp_sdpc_tick(&d, 100 + 2000, 2000);
		CHECK("and gives up", d.state == BTP_SDPC_FAILED);
		btp_sdpc_start(&d, 0x1124, attrs, 4, 7);
		(void)btp_sdpc_request(&d, req, sizeof req, 0);
		got = sdp_server_reply(req, 100, resp);
		resp[2] = 9;
		CHECK("a response to another transaction is not taken", btp_sdpc_response(&d, resp, got) == 0 &&
		      d.state == BTP_SDPC_WAIT);
	}
}

int main(void)
{
	consumer();
	pad();
	sdp();
	return check_summary("hidx");
}
