/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for BluetoothVoice's shared core (modules/network/bluetooth/
 * c/bt_core.c): discovery, pairing and bonding, driven with recorded HCI
 * packets through a fake transport.
 *
 * The fake plays the controller. Every command the core sends is kept, and
 * answered with a successful Command Complete (or the Command Status the
 * real controller gives for Inquiry, Create Connection and the like);
 * events the test wants the core to see are queued and handed back by the
 * transport's poll in order. Expected bytes are written out from the
 * Bluetooth Core Specification (Vol 4 Part E, sections 7.1 to 7.7), not
 * taken from the code under test: each check names the opcode or event.
 *
 * The profiles and L2CAP are stubbed: this file is about the controller
 * conversation, not what rides on the links.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bt_core.h"
#include "check.h"

/* ---------------------------------------------------------------- the fake */

#define SENT_MAX 64
static struct { uint8_t bytes[260]; unsigned int length; } sent[SENT_MAX];
static unsigned int sent_count;

#define QUEUE_MAX 64
static struct { uint8_t bytes[260]; unsigned int length; } queue[QUEUE_MAX];
static unsigned int queue_head, queue_tail;

#define TICKETS 256
static uint8_t ticket_kind[TICKETS];
static uint32_t next_ticket;

static void send_event(const uint8_t *event, unsigned int length)
{
	memcpy(queue[queue_tail % QUEUE_MAX].bytes, event, length);
	queue[queue_tail % QUEUE_MAX].length = length;
	queue_tail++;
}

/* With this set the fake is a whole controller: the bring-up commands get
 * the replies of the right size (a BCM4345C0's, HCI 9), not a bare status. */
static int full_controller;

static unsigned int boot_reply(uint16_t opcode, uint8_t *e)
{
	unsigned int n = 6, i;

	memset(e, 0, 80);
	e[0] = 0x0e; e[2] = 1;
	e[3] = (uint8_t)opcode; e[4] = (uint8_t)(opcode >> 8);
	switch (opcode) {
	case 0x1009: n = 12; for (i = 0; i < 6; i++) e[6 + i] = (uint8_t)(0x91 - i); break;
	case 0x1005: n = 13; e[6] = 0xfd; e[7] = 0x03; e[8] = 60; e[9] = 8; e[11] = 0; break;
	case 0x1003: n = 14; e[6] = 0xff; e[7] = 0xfe; e[8] = 0x8f; e[9] = 0xfe;
	             e[10] = 0xd8; e[11] = 0x3f; e[12] = 0x5b; e[13] = 0x87; break;
	case 0x1001: n = 14; e[6] = 9; e[7] = 0x7e; e[8] = 0x01; e[9] = 9;
	             e[10] = 0x31; e[11] = 0x01; e[12] = 0x19; e[13] = 0x61; break;
	case 0x1002: n = 70; break;
	case 0x2002: n = 9; e[6] = 0xfb; e[7] = 0x00; e[8] = 8; break;  /* LE buffer */
	default: break;
	}
	e[1] = (uint8_t)(n - 2);
	return n;
}

/* The status the fake gives Inquiry and Create Connection (0: accepted). */
static uint8_t inquiry_status, create_status;

/* A command completes with status 0 and the credit back, except those the
 * real controller answers with Command Status. */
static void answer(const uint8_t *command)
{
	uint16_t opcode = (uint16_t)(command[0] | (command[1] << 8));
	uint8_t e[80];

	switch (opcode) {
	case 0x0401: case 0x0405: case 0x0406: case 0x0411: case 0x0413:
	case 0x0419: case 0x0409:
		/* Command Status: status, credits, opcode. */
		e[0] = 0x0f; e[1] = 4;
		e[2] = opcode == 0x0401 ? inquiry_status :
		       opcode == 0x0405 ? create_status : 0;
		e[3] = 1;
		e[4] = (uint8_t)opcode; e[5] = (uint8_t)(opcode >> 8);
		send_event(e, 6);
		break;
	default:
		if (full_controller) {
			send_event(e, boot_reply(opcode, e));
			break;
		}
		/* Command Complete: credits, opcode, status. */
		e[0] = 0x0e; e[1] = 4; e[2] = 1;
		e[3] = (uint8_t)opcode; e[4] = (uint8_t)(opcode >> 8);
		e[5] = 0;
		send_event(e, 6);
		break;
	}
}

/* The transport's start-up: it goes on for `transport_pumps` calls. */
static int transport_pumps, transport_calls;
int btv_host_bridge_boot(void)
{
	return ++transport_calls > transport_pumps ? 1 : 0;
}

int btv_host_bridge_open(uint32_t *windows_error)
{
	*windows_error = 0;
	return BTV_HOST_BRIDGE_OK;
}

int btv_host_bridge_close(uint32_t *windows_error)
{
	*windows_error = 0;
	return BTV_HOST_BRIDGE_OK;
}

/* Command transfers to lose: the next n are submitted, recorded and then
 * reported failed by the USB side, with no answer from the controller. */
static unsigned int fail_commands;
static uint8_t ticket_failed[TICKETS];

int btv_host_bridge_submit(unsigned int kind, const uint8_t *data,
                           uint32_t bytes, uint32_t packets,
                           uint32_t *ticket, uint32_t *windows_error)
{
	(void)packets;
	*windows_error = 0;
	*ticket = ++next_ticket;
	ticket_kind[*ticket % TICKETS] = (uint8_t)kind;
	if (kind == BTV_HOST_BRIDGE_COMMAND) {
		if (sent_count < SENT_MAX) {
			memcpy(sent[sent_count].bytes, data, bytes);
			sent[sent_count].length = bytes;
			sent_count++;
		}
		if (fail_commands != 0) {
			--fail_commands;
			ticket_failed[*ticket % TICKETS] = 1;
		} else {
			ticket_failed[*ticket % TICKETS] = 0;
			answer(data);
		}
	}
	return BTV_HOST_BRIDGE_OK;
}

int btv_host_bridge_poll(uint32_t ticket, btv_host_bridge_completion *c)
{
	unsigned int kind = ticket_kind[ticket % TICKETS];

	memset(c, 0, offsetof(btv_host_bridge_completion, data));
	c->kind = kind;
	if (kind == BTV_HOST_BRIDGE_COMMAND) {
		if (ticket_failed[ticket % TICKETS]) c->error = 1;
		return BTV_HOST_BRIDGE_OK;
	}
	if (kind == BTV_HOST_BRIDGE_EVENT_IN && queue_head != queue_tail) {
		unsigned int i = queue_head++ % QUEUE_MAX;

		memcpy(c->data, queue[i].bytes, queue[i].length);
		c->bytes = queue[i].length;
		return BTV_HOST_BRIDGE_OK;
	}
	return BTV_HOST_BRIDGE_PENDING;
}

int btv_host_bridge_cancel(uint32_t ticket, uint32_t *windows_error)
{
	(void)ticket;
	*windows_error = 0;
	return BTV_HOST_BRIDGE_OK;
}

