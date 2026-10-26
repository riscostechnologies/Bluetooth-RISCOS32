/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for BluetoothVoice's HID input (modules/network/bluetooth/
 * c/hid_input.c): boot-protocol keyboard and mouse reports as a Bluetooth
 * keyboard and mouse send them on the HID interrupt channel, and the
 * RISC OS key numbers and pointer movement they become. The usages are the
 * HID Usage Tables', page 7 (keyboard): 0x04 is "a", 0xE1 left shift.
 */

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "hid_input.h"

struct log {
	int keys[32];
	int count;
	int dx, dy;
	unsigned int dz_extra;
};

static void on_key(void *context, int down, unsigned int key)
{
	struct log *l = context;

	if (l->count < 32)
		l->keys[l->count++] = down ? (int)key : -(int)key - 1;
}

static void on_pointer(void *context, int dx, int dy)
{
	struct log *l = context;

	l->dx += dx;
	l->dy += dy;
}

static void on_wheel(void *context, int dz, int dw, unsigned int extra)
{
	struct log *l = context;

	(void)dz; (void)dw;
	l->dz_extra |= extra;
}

int main(void)
{
	struct log log;
	btv_hid_sink sink;
	btv_hid_keyboard kb;
	btv_hid_mouse mouse;
	const uint8_t *payload;
	size_t payload_bytes;
	/* A1 01 = a DATA input report, id 1 (keyboard), then the boot report. */
	static const uint8_t frame_a_down[] =
		{ 0xa1, 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
	static const uint8_t frame_mouse[] = { 0xa1, 0x02, 0x01, 0x05, 0xfb };
	static const uint8_t shift_a[] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
	static const uint8_t a_only[] = { 0x00, 0, 0x04, 0, 0, 0, 0, 0 };
	static const uint8_t none[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t rollover[] = { 0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };

	memset(&log, 0, sizeof(log));
	memset(&sink, 0, sizeof(sink));
	sink.context = &log;
	sink.key = on_key;
	sink.pointer = on_pointer;
	sink.wheel = on_wheel;

	printf("hid\n");
	CHECK("an interrupt-channel keyboard frame is report 1",
	      btv_hid_classic_input(frame_a_down, sizeof(frame_a_down), &payload,
	                            &payload_bytes) == 1 && payload_bytes == 8);
	CHECK("a mouse frame is report 2",
	      btv_hid_classic_input(frame_mouse, sizeof(frame_mouse), &payload,
	                            &payload_bytes) == 2 && payload_bytes == 3);
	CHECK("a handshake frame is not input",
	      btv_hid_classic_input((const uint8_t[]){ 0x00 }, 1, &payload,
	                            &payload_bytes) == 0);

	btv_hid_keyboard_init(&kb);
	btv_hid_keyboard_report(&kb, a_only, sizeof(a_only), &sink);
	CHECK("'a' down is one key down: RISC OS key A",
	      log.count == 1 && log.keys[0] == (int)BTV_KEY_LetterA);
	btv_hid_keyboard_report(&kb, a_only, sizeof(a_only), &sink);
	CHECK("a repeated report adds nothing", log.count == 1);
	btv_hid_keyboard_report(&kb, none, sizeof(none), &sink);
	CHECK("all keys up releases 'a'",
	      log.count == 2 && log.keys[1] == -(int)BTV_KEY_LetterA - 1);
	log.count = 0;
	btv_hid_keyboard_report(&kb, shift_a, sizeof(shift_a), &sink);
	CHECK("shift then 'a': the modifier comes first",
	      log.count == 2 && log.keys[0] == (int)BTV_KEY_ShiftLeft &&
	      log.keys[1] == (int)BTV_KEY_LetterA);
	log.count = 0;
	btv_hid_keyboard_report(&kb, rollover, sizeof(rollover), &sink);
	CHECK("an ErrorRollOver report is ignored", log.count == 0);
	btv_hid_keyboard_release_all(&kb, &sink);
	CHECK("disconnecting releases the held keys", log.count == 2);
	CHECK("a LED mask is a boot output report byte",
	      btv_hid_keyboard_leds(0) == 0);

	btv_hid_mouse_init(&mouse);
	memset(&log, 0, sizeof(log));
	btv_hid_mouse_report(&mouse, payload, 3, &sink);
	CHECK("a mouse report moves right 5", log.dx == 5);
	CHECK("HID y is down, RISC OS y is up: -5 becomes up 5", log.dy == 5);
	return check_summary("hid");
}
