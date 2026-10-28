/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/* Shared by the HID checks (hidx.c, a2dp.c): report descriptors written
 * from the USB HID 1.11 item encoding and the Usage Tables, and a HID
 * service record built from the Bluetooth HID Profile 1.1.1 attribute list. */
#ifndef TESTS_HIDTEST_H
#define TESTS_HIDTEST_H

#include <stdint.h>
#include <string.h>

/* A keyboard's consumer controls, as an array of 16-bit usages (report 2),
 * and as bits for volume up, down, mute and play/pause (report 3). */
__attribute__((unused)) static const uint8_t consumer_array[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02,
	0x19, 0x00, 0x2a, 0x3c, 0x02, 0x15, 0x00, 0x26, 0x3c, 0x02,
	0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0
};
__attribute__((unused)) static const uint8_t consumer_bits[] = {
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04,
	0x09, 0xe9, 0x09, 0xea, 0x09, 0xe2, 0x09, 0xcd, 0x81, 0x02,
	0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0
};

/* A game pad: report 1, X and Y as 16 bits unsigned, a hat switch 1 to 8
 * (null state outside), ten buttons. */
__attribute__((unused)) static const uint8_t gamepad[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
	0x09, 0x01, 0xa1, 0x00,
	0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
	0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0xc0,
	0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01,
	0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x0a, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x0a, 0x81, 0x02,
	0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
	0xc0
};


/* A keyboard that also has consumer keys: report 1 (keyboard), then 2. */
__attribute__((unused)) static uint8_t kbd_desc[256];
__attribute__((unused)) static size_t kbd_desc_bytes;
__attribute__((unused)) static void kbd_desc_build(void)
{
	static const uint8_t keyboard[] = {
		0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
		0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
		0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
		0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
		0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02,
		0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
		0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
		0x95, 0x06, 0x75, 0x08, 0x81, 0x00, 0xc0
	};
	memcpy(kbd_desc, keyboard, sizeof keyboard);
	memcpy(kbd_desc + sizeof keyboard, consumer_array, sizeof consumer_array);
	kbd_desc_bytes = sizeof keyboard + sizeof consumer_array;
}

/* A HID record around a descriptor, as an attribute lists sequence. */
__attribute__((unused)) static uint8_t record[600];
__attribute__((unused)) static size_t record_bytes;
__attribute__((unused)) static int record_boot_device;

__attribute__((unused)) static void build_record_for(const uint8_t *desc, size_t d)
{
	uint8_t *p = record + 6;
	size_t n;
	uint8_t head[6];

	*p++ = 0x09; *p++ = 0x00; *p++ = 0x01;                      /* class list */
	*p++ = 0x35; *p++ = 0x03; *p++ = 0x19; *p++ = 0x11; *p++ = 0x24;
	*p++ = 0x09; *p++ = 0x02; *p++ = 0x02; *p++ = 0x08; *p++ = 0x40;   /* subclass */
	*p++ = 0x09; *p++ = 0x02; *p++ = 0x05; *p++ = 0x28; *p++ = 0x01;   /* reconnect */
	*p++ = 0x09; *p++ = 0x02; *p++ = 0x06;                      /* descriptor list */
	*p++ = 0x36; *p++ = (uint8_t)((d + 8) >> 8); *p++ = (uint8_t)(d + 8);
	*p++ = 0x36; *p++ = (uint8_t)((d + 5) >> 8); *p++ = (uint8_t)(d + 5);
	*p++ = 0x08; *p++ = 0x22;
	*p++ = 0x26; *p++ = (uint8_t)(d >> 8); *p++ = (uint8_t)d;
	memcpy(p, desc, d);
	p += d;
	*p++ = 0x09; *p++ = 0x02; *p++ = 0x0e; *p++ = 0x28;
	*p++ = record_boot_device ? 0x01 : 0x00;                    /* boot */
	n = (size_t)(p - (record + 6));
	/* The record's sequence, then the outer one around it. */
	head[0] = 0x36; head[1] = (uint8_t)(n >> 8); head[2] = (uint8_t)n;
	memcpy(record + 3, head, 3);
	record[0] = 0x36; record[1] = (uint8_t)((n + 3) >> 8); record[2] = (uint8_t)(n + 3);
	record_bytes = n + 6;
}

/* The server's reply to the client's request: the record in pieces of `chunk`
 * octets with continuation state (two octets, the offset). */
__attribute__((unused))
static size_t sdp_server_reply(const uint8_t *req, size_t chunk, uint8_t *out)
{
	size_t off = 0, plen, take, at;
	size_t attr_len = req[5 + 5 + 2 + 1];            /* AttributeIDList length */
	size_t cont = 5 + 5 + 2 + 2 + attr_len;

	if (req[cont] == 2) off = (size_t)(req[cont + 1] << 8 | req[cont + 2]);
	take = record_bytes - off < chunk ? record_bytes - off : chunk;
	plen = 2 + take + 1 + (off + take < record_bytes ? 2 : 0);
	out[0] = 0x07; out[1] = req[1]; out[2] = req[2];
	out[3] = (uint8_t)(plen >> 8); out[4] = (uint8_t)plen;
	out[5] = (uint8_t)(take >> 8); out[6] = (uint8_t)take;
	memcpy(out + 7, record + off, take);
	at = 7 + take;
	if (off + take < record_bytes) {
		out[at++] = 2; out[at++] = (uint8_t)((off + take) >> 8);
		out[at++] = (uint8_t)(off + take);
	} else
		out[at++] = 0;
	return at;
}

#endif