/* What rides on the links is not under test. */
void btv_host_bridge_note(const char *a, const char *b) { (void)a; (void)b; }
void btc_l2cap_link_closed(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btc_l2cap_receive(btc_core *c, uint8_t l, const uint8_t *p, size_t n)
{ (void)c; (void)l; (void)p; (void)n; }
void btc_l2cap_tick(btc_core *c) { (void)c; }
void btp_init(btc_core *c) { (void)c; }
void btp_pump(btc_core *c) { (void)c; }
int btp_le_busy(btc_core *c) { (void)c; return 0; }
void btp_le_advert(btc_core *c, const uint8_t *a, uint8_t t)
{ (void)c; (void)a; (void)t; }
void btp_le_data(btc_core *c, uint8_t l, uint16_t cid, const uint8_t *p,
                 size_t n)
{ (void)c; (void)l; (void)cid; (void)p; (void)n; }
void btp_le_encryption(btc_core *c, uint8_t l, uint8_t s)
{ (void)c; (void)l; (void)s; }
void btp_le_rand(btc_core *c, uint8_t l, uint8_t s, const uint8_t *r,
                 size_t n)
{ (void)c; (void)l; (void)s; (void)r; (void)n; }
void btp_link_closed(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_link_opened(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_link_secured(btc_core *c, uint8_t l) { (void)c; (void)l; }

/* --------------------------------------------------------------- the rig */

static btc_core core;
static uint32_t clock_ms = 1000;
static btv_host_bridge_completion completion;
static char profiles_storage[16];

extern uint32_t btc_passkey, btc_passkey_generation;

static void pump(unsigned int times)
{
	while (times--) {
		clock_ms += 10;
		btc_pump(&core, clock_ms);
	}
}

static void rig(void)
{
	memset(sent, 0, sizeof(sent));
	sent_count = 0;
	queue_head = queue_tail = 0;
	btc_passkey = 0;
	btc_start(&core, (struct btc_profiles *)profiles_storage, &completion,
	          clock_ms);
	/* Past bring-up: the controller as the bootstrap would leave it. */
	core.state = BTC_READY;
	core.credits = 1;
	core.acl_mtu = 1021;
	core.acl_max = core.acl_free = 8;
	core.le_supported = 1;
	core.ssp_supported = 1;
}

/* The nth command sent, or -1: its opcode, to compare. */
static int opcode_of(unsigned int n)
{
	if (n >= sent_count) return -1;
	return sent[n].bytes[0] | (sent[n].bytes[1] << 8);
}

static int last_opcode(void)
{
	return sent_count == 0 ? -1 : opcode_of(sent_count - 1);
}

static const uint8_t *last_params(void)
{
	return sent[sent_count - 1].bytes + 3;
}

static int last_length(void)
{
	return sent[sent_count - 1].bytes[2];
}

static const uint8_t PEER[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static int has_address(const uint8_t *p)
{
	return memcmp(p, PEER, 6) == 0;
}

/* ------------------------------------------------------------ discovery */

static void discovery(void)
{
	/* Inquiry Result with RSSI (0x22), one response: a keyboard.
	 * BD_ADDR, page scan repetition mode R1, reserved, class of
	 * device 0x002540 (peripheral, keyboard), clock offset, RSSI -50. */
	static const uint8_t rssi_result[] = {
		0x22, 15, 1,
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0x00,
		0x40, 0x25, 0x00, 0x34, 0x12, 0xce };
	/* Remote Name Request Complete (0x07): status 0, address, name. */
	static const uint8_t name[] = {
		0x07, 6 + 1 + 9, 0x00,
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
		'B', 'T', ' ', 'K', 'e', 'y', 'b', 'd', 0 };
	static const uint8_t complete[] = { 0x01, 1, 0x00 };
	btc_device *d;

	printf("discovery\n");
	rig();
	CHECK("a scan is accepted", btc_scan(&core, 10));
	pump(4);
	/* Inquiry: OGF 1 OCF 1; GIAC LAP 0x9E8B33, length in 1.28 s units
	 * (10 s is 8), unlimited responses. */
	{
		unsigned int i, found = 0;
		for (i = 0; i < sent_count; i++)
			if (opcode_of(i) == 0x0401) {
				static const uint8_t want[] =
					{ 0x33, 0x8b, 0x9e, 0x08, 0x00 };
				found = 1;
				CHECK("Inquiry has the general access code, 8 units, no limit",
				      sent[i].bytes[2] == 5 &&
				      memcmp(sent[i].bytes + 3, want, 5) == 0);
			}
		CHECK("Inquiry (0x0401) was sent", found);
	}
	CHECK("a second scan is refused while the first runs",
	      !btc_scan(&core, 10));
	CHECK("the controller is reported as scanning", btc_scanning(&core));

	send_event(rssi_result, sizeof(rssi_result));
	pump(2);
	d = btc_device_find(&core, PEER);
	CHECK("an Inquiry Result with RSSI adds a device", d != 0);
	if (d == 0) return;
	CHECK("its class of device is kept",
	      d->device_class[0] == 0x40 && d->device_class[1] == 0x25 &&
	      d->device_class[2] == 0x00);
	CHECK("RSSI -50 dBm", d->rssi == -50);
	CHECK("a peripheral keyboard class is a keyboard",
	      (d->flags & BTC_DEV_KEYBOARD) != 0 && (d->flags & BTC_DEV_HID) != 0);
	CHECK("it is not a mouse", (d->flags & BTC_DEV_MOUSE) == 0);
	CHECK("its clock offset is kept", d->clock_offset == 0x1234);

	/* A repeat of the same address does not make a second entry. */
	send_event(rssi_result, sizeof(rssi_result));
	pump(2);
	CHECK("a repeat response updates the same entry", core.device_count == 1);

	/* Names are asked for once the Low Energy scan has run its course. */
	clock_ms += 11000;
	send_event(complete, sizeof(complete));
	pump(6);
	CHECK("Inquiry Complete ends the inquiry", !core.inquiring);
	{
		unsigned int i, found = 0;
		for (i = 0; i < sent_count; i++)
			if (opcode_of(i) == 0x0419) {
				found = 1;
				/* Remote Name Request: address, page scan
				 * repetition mode, reserved, clock offset with
				 * bit 15 (valid) set. */
				CHECK("Remote Name Request (0x0419) names the device",
				      sent[i].bytes[2] == 10 &&
				      has_address(sent[i].bytes + 3) &&
				      sent[i].bytes[9] == 0x01 &&
				      sent[i].bytes[11] == 0x34 &&
				      sent[i].bytes[12] == 0x92);
			}
		CHECK("a nameless device is asked for its name", found);
	}
	send_event(name, sizeof(name));
	pump(4);
	CHECK_TEXT("Remote Name Request Complete gives the name", d->name,
	           "BT Keybd");
	CHECK("the device is marked named", (d->flags & BTC_DEV_NAMED) != 0);
	CHECK("the scan finishes once names are in", !core.scan_requested);

	/* A damaged inquiry result: the count says two, the data holds one. */
	{
		static const uint8_t short_result[] = {
			0x22, 15, 2,
			0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0x00,
			0x04, 0x02, 0x00, 0x00, 0x00, 0xd0 };
		unsigned int before = core.device_count;
		send_event(short_result, sizeof(short_result));
		pump(2);
		CHECK("a truncated Inquiry Result is ignored",
		      core.device_count == before);
	}

	/* Extended Inquiry Result (0x2F): complete name in the data. */
	{
		uint8_t eir[3 + 14 + 240];
		static const uint8_t mouse[6] = { 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6 };
		btc_device *m;

		memset(eir, 0, sizeof(eir));
		eir[0] = 0x2f; eir[1] = 255; eir[2] = 1;
		memcpy(eir + 3, mouse, 6);
		eir[9] = 0x01;
		/* Class 0x002580: peripheral, pointing device. */
		eir[11] = 0x80; eir[12] = 0x25; eir[13] = 0x00;
		eir[16] = 0xc4;                     /* -60 dBm */
		/* EIR: length 6, type 0x09 (complete local name), "Mouse". */
		eir[17] = 6; eir[18] = 0x09; memcpy(eir + 19, "Mouse", 5);
		send_event(eir, sizeof(eir));
		pump(2);
		m = btc_device_find(&core, mouse);
		CHECK("an Extended Inquiry Result adds a device", m != 0);
		if (m != 0) {
			CHECK("a pointing device class is a mouse",
			      (m->flags & BTC_DEV_MOUSE) != 0);
			CHECK("RSSI -60 dBm", m->rssi == -60);
			CHECK_TEXT("the complete name comes from the EIR data",
			           m->name, "Mouse");
		}
	}
}


/* ------------------------------------------- scans, names, connections */

static unsigned int count_opcode(unsigned int from, int opcode);
static unsigned int mark(void);
static const uint8_t *last_params(void);

static void rssi_inquiry(const uint8_t address[6], uint8_t class0,
                         uint8_t class1, uint8_t class2, int8_t rssi)
{
	uint8_t e[17];

	e[0] = 0x22; e[1] = 15; e[2] = 1;
	memcpy(e + 3, address, 6);
	e[9] = 0x01; e[10] = 0x00;
	e[11] = class0; e[12] = class1; e[13] = class2;
	e[14] = 0x34; e[15] = 0x12; e[16] = (uint8_t)rssi;
	send_event(e, 17);
}

static void name_event(const uint8_t address[6], uint8_t status,
                       const char *name)
{
	uint8_t e[3 + 6 + 248];
	size_t n = name != 0 ? strlen(name) + 1 : 0;

	e[0] = 0x07; e[1] = (uint8_t)(1 + 6 + n); e[2] = status;
	memcpy(e + 3, address, 6);
	if (n != 0) memcpy(e + 9, name, n);
	send_event(e, 9 + (unsigned int)n);
}

/* The address a Remote Name Request (0x0419) after `from` is for, or -1. */
static int name_request_after(unsigned int from, unsigned int skip, uint8_t address[6])
{
	unsigned int i;

	for (i = from; i < sent_count; i++)
		if (opcode_of(i) == 0x0419 && skip-- == 0) {
			memcpy(address, sent[i].bytes + 3, 6);
			return 1;
		}
	return -1;
}

static void le_report(uint8_t type, const uint8_t address[6], int8_t rssi,
                      const uint8_t *data, uint8_t n)
{
	uint8_t e[64];

	e[0] = 0x3e; e[1] = (uint8_t)(11 + n); e[2] = 0x02; e[3] = 1;
	e[4] = type; e[5] = 0;
	memcpy(e + 6, address, 6);
	e[12] = n;
	memcpy(e + 13, data, n);
	e[13 + n] = (uint8_t)rssi;
	send_event(e, 14u + n);
}

static void names(void)
{
	static const uint8_t KEYBD[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	static const uint8_t PHONE[6] = { 0x21, 0x22, 0x33, 0x44, 0x55, 0x77 };
	static const uint8_t JUNK[6] = { 0x31, 0x22, 0x33, 0x44, 0x55, 0x88 };
	static const uint8_t TV[6] = { 0x41, 0x22, 0x33, 0x44, 0x55, 0x99 };
	uint8_t eir[3 + 14 + 240], who[6];
	btc_device *d;
	unsigned int m, asked;

	printf("names: shortened EIR names, advertising data and scan responses\n");
	rig();
	memset(eir, 0, sizeof eir);
	eir[0] = 0x2f; eir[1] = 255; eir[2] = 1;
	memcpy(eir + 3, TV, 6);
	eir[9] = 0x01; eir[11] = 0x0c; eir[12] = 0x04; eir[13] = 0x24;   /* audio/video */
	eir[16] = 0xc0;
	/* Flags, then a Shortened Local Name (0x08), then a 16-bit UUID list. */
	eir[17] = 2; eir[18] = 0x01; eir[19] = 0x06;
	eir[20] = 6; eir[21] = 0x08; memcpy(eir + 22, "Sony ", 5);
	send_event(eir, sizeof eir);
	CHECK("a scan is started", btc_scan(&core, 10));
	pump(6);
	d = btc_device_find(&core, TV);
	CHECK("an EIR with only a shortened name still gives the device a name",
	      d != 0 && strcmp(d->name, "Sony ") == 0 && (d->flags & BTC_DEV_NAMED));
	/* A later complete name (type 0x09) replaces the shortened one. */
	eir[20] = 11; eir[21] = 0x09; memcpy(eir + 22, "Sony TV 55", 10);
	send_event(eir, sizeof eir);
	pump(4);
	CHECK("a complete name replaces the shortened one",
	      d != 0 && strcmp(d->name, "Sony TV 55") == 0);
	CHECK("EIR names are counted", core.stats.eir_names == 2);

	/* Low Energy: the advertisement has the flags, the scan response the
	 * name (Core Vol 3 Part C 11: a name may be in either). */
	{
		static const uint8_t LE_ADDR[6] = { 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xc6 };
		static const uint8_t advert[] = { 2, 0x01, 0x06, 3, 0x19, 0xc2, 0x03 };
		static const uint8_t response[] = { 9, 0x09, 'M', 'X', ' ', 'A', 'n', 'y', 'w', 0 };

		le_report(0x00, LE_ADDR, -60, advert, sizeof advert);
		pump(2);
		d = btc_device_find(&core, LE_ADDR);
		CHECK("an LE advertisement adds a device, a mouse by its appearance",
		      d != 0 && (d->flags & BTC_DEV_LE) && (d->flags & BTC_DEV_MOUSE) &&
		      d->name[0] == 0);
		/* The scan response is type 4. */
		le_report(0x04, LE_ADDR, -60, response, sizeof response);
		pump(2);
		d = btc_device_find(&core, LE_ADDR);
		CHECK("its scan response names it", d != 0 && strcmp(d->name, "MX Anyw") == 0);
		CHECK("LE names are counted", core.stats.le_names == 1);
	}

	/* A busy controller packs several reports into one event, laid out
	 * field by field (Core Vol 4 Part E 7.7.65.2). */
	{
		static const uint8_t A1[6] = { 0xa1, 0, 0, 0, 0, 0xc1 };
		static const uint8_t A2[6] = { 0xa2, 0, 0, 0, 0, 0xc1 };
		static const uint8_t A3[6] = { 0xa3, 0, 0, 0, 0, 0xc1 };
		uint8_t e[80];
		unsigned int at = 0;
		static const uint8_t d1[] = { 2, 0x01, 0x06 };
		static const uint8_t d2[] = { 9, 0x09, 'M', 'X', ' ', 'K', 'e', 'y', 's', 0 };
		static const uint8_t d3[] = { 2, 0x01, 0x04 };

		e[at++] = 0x3e; e[at++] = 0; e[at++] = 0x02; e[at++] = 3;
		e[at++] = 0x00; e[at++] = 0x04; e[at++] = 0x00;            /* event types */
		e[at++] = 0; e[at++] = 0; e[at++] = 1;                     /* address types */
		memcpy(e + at, A1, 6); at += 6;
		memcpy(e + at, A2, 6); at += 6;
		memcpy(e + at, A3, 6); at += 6;
		e[at++] = sizeof d1; e[at++] = sizeof d2; e[at++] = sizeof d3;   /* lengths */
		memcpy(e + at, d1, sizeof d1); at += sizeof d1;
		memcpy(e + at, d2, sizeof d2); at += sizeof d2;
		memcpy(e + at, d3, sizeof d3); at += sizeof d3;
		e[at++] = (uint8_t)-55; e[at++] = (uint8_t)-56; e[at++] = (uint8_t)-57;   /* RSSI */
		e[1] = (uint8_t)(at - 2);
		send_event(e, at);
		pump(2);
		CHECK("three reports in one event: all three devices are listed",
		      btc_device_find(&core, A1) != 0 && btc_device_find(&core, A2) != 0 &&
		      btc_device_find(&core, A3) != 0);
		d = btc_device_find(&core, A2);
		CHECK("the second one's scan response named it, with its own RSSI",
		      d != 0 && strcmp(d->name, "MX Keys") == 0 && d->rssi == -56);
		d = btc_device_find(&core, A3);
		CHECK("the third has the third's data and RSSI, a random address",
		      d != 0 && d->rssi == -57 && (d->flags & BTC_DEV_RANDOM_ADDR));
		/* A length that runs past the event is refused whole. */
		{
			unsigned int before = core.device_count;

			e[10] = 0xb0; e[16] = 0xb1; e[22] = 0xb2;      /* new addresses */
			e[29] = 31;                                    /* the second's data length */
			send_event(e, at);
			pump(2);
			CHECK("a report whose lengths run past the event is ignored",
			      core.device_count == before);
		}
	}

	printf("names: which device is asked first, and again\n");
	rig();
	CHECK("a scan is started", btc_scan(&core, 10));
	pump(2);
	rssi_inquiry(JUNK, 0x00, 0x00, 0x00, -40);       /* unclassified, strong */
	rssi_inquiry(PHONE, 0x0c, 0x02, 0x5a, -70);      /* phone */
	rssi_inquiry(KEYBD, 0x40, 0x25, 0x00, -75);      /* keyboard, weak */
	pump(4);
	m = mark();
	clock_ms += 11000;
	{
		static const uint8_t complete[] = { 0x01, 1, 0x00 };
		send_event(complete, sizeof complete);
	}
	pump(6);
	CHECK("the keyboard is asked for its name before the nearer unclassified device",
	      name_request_after(m, 0, who) == 1 && memcmp(who, KEYBD, 6) == 0);
	name_event(KEYBD, 0x04, 0);                      /* page timeout */
	pump(4);
	CHECK("a failed request is counted, with its status",
	      core.stats.name_failures == 1 && core.stats.name_last_status == 4);
	CHECK("the status is kept on the device",
	      btc_device_find(&core, KEYBD)->name_status == 4);
	asked = 1;
	{
		/* Answer each in turn until the scan is over. */
		unsigned int guard;
		for (guard = 0; guard < 8 && core.scan_requested; guard++) {
			if (name_request_after(m, asked, who) == 1) {
				if (memcmp(who, PHONE, 6) == 0) name_event(who, 0, "Pixel 8");
				else name_event(who, 0x04, 0);
				asked++;
			}
			pump(4);
		}
	}
	CHECK("the phone was named", strcmp(btc_device_find(&core, PHONE)->name, "Pixel 8") == 0);
	{
		unsigned int keyboard_requests = 0, junk_requests = 0, i;
		for (i = m; i < sent_count; i++)
			if (opcode_of(i) == 0x0419) {
				if (memcmp(sent[i].bytes + 3, KEYBD, 6) == 0) keyboard_requests++;
				if (memcmp(sent[i].bytes + 3, JUNK, 6) == 0) junk_requests++;
			}
		CHECK("the keyboard, which a person is looking for, was asked twice", keyboard_requests == 2);
		CHECK("the unclassified device only once", junk_requests == 1);
	}
	CHECK("the scan ends", !core.scan_requested);

	printf("names: a second scan keeps what was learnt\n");
	CHECK("the phone's name is remembered", core.names[0].used);
	name_event(KEYBD, 0, "BT Keybd");               /* a late answer */
	pump(2);
	CHECK("a name that arrives late is still taken",
	      strcmp(btc_device_find(&core, KEYBD)->name, "BT Keybd") == 0);
	core.inquiring = 0;
	CHECK("a second scan starts", btc_scan(&core, 10));
	pump(2);
	CHECK("the scan list was emptied of what is neither paired nor connected",
	      core.device_count == 0);
	rssi_inquiry(PHONE, 0x0c, 0x02, 0x5a, -70);
	pump(3);
	d = btc_device_find(&core, PHONE);
	CHECK("the phone returns with its name, before any request",
	      d != 0 && strcmp(d->name, "Pixel 8") == 0 && (d->flags & BTC_DEV_NAMED) &&
	      d->name_cached);
	{
		/* An inquiry result that carries a different name wins. */
		memset(eir, 0, sizeof eir);
		eir[0] = 0x2f; eir[1] = 255; eir[2] = 1;
		memcpy(eir + 3, PHONE, 6); eir[9] = 1;
		eir[11] = 0x0c; eir[12] = 0x02; eir[13] = 0x5a; eir[16] = 0xba;
		eir[17] = 7; eir[18] = 0x09; memcpy(eir + 19, "Pixel9", 6);
		send_event(eir, sizeof eir);
		pump(2);
		CHECK("a fresh name from the device replaces the remembered one",
		      strcmp(btc_device_find(&core, PHONE)->name, "Pixel9") == 0 &&
		      !btc_device_find(&core, PHONE)->name_cached);
	}
}

static void crowded(void)
{
	unsigned int i;
	uint8_t a[6];
	btc_device *d;
	static const uint8_t KEYBD[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

	printf("a crowded place: beacons do not push out the devices\n");
	rig();
	CHECK("a scan is started", btc_scan(&core, 10));
	pump(2);
	/* Fifty beacons, each unnamed, weak: the table has room for forty. */
	for (i = 0; i < 50; i++) {
		static const uint8_t flags[] = { 2, 0x01, 0x06 };
		a[0] = (uint8_t)i; a[1] = 0xb0; a[2] = a[3] = a[4] = 0; a[5] = 0xc1;
		le_report(0x03, a, (int8_t)(-90 + (int)(i % 10)), flags, sizeof flags);
		pump(1);
	}
	CHECK("the table is full", core.device_count == BTC_DEVICES);
	rssi_inquiry(KEYBD, 0x40, 0x25, 0x00, -70);
	pump(3);
	d = btc_device_find(&core, KEYBD);
	CHECK("a classic keyboard still gets in, ahead of a beacon", d != 0 &&
	      core.stats.devices_evicted > 0);
	CHECK("the table is no bigger", core.device_count == BTC_DEVICES);
	{
		unsigned int before = core.stats.devices_dropped;
		static const uint8_t flags[] = { 2, 0x01, 0x06 };
		a[0] = 0xee; a[1] = 0xb0; a[2] = a[3] = a[4] = 0; a[5] = 0xc1;
		/* A beacon weaker than all of those held: dropped, not a victim. */
		le_report(0x03, a, -120, flags, sizeof flags);
		pump(2);
		CHECK("a still weaker beacon is dropped, and counted",
		      btc_device_find(&core, a) == 0 && core.stats.devices_dropped > before);
	}
	{
		/* A paired device is never the victim. */
		unsigned int k, found = 0;
		btc_key_store(&core, KEYBD, 0)->flags = BTC_DEV_HID | BTC_DEV_KEYBOARD;
		for (k = 0; k < 60; k++) {
			static const uint8_t flags[] = { 2, 0x01, 0x06 };
			a[0] = (uint8_t)(100 + k); a[1] = 0xb1; a[2] = a[3] = a[4] = 0; a[5] = 0xc1;
			le_report(0x03, a, -30, flags, sizeof flags);
			pump(1);
		}
		found = btc_device_find(&core, KEYBD) != 0;
		CHECK("a paired keyboard stays in the list however many strong beacons come", found);
	}
}

static void connect_retry(void)
{
	uint8_t e[16];
	unsigned int m, creates;
	btc_link *link;

	printf("a person's Connect: tried again when nobody answers the page\n");
	rig();
	btc_key_store(&core, PEER, 0)->flags = BTC_DEV_HID | BTC_DEV_KEYBOARD;
	m = mark();
	CHECK("Connect is accepted", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	pump(3);
	/* Connection Complete, status 4: Page Timeout. */
	memset(e, 0, sizeof e);
	e[0] = 0x03; e[1] = 11; e[2] = 0x04; memcpy(e + 5, PEER, 6); e[11] = 1;
	send_event(e, 13);
	pump(3);
	CHECK("no link is left, and nothing is yet reported as a failure",
	      btc_link_by_address(&core, PEER) == 0 && (core.last_problem >> 8) == 0x0d);
	clock_ms += 1600;
	pump(4);
	creates = count_opcode(m, 0x0405);
	CHECK("a second Create Connection goes out after a short wait", creates == 2);
	CHECK("by a page again", btc_link_by_address(&core, PEER) != 0);
	send_event(e, 13);
	pump(3);
	clock_ms += 3200;
	pump(4);
	CHECK("and a third", count_opcode(m, 0x0405) == 3);
	send_event(e, 13);
	pump(3);
	clock_ms += 6000;
	pump(4);
	CHECK("then it stops: three pages in all", count_opcode(m, 0x0405) == 3);
	CHECK("the failure is reported with the status: area 3, 0x04",
	      core.last_problem == 0x0304u);
	CHECK("counted: three failures, two retries", core.stats.connect_failures == 3 &&
	      core.stats.connect_retries == 2 && core.stats.connect_last_status == 4);

	printf("a person's Connect: the second try connects\n");
	rig();
	m = mark();
	CHECK("Connect", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	pump(3);
	send_event(e, 13);                               /* page timeout */
	pump(3);
	clock_ms += 1600;
	pump(4);
	e[2] = 0; e[3] = 0x41; e[4] = 0;
	send_event(e, 13);                               /* now it answers */
	pump(3);
	link = btc_link_by_address(&core, PEER);
	CHECK("the link opens", link != 0 && link->state == BTC_LINK_OPEN);
	CHECK("and the 'trying again' message is gone", core.last_problem == 0);

	printf("the reconnection engine keeps its own schedule\n");
	rig();
	m = mark();
	core.engine_call = 1;
	CHECK("an engine connect", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	core.engine_call = 0;
	pump(3);
	e[2] = 0x04; e[3] = 0; e[4] = 0;
	send_event(e, 13);
	pump(3);
	clock_ms += 3000;
	pump(4);
	CHECK("it is not retried by the core", count_opcode(m, 0x0405) == 1 &&
	      core.last_problem == 0x0304u);
}

static void incoming(void)
{
	uint8_t e[16];
	unsigned int m;
	btc_link *link;
	static const uint8_t STRANGER[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };

	printf("incoming calls: a bonded device is accepted\n");
	rig();
	btc_key_store(&core, PEER, 0)->flags = BTC_DEV_HID | BTC_DEV_MOUSE;
	m = mark();
	/* Connection Request (0x04): address, class (peripheral, mouse), ACL. */
	e[0] = 0x04; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x80; e[9] = 0x05; e[10] = 0x00; e[11] = 0x01;
	send_event(e, 12);
	pump(4);
	CHECK("Accept Connection Request (0x0409) goes out", last_opcode() == 0x0409 &&
	      has_address(last_params()));
	CHECK("the mouse stays the master: we remain the slave (role 1)",
	      last_params()[6] == 0x01);
	CHECK("counted", core.stats.incoming == 1 && core.stats.incoming_accepted == 1);

	printf("incoming calls: a stranger is refused unless discoverable and an input device\n");
	rig();
	m = mark();
	memcpy(e + 2, STRANGER, 6);
	e[8] = 0x40; e[9] = 0x05;
	send_event(e, 12);
	pump(4);
	CHECK("a stranger gets Reject Connection Request (0x040A)",
	      last_opcode() == 0x040a && last_params()[6] == 0x0f);
	CHECK("the reason is recorded: not a bonded device",
	      core.stats.last_incoming_reason == 1 && core.stats.incoming_rejected == 1);
	btc_discoverable(&core, 1);
	pump(3);
	m = mark();
	send_event(e, 12);
	pump(4);
	CHECK("while discoverable a keyboard that calls is accepted",
	      count_opcode(m, 0x0409) == 1);
	link = btc_link_by_address(&core, STRANGER);
	CHECK("as a HID link", link != 0 && link->profile == BTC_PROFILE_HID &&
	      !link->outgoing);
	e[8] = 0x0c; e[9] = 0x02; e[2] = 0x81;           /* a phone */
	m = mark();
	send_event(e, 12);
	pump(4);
	CHECK("but not a phone", count_opcode(m, 0x040a) == 1);

	printf("incoming calls: the device calls while we are paging it\n");
	rig();
	btc_key_store(&core, PEER, 0)->flags = BTC_DEV_HID | BTC_DEV_MOUSE;
	CHECK("a page is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	pump(3);
	m = mark();
	memcpy(e + 2, PEER, 6);
	e[8] = 0x80; e[9] = 0x05;
	send_event(e, 12);
	pump(4);
	CHECK("its call is accepted, not refused", count_opcode(m, 0x0409) == 1 &&
	      count_opcode(m, 0x040a) == 0);
	link = btc_link_by_address(&core, PEER);
	CHECK("the link is now an incoming one", link != 0 && link->crossed && !link->outgoing);
	CHECK("counted as crossed", core.stats.incoming_crossed == 1);
	/* Our own page fails (it was answered by their call): the link stays. */
	memset(e, 0, sizeof e);
	e[0] = 0x03; e[1] = 11; e[2] = 0x04; memcpy(e + 5, PEER, 6); e[11] = 1;
	send_event(e, 13);
	pump(3);
	link = btc_link_by_address(&core, PEER);
	CHECK("the failure of our page does not end it", link != 0 &&
	      link->state == BTC_LINK_CONNECTING);
	e[2] = 0; e[3] = 0x42; e[4] = 0;
	send_event(e, 13);
	pump(3);
	link = btc_link_by_address(&core, PEER);
	CHECK("the connection completes", link != 0 && link->state == BTC_LINK_OPEN &&
	      link->handle == 0x42);
}

static void inquiry_retry(void)
{
	unsigned int m;

	printf("a scan that the controller refuses is begun again\n");
	rig();
	inquiry_status = 0x0c;                           /* command disallowed */
	m = mark();
	CHECK("Scan is accepted", btc_scan(&core, 10));
	pump(4);
	CHECK("the first Inquiry was refused", count_opcode(m, 0x0401) == 1 &&
	      core.stats.scan_failures == 1 && core.stats.scan_last_status == 0x0c);
	CHECK("no failure shown to the person yet", core.last_problem == 0);
	inquiry_status = 0;
	clock_ms += 1300;
	pump(4);
	CHECK("a second Inquiry goes out soon", count_opcode(m, 0x0401) == 2);
	CHECK("and the controller is scanning", core.inquiring);
	inquiry_status = 0x0c;
	core.inquiring = 0;
	core.scan_requested = 0;
	CHECK("a refusal three times in a row is shown", btc_scan(&core, 10));
	pump(4);
	clock_ms += 1300; pump(4);
	clock_ms += 2500; pump(4);
	clock_ms += 4000; pump(4);
	CHECK("after two more tries the failure is reported (area 9)",
	      (core.last_problem >> 8) == 0x09 && (core.last_problem & 0xff) == 0x0c);
	inquiry_status = 0;
}

/* -------------------------------------------------------------- pairing */

static unsigned int mark(void)
{
	return sent_count;
}

static void io_request(void)
{
	uint8_t e[8] = { 0x31, 6 };
	memcpy(e + 2, PEER, 6);
	send_event(e, 8);
}

static void pairing(void)
{
	uint8_t e[16];
	unsigned int m;

	printf("pairing\n");
	rig();

	/* IO Capability Request (0x31) is answered with IO Capability
	 * Request Reply (0x042B): capability 1 (DisplayYesNo), no OOB data,
	 * authentication requirements 4 (general bonding, no MITM). */
	m = mark();
	io_request();
	pump(4);
	CHECK("IO Capability Request Reply (0x042B) is sent",
	      sent_count > m && last_opcode() == 0x042b);
	CHECK("it names the peer, DisplayYesNo, no OOB, general bonding",
	      last_length() == 9 && has_address(last_params()) &&
	      last_params()[6] == 0x01 && last_params()[7] == 0x00 &&
	      last_params()[8] == 0x04);

	/* Just Works: the peer said NoInputNoOutput (IO Capability Response
	 * 0x32), so the confirmation is accepted without bothering anyone. */
	m = mark();
	memset(e, 0, sizeof(e));
	e[0] = 0x32; e[1] = 9; memcpy(e + 2, PEER, 6);
	e[8] = 0x03; e[9] = 0x00; e[10] = 0x00;
	send_event(e, 11);
	memset(e, 0, sizeof(e));
	e[0] = 0x33; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x40; e[9] = 0xe2; e[10] = 0x01; e[11] = 0x00; /* 123456 */
	send_event(e, 12);
	pump(6);
	CHECK("Just Works is accepted: User Confirmation Request Reply (0x042C)",
	      sent_count > m && last_opcode() == 0x042c && has_address(last_params()));
	CHECK("no question reaches the user", core.prompt_kind == BTC_PROMPT_NONE);

	/* Numeric comparison: both sides can show six digits. The numeric
	 * value is shown modulo 1,000,000. */
	m = mark();
	e[0] = 0x32; e[1] = 9; memcpy(e + 2, PEER, 6);
	e[8] = 0x01; e[9] = 0x00; e[10] = 0x05;
	send_event(e, 11);
	e[0] = 0x33; e[1] = 10; memcpy(e + 2, PEER, 6);
	/* 1234567 = 0x0012D687, so the six digits are 234567. */
	e[8] = 0x87; e[9] = 0xd6; e[10] = 0x12; e[11] = 0x00;
	send_event(e, 12);
	pump(6);
	CHECK("numeric comparison waits for the user", sent_count == m);
	CHECK("a confirmation question is posted",
	      core.prompt_kind == BTC_PROMPT_CONFIRM);
	CHECK("the digits are the value modulo a million",
	      core.prompt_value == 234567u);
	CHECK("the question is about the peer",
	      has_address(core.prompt_address));
	{
		uint32_t generation = core.prompt_generation;
		CHECK("the user says yes", btc_prompt_answer(&core, 1, 0));
		CHECK("answering clears the question and bumps its generation",
		      core.prompt_kind == BTC_PROMPT_NONE &&
		      core.prompt_generation != generation);
		pump(4);
		CHECK("yes is User Confirmation Request Reply (0x042C)",
		      last_opcode() == 0x042c && last_length() == 6 &&
		      has_address(last_params()));
	}

	/* No: User Confirmation Request Negative Reply (0x042D). */
	m = mark();
	e[0] = 0x33; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x01; e[9] = 0x00; e[10] = 0x00; e[11] = 0x00;
	send_event(e, 12);
	pump(4);
	CHECK("the user says no", btc_prompt_answer(&core, 0, 0));
	pump(4);
	CHECK("no is User Confirmation Request Negative Reply (0x042D)",
	      sent_count > m && last_opcode() == 0x042d &&
	      has_address(last_params()));

	/* A question nobody answers is refused after the timeout. */
	m = mark();
	send_event(e, 12);
	pump(4);
	CHECK("an unanswered question is pending",
	      core.prompt_kind == BTC_PROMPT_CONFIRM);
	clock_ms += BTC_PROMPT_TIMEOUT_MS;
	pump(6);
	CHECK("it is refused on timeout (0x042D)",
	      core.prompt_kind == BTC_PROMPT_NONE && last_opcode() == 0x042d);

	/* Passkey entry, where the peer shows the digits: User Passkey
	 * Request (0x34), answered by User Passkey Request Reply (0x042E)
	 * with the number, least significant byte first. */
	e[0] = 0x34; e[1] = 6; memcpy(e + 2, PEER, 6);
	send_event(e, 8);
	pump(4);
	CHECK("a passkey request posts an entry question",
	      core.prompt_kind == BTC_PROMPT_ENTRY);
	CHECK("a passkey over six digits is refused (0x042F)",
	      btc_prompt_answer(&core, 1, 1000000u));
	pump(4);
	CHECK("the refusal is User Passkey Request Negative Reply",
	      last_opcode() == 0x042f);
	send_event(e, 8);
	pump(4);
	CHECK("123456 is typed", btc_prompt_answer(&core, 1, 123456u));
	pump(4);
	CHECK("User Passkey Request Reply (0x042E) carries it little-endian",
	      last_opcode() == 0x042e && last_length() == 10 &&
	      has_address(last_params()) && last_params()[6] == 0x40 &&
	      last_params()[7] == 0xe2 && last_params()[8] == 0x01 &&
	      last_params()[9] == 0x00);
	CHECK("with nothing pending an answer does nothing",
	      !btc_prompt_answer(&core, 1, 0));

	/* Passkey Notification (0x3B): we show the digits for a keyboard to
	 * type. */
	{
		uint32_t generation = btc_passkey_generation;
		e[0] = 0x3b; e[1] = 10; memcpy(e + 2, PEER, 6);
		e[8] = 0x3f; e[9] = 0x42; e[10] = 0x0f; e[11] = 0x00; /* 999999 */
		send_event(e, 12);
		pump(4);
		CHECK("the passkey to display is kept", btc_passkey == 999999u);
		CHECK("and announced by its generation",
		      btc_passkey_generation != generation);
	}

	/* Simple Pairing Complete (0x36) withdraws any open question and
	 * reports a failure. */
	e[0] = 0x33; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x01; e[9] = e[10] = e[11] = 0;
	send_event(e, 12);
	pump(4);
	e[0] = 0x36; e[1] = 7; e[2] = 0x05; memcpy(e + 3, PEER, 6);
	send_event(e, 9);
	pump(4);
	CHECK("a completed pairing withdraws the question",
	      core.prompt_kind == BTC_PROMPT_NONE);
	CHECK("an authentication failure is reported to the user",
	      (core.last_problem & 0xff00u) == 0x0700u &&
	      (core.last_problem & 0xffu) == 0x05);
}

/* ------------------------------------------------------------- bonding */

/* The bond file grew at its end (consumer keys, controllers): an entry of
 * the first version is still read, and its kept handles are not trusted. */
static void bond_file(void)
{
	btc_key now, old, back;
	uint8_t image[sizeof(btc_key)];

	printf("the bond file\n");
	memset(&now, 0, sizeof now);
	now.used = 1; now.le = 1; now.key_size = 16;
	memset(now.key, 0x5a, 16);
	memcpy(now.address, "\1\2\3\4\5\6", 6);
	now.gatt.valid = 1; now.gatt.count = 1; now.gatt.value[0] = 0x2a;
	now.gatt.map.fields_used = 3;
	now.gatt.extras.consumer_used = 1;
	memcpy(image, &now, sizeof now);
	CHECK("an entry of this size is read as it is",
	      btc_key_unpack(&back, image, sizeof now) &&
	      memcmp(&back, &now, sizeof now) == 0);
	memset(&old, 0, sizeof old);
	CHECK("the first version's entry is read",
	      btc_key_unpack(&old, image, BTC_KEY_V1_BYTES));
	CHECK("with its key, address and map", old.used && old.le && old.key_size == 16 &&
	      old.key[5] == 0x5a && old.address[5] == 6 && old.gatt.map.fields_used == 3);
	CHECK("its kept handles are not trusted (the device is discovered again)",
	      old.gatt.valid == 0);
	CHECK("and what was added is empty", old.gatt.extras.consumer_used == 0);
	CHECK("any other size is refused", !btc_key_unpack(&old, image, 100) &&
	      !btc_key_unpack(&old, image, BTC_KEY_V1_BYTES + 1));
}

static void bonding(void)
{
	uint8_t e[32];
	btc_key *key;
	unsigned int m;
	static const uint8_t KEY[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
	                                 12, 13, 14, 15 };

	printf("bonding\n");
	rig();

	/* Link Key Request (0x17) for a stranger: Link Key Request Negative
	 * Reply (0x040C), which makes the controller start pairing. */
	m = mark();
	e[0] = 0x17; e[1] = 6; memcpy(e + 2, PEER, 6);
	send_event(e, 8);
	pump(4);
	CHECK("an unknown peer gets Link Key Request Negative Reply (0x040C)",
	      sent_count > m && last_opcode() == 0x040c &&
	      has_address(last_params()));

	/* Link Key Notification (0x18): address, key, key type 4
	 * (unauthenticated combination, P-192). The core keeps it. */
	e[0] = 0x18; e[1] = 23; memcpy(e + 2, PEER, 6);
	memcpy(e + 8, KEY, 16);
	e[24] = 0x04;
	send_event(e, 25);
	pump(4);
	key = btc_key_find(&core, PEER);
	CHECK("a Link Key Notification stores the key", key != 0);
	if (key == 0) return;
	CHECK("the key is the notified one", memcmp(key->key, KEY, 16) == 0);
	CHECK("its type is kept", key->key_type == 0x04);
	CHECK("it is a classic key", key->le == 0);
	CHECK("the key generation tells the app to save", core.keys_generation != 0);

	/* The next Link Key Request is answered with the key: Link Key
	 * Request Reply (0x040B), address then key. */
	m = mark();
	e[0] = 0x17; e[1] = 6; memcpy(e + 2, PEER, 6);
	send_event(e, 8);
	pump(4);
	CHECK("a known peer gets Link Key Request Reply (0x040B)",
	      sent_count > m && last_opcode() == 0x040b && last_length() == 22 &&
	      has_address(last_params()) && memcmp(last_params() + 6, KEY, 16) == 0);

	/* Forget removes it, and the peer is a stranger again. */
	{
		uint32_t generation = core.keys_generation;
		CHECK("forgetting succeeds", btc_key_forget(&core, PEER));
		CHECK("the key has gone", btc_key_find(&core, PEER) == 0);
		CHECK("forgetting bumps the key generation",
		      core.keys_generation != generation);
	}
	m = mark();
	send_event(e, 8);
	pump(4);
	CHECK("a forgotten peer is refused again (0x040C)",
	      last_opcode() == 0x040c);

	/* Legacy pairing: PIN Code Request (0x16), answered 0000 with PIN
	 * Code Request Reply (0x040D): address, length, 16 octets padded. */
	m = mark();
	e[0] = 0x16; e[1] = 6; memcpy(e + 2, PEER, 6);
	send_event(e, 8);
	pump(4);
	CHECK("PIN Code Request Reply (0x040D) answers a legacy request",
	      sent_count > m && last_opcode() == 0x040d && last_length() == 23);
	CHECK("the PIN is 0000",
	      last_params()[6] == 4 && memcmp(last_params() + 7, "0000", 4) == 0 &&
	      last_params()[11] == 0 && last_params()[22] == 0);

	/* Connect on demand: Create Connection (0x0405) to a bonded device.
	 * Address, packet types DM1/DH1/DM3/DH3/DM5/DH5 (0xCC18), page scan
	 * repetition mode, reserved, clock offset, allow role switch. */
	{
		btc_device *d = btc_device_add(&core, PEER);
		m = mark();
		if (d != 0) { d->page_scan_rep_mode = 1; d->clock_offset = 0x1234; }
		CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
		pump(4);
		CHECK("Create Connection (0x0405) is sent",
		      sent_count > m && last_opcode() == 0x0405 &&
		      last_length() == 13 && has_address(last_params()));
		CHECK("a second connect to the same address does not duplicate",
		      btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
		{
			unsigned int links = 0, i;
			for (i = 0; i < BTC_LINKS; i++)
				if (core.links[i].state != BTC_LINK_FREE) links++;
			CHECK("exactly one link exists", links == 1);
		}
		/* Connection Complete (0x03): status 0, handle 0x0040,
		 * address, link type 1 (ACL), encryption 0. */
		e[0] = 0x03; e[1] = 11; e[2] = 0x00; e[3] = 0x40; e[4] = 0x00;
		memcpy(e + 5, PEER, 6); e[11] = 0x01; e[12] = 0x00;
		send_event(e, 13);
		pump(4);
		{
			btc_link *link = btc_link_by_address(&core, PEER);
			CHECK("Connection Complete opens the link",
			      link != 0 && link->state == BTC_LINK_OPEN &&
			      link->handle == 0x0040);
		}
	}
}

static unsigned int count_opcode(unsigned int from, int opcode)
{
	unsigned int i, n = 0;

	for (i = from; i < sent_count; i++)
		if (opcode_of(i) == opcode)
			n++;
	return n;
}

/* A speaker that reconnects to us: the remote starts authentication, so
 * the host does not, and a collision (HCI error 0x23, Core Vol 1 Part F)
 * is retried and never shown as a failure. */
static void collisions(void)
{
	uint8_t e[16];
	unsigned int m;
	btc_key *key;

	printf("collisions\n");
	rig();
	key = btc_key_store(&core, PEER, 0);
	key->flags = BTC_DEV_AUDIO_SINK;

	/* Connection Request (0x04): address, class, ACL link type. */
	e[0] = 0x04; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x14; e[9] = 0x04; e[10] = 0x24; e[11] = 0x01;
	send_event(e, 12);
	pump(4);
	CHECK("a paired device's connection is accepted (0x0409)",
	      count_opcode(0, 0x0409) == 1);
	e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = 0x41; e[4] = 0;
	memcpy(e + 5, PEER, 6); e[11] = 1; e[12] = 0;
	send_event(e, 13);
	pump(4);
	m = sent_count;
	CHECK("the incoming link is open",
	      btc_link_by_address(&core, PEER) != 0 &&
	      btc_link_by_address(&core, PEER)->state == BTC_LINK_OPEN);
	CHECK("a profile asks for security", btc_secure(&core, 0));
	pump(20);
	CHECK("we do not start authentication on an incoming link at once",
	      count_opcode(m, 0x0411) == 0);
	clock_ms += 3000;
	pump(4);
	CHECK("after the remote's turn has passed we do (0x0411)",
	      count_opcode(m, 0x0411) == 1);

	/* Authentication Complete with 0x23: retry, not a failure. */
	core.last_problem = 0;
	e[0] = 0x06; e[1] = 3; e[2] = 0x23; e[3] = 0x41; e[4] = 0;
	send_event(e, 5);
	pump(4);
	CHECK("a collision is not reported as a failure",
	      core.last_problem == 0 && core.links[0].state == BTC_LINK_OPEN);
	m = sent_count;
	clock_ms += 1500;
	pump(6);
	CHECK("and the procedure is tried again", count_opcode(m, 0x0411) >= 1);

	/* An outgoing link authenticates at once. */
	rig();
	key = btc_key_store(&core, PEER, 0);
	CHECK("an outgoing connect", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_A2DP));
	pump(4);
	e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = 0x42; e[4] = 0;
	memcpy(e + 5, PEER, 6); e[11] = 1; e[12] = 0;
	send_event(e, 13);
	pump(4);
	m = sent_count;
	CHECK("the profile secures it", btc_secure(&core, 0));
	pump(6);
	CHECK("an outgoing link authenticates at once",
	      count_opcode(m, 0x0411) == 1);
}

static void discoverable(void)
{
	printf("discoverable\n");
	rig();
	btc_discoverable(&core, 1);
	pump(4);
	/* Write Scan Enable (0x0C1A): 0x03 is inquiry scan and page scan. */
	CHECK("discoverable writes Scan Enable (0x0C1A) = 3",
	      last_opcode() == 0x0c1a && last_length() == 1 &&
	      last_params()[0] == 0x03 && core.discoverable);
	btc_discoverable(&core, 0);
	pump(4);
	CHECK("not discoverable keeps page scan only: 2",
	      last_opcode() == 0x0c1a && last_params()[0] == 0x02 &&
	      !core.discoverable);
}

/* Number Of Completed Packets: several handles in one event, and a
 * reconnect with events for the old handle still arriving. */
static void credits(void)
{
	uint8_t e[24];
	btc_link *a, *b;

	printf("credits\n");
	rig();
	a = &core.links[0]; b = &core.links[1];
	memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
	a->state = BTC_LINK_OPEN; a->handle = 0x41; a->outstanding = 3;
	b->state = BTC_LINK_OPEN; b->handle = 0x42; b->outstanding = 2;
	core.acl_free = 3;                      /* 8 buffers, 5 in flight */

	/* Two handles in one event: (0x41, 2) and (0x42, 1). */
	e[0] = 0x13; e[1] = 9; e[2] = 2;
	e[3] = 0x41; e[4] = 0; e[5] = 2; e[6] = 0;
	e[7] = 0x42; e[8] = 0; e[9] = 1; e[10] = 0;
	send_event(e, 11);
	pump(4);
	CHECK("each handle is credited its own packets",
	      a->outstanding == 1 && b->outstanding == 1);
	CHECK("and the shared pool gets all three back", core.acl_free == 6);

	/* More than are in flight is capped, not credited. */
	e[0] = 0x13; e[1] = 5; e[2] = 1;
	e[3] = 0x41; e[4] = 0; e[5] = 5; e[6] = 0;
	send_event(e, 7);
	pump(4);
	CHECK("a count above the packets in flight gives no extra credit",
	      a->outstanding == 0 && core.acl_free == 7);

	/* An unknown handle is ignored. */
	e[3] = 0x77;
	send_event(e, 7);
	pump(4);
	CHECK("an unknown handle changes nothing", core.acl_free == 7);

	/* The link to b drops with two packets still counted (one left): its
	 * credit returns at once; the late event for the old handle is then
	 * ignored. */
	core.links[1].outstanding = 2;
	core.acl_free = 5;
	e[0] = 0x05; e[1] = 4; e[2] = 0; e[3] = 0x42; e[4] = 0; e[5] = 0x13;
	send_event(e, 6);
	pump(4);
	e[0] = 0x13; e[1] = 5; e[2] = 1; e[4] = 0; e[6] = 0;
	CHECK("a closed link hands back its credits", core.acl_free == 7);
	e[3] = 0x42; e[5] = 2;
	send_event(e, 7);
	pump(4);
	CHECK("a late event for the old handle adds nothing", core.acl_free == 7);

	/* The same handle comes back as a new link: fresh accounting. */
	b = &core.links[1];
	b->state = BTC_LINK_OPEN; b->handle = 0x42; b->outstanding = 1;
	core.acl_free = 6;
	e[5] = 1;
	send_event(e, 7);
	pump(4);
	CHECK("a reconnected link under the same handle is credited again",
	      b->outstanding == 0 && core.acl_free == 7);
}

/* The whole bring-up, as the module's ticker drives it: start, then pump
 * every 10 ms until Ready. */
static void bringup(void)
{
	unsigned int i;
	char what[64];

	printf("bring-up\n");
	memset(sent, 0, sizeof(sent));
	sent_count = 0;
	queue_head = queue_tail = 0;
	full_controller = 1;
	CHECK("the core starts", btc_start(&core, (struct btc_profiles *)profiles_storage,
	                                   &completion, clock_ms));
	/* The bridge did its own reset and patch first, which took 8 seconds:
	 * the core's clock has moved on since btc_start was given its time. */
	clock_ms += 8000;
	for (i = 0; i < 300 && core.state != BTC_READY &&
	     core.state != BTC_FAILED; i++)
		pump(1);
	snprintf(what, sizeof what, "the core reaches Ready (state %d error %x, "
	         "%u commands sent, config step %u)", core.state,
	         (unsigned)core.error, sent_count, core.config_step);
	CHECK(what, core.state == BTC_READY);
	CHECK("bring-up took fewer than 300 pumps", i < 300);
	full_controller = 0;
}

/* The desktop must not wait for the controller: btc_start returns at once,
 * the core stays Starting (and sends nothing) while the transport brings
 * the controller up in the background, then the HCI bring-up follows. */
static void background_start(void)
{
	unsigned int i, sent_before;

	printf("start-up in the background\n");
	memset(sent, 0, sizeof(sent));
	sent_count = 0;
	queue_head = queue_tail = 0;
	full_controller = 1;
	transport_pumps = 50;
	transport_calls = 0;
	CHECK("btc_start returns at once, without calling the transport's start-up",
	      btc_start(&core, (struct btc_profiles *)profiles_storage, &completion,
	                clock_ms) && transport_calls == 0);
	CHECK("the core is Starting (BOOTING)", core.state == BTC_BOOTING);
	for (i = 0; i < 40; i++)
		pump(1);
	sent_before = sent_count;
	CHECK("40 ticks later the transport is still starting: still Starting, nothing sent",
	      core.state == BTC_BOOTING && sent_before == 0 && transport_calls == 40);
	CHECK("a request now is refused, not queued into a half-started controller",
	      !btc_connect(&core, (const uint8_t *)"\1\2\3\4\5\6", 0, 0, BTC_PROFILE_A2DP));
	for (i = 0; i < 400 && core.state != BTC_READY && core.state != BTC_FAILED; i++)
		pump(1);
	CHECK("then the HCI bring-up runs to Ready", core.state == BTC_READY);
	CHECK("the first HCI command came only after the transport finished",
	      sent_count > 0 && transport_calls > 50);

	transport_pumps = 5;
	transport_calls = 0;
	(void)btc_start(&core, (struct btc_profiles *)profiles_storage, &completion,
	                clock_ms);
	/* The transport reports failure. */
	transport_pumps = 1000000;
	CHECK("a transport that never finishes keeps the core Starting",
	      (pump(100), core.state == BTC_BOOTING));
	full_controller = 0;
	transport_pumps = 0;
}


/* The 32-bit build's USB adapter loses the odd control transfer: the core
 * sends the same command again (three times) before it gives up. */
static void command_resend(void)
{
	unsigned int before;

	printf("command resend\n");
	rig();
	before = sent_count;
	fail_commands = 2;
	btc_discoverable(&core, 1);
	pump(10);
	CHECK("a command lost twice is sent three times",
	      sent_count == before + 3 && opcode_of(before) == 0x0c1a &&
	      opcode_of(before + 1) == 0x0c1a && opcode_of(before + 2) == 0x0c1a);
	CHECK("and the core is still ready", core.state == BTC_READY);
	CHECK("with the answer to the third taken", core.command_waiting == 0);
	CHECK("two resends counted", core.stats.command_resends == 2);

	rig();
	fail_commands = 4;
	btc_discoverable(&core, 0);
	pump(10);
	CHECK("a command lost four times fails the core (error &102)",
	      core.error == 0x102);
	fail_commands = 0;
}


/* While A2DP is up the CSR radio has little time to spare: LE scanning and
 * LE connecting drop to 11.25 ms in 160 (the 32-bit build's 0.72). */
static void low_duty_le(void)
{
	static const uint8_t mouse[6] = { 0xba, 0xbf, 0x95, 0xa5, 0xa1, 0xf1 };
	unsigned int before, i;
	int params_at = -1, enable_at = -1;

	printf("low-duty LE while audio\n");
	rig();
	core.audio_busy = 1;
	before = sent_count;
	btc_le_background(&core, 1);
	pump(10);
	for (i = before; i < sent_count; i++) {
		if (opcode_of(i) == 0x200b && params_at < 0) params_at = (int)i;
		if (opcode_of(i) == 0x200c && enable_at < 0) enable_at = (int)i;
	}
	CHECK("scan parameters are set before the scan starts",
	      params_at >= 0 && enable_at > params_at);
	if (params_at >= 0) {
		const uint8_t *p = sent[params_at].bytes + 3;
		CHECK("to 160 ms interval, 11.25 ms window",
		      (p[1] | p[2] << 8) == 0x0100 && (p[3] | p[4] << 8) == 0x0012);
	}
	CHECK("and the duty is remembered", core.le_scan_duty == 2);

	rig();
	core.audio_busy = 1;
	CHECK("an LE connection can be asked for",
	      btc_connect(&core, mouse, 1, 1, BTC_PROFILE_LE_HID) != 0);
	pump(6);
	for (i = 0, params_at = -1; i < sent_count; i++)
		if (opcode_of(i) == 0x200d) params_at = (int)i;
	CHECK("LE Create Connection is sent", params_at >= 0);
	if (params_at >= 0) {
		const uint8_t *p = sent[params_at].bytes + 3;
		CHECK("and scans 11.25 ms in 160 while audio is up",
		      (p[0] | p[1] << 8) == 0x0100 && (p[2] | p[3] << 8) == 0x0012);
	}

	rig();
	core.audio_busy = 0;
	btc_connect(&core, mouse, 1, 1, BTC_PROFILE_LE_HID);
	pump(6);
	for (i = 0, params_at = -1; i < sent_count; i++)
		if (opcode_of(i) == 0x200d) params_at = (int)i;
	CHECK("without audio it scans 30 ms in 60",
	      params_at >= 0 &&
	      (sent[params_at].bytes[3] | sent[params_at].bytes[4] << 8) == 0x0060 &&
	      (sent[params_at].bytes[5] | sent[params_at].bytes[6] << 8) == 0x0030);
}


/* A classic link gets multi-slot packets whichever side made it. */
static void packet_types(void)
{
	static const uint8_t spk[6] = { 0xca, 0x36, 0x58, 0xfa, 0x58, 0xfc };
	uint8_t e[16];
	unsigned int before, i;
	int at = -1;

	printf("packet types\n");
	rig();
	CHECK("a classic connection can be asked for",
	      btc_connect(&core, spk, 0, 0, BTC_PROFILE_A2DP) != 0);
	pump(4);
	before = sent_count;
	/* Connection Complete: status 0, handle 0x0047, the address, ACL. */
	e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = 0x47; e[4] = 0;
	memcpy(e + 5, spk, 6); e[11] = 1; e[12] = 0;
	send_event(e, 13);
	pump(6);
	for (i = before; i < sent_count; i++)
		if (opcode_of(i) == 0x040f) at = (int)i;
	CHECK("Change Connection Packet Type is sent when the link opens", at >= 0);
	if (at >= 0)
		CHECK("for its handle, with DM/DH 1, 3 and 5 (0xcc18)",
		      (sent[at].bytes[3] | sent[at].bytes[4] << 8) == 0x47 &&
		      (sent[at].bytes[5] | sent[at].bytes[6] << 8) == 0xcc18);
}


/* While A2DP plays, a scan's inquiry runs in 1.28 s bursts 2.5 s apart,
 * and no names are asked for: each would hold the radio. */
static void scan_with_audio(void)
{
	uint8_t done[3] = { 0x01, 1, 0 };
	unsigned int before, i, inquiries = 0, short_ones = 0, names = 0;

	printf("scanning while audio plays\n");
	rig();
	core.audio_busy = 1;
	before = sent_count;
	CHECK("a scan starts", btc_scan(&core, 10) != 0);
	for (i = 0; i < 3; i++) {
		pump(20);
		send_event(done, 3);            /* Inquiry Complete */
		pump(300);                      /* 3 s */
	}
	pump(100);
	for (i = before; i < sent_count; i++) {
		if (opcode_of(i) == 0x0401) {
			++inquiries;
			if (sent[i].bytes[3 + 3] == 1) ++short_ones;
		}
		if (opcode_of(i) == 0x0419) ++names;
	}
	CHECK("three inquiries, not one long one", inquiries == 3);
	CHECK("each 1.28 s long", short_ones == 3);
	CHECK("no Remote Name Requests while audio plays", names == 0);
	CHECK("and the scan finishes", !btc_scanning(&core));
	core.audio_busy = 0;
}

int main(void)
{
	bringup();
	background_start();
	collisions();
	discoverable();
	credits();
	discovery();
	names();
	crowded();
	connect_retry();
	incoming();
	inquiry_retry();
	pairing();
	bonding();
	bond_file();
	command_resend();
	low_duty_le();
	packet_types();
	scan_with_audio();
	return check_summary("btcore");
}
