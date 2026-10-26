/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for HID report-mode input (modules/network/bluetooth/c/
 * hid_report.c): a report descriptor in the style of a Logitech MX Master
 * (one mouse report with an ID, 16 buttons, 16-bit relative X and Y, an
 * 8-bit wheel and an AC Pan, plus vendor-defined HID++ reports that must be
 * ignored), a keyboard with report IDs, and the decoded reports reaching the
 * same sink as boot-protocol devices.
 *
 * The descriptors are written from the HID 1.11 item encoding and the Usage
 * Tables; they are modelled on the device, not copied from one.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "hid_input.h"
#include "hid_report.h"
#include "mxmap.h"

struct log {
	int keys[16], count, dx, dy, dz, dw;
	unsigned int extra;
};

static void on_key(void *c, int down, unsigned int key)
{
	struct log *l = c;
	if (l->count < 16) l->keys[l->count++] = down ? (int)key : -(int)key - 1;
}
static void on_pointer(void *c, int dx, int dy)
{
	struct log *l = c;
	l->dx += dx; l->dy += dy;
}
static void on_wheel(void *c, int dz, int dw, unsigned int extra)
{
	struct log *l = c;
	l->dz += dz; l->dw += dw; l->extra = extra;
}

static void parse(btv_hid_map *m, const uint8_t *d, size_t n, size_t piece)
{
	size_t i;
	btv_hid_map_init(m);
	for (i = 0; i < n; i += piece)
		btv_hid_map_feed(m, d + i, n - i < piece ? n - i : piece);
}

static void descriptor(void)
{
	btv_hid_map a, b, c;
	btv_hid_event e;
	uint8_t report[] = { 0x05, 0x00, 0xd4, 0xfe, 0xc8, 0x00, 0xff, 0x02 };

	printf("a mouse descriptor with report IDs\n");
	parse(&a, mouse_map, sizeof(mouse_map), sizeof(mouse_map));
	parse(&b, mouse_map, sizeof(mouse_map), 22);       /* a Report Map read */
	parse(&c, mouse_map, sizeof(mouse_map), 1);        /* worst case */
	CHECK("it holds a usable pointer", btv_hid_map_finish(&a) && a.has_pointer);
	CHECK("and no keyboard", !a.has_keyboard);
	CHECK("read in 22-octet pieces it parses the same",
	      btv_hid_map_finish(&b) && memcmp(a.fields, b.fields, sizeof(a.fields)) == 0 &&
	      a.fields_used == b.fields_used);
	CHECK("one octet at a time too", btv_hid_map_finish(&c) &&
	      memcmp(a.fields, c.fields, sizeof(a.fields)) == 0);
	CHECK("report 2 is wanted", btv_hid_map_wants(&a, 2));
	CHECK("the HID++ reports (0x10, 0x11) are not", !btv_hid_map_wants(&a, 0x10) &&
	      !btv_hid_map_wants(&a, 0x11));
	CHECK("nor the feature report (3)", !btv_hid_map_wants(&a, 3));
	CHECK("five fields: buttons, X, Y, wheel, pan", a.fields_used == 5);
	CHECK("the 16 buttons are one run of 16 bits at offset 0",
	      a.fields[0].kind == BTV_HIDF_BUTTONS && a.fields[0].count == 16 &&
	      a.fields[0].offset == 0 && a.fields[0].aux == 1);
	CHECK("X is 16 bits signed at 16, Y at 32",
	      a.fields[1].kind == BTV_HIDF_X && a.fields[1].size == 16 &&
	      a.fields[1].is_signed && a.fields[1].offset == 16 && a.fields[1].relative &&
	      a.fields[2].kind == BTV_HIDF_Y && a.fields[2].offset == 32);
	CHECK("the wheel is 8 bits at 48 and the pan 8 bits at 56",
	      a.fields[3].kind == BTV_HIDF_WHEEL && a.fields[3].size == 8 &&
	      a.fields[3].offset == 48 && a.fields[4].kind == BTV_HIDF_PAN &&
	      a.fields[4].offset == 56);

	CHECK("a report decodes", btv_hid_map_decode(&a, 2, report, sizeof(report), &e));
	CHECK("buttons 1 and 3 are down", e.buttons == 5u);
	CHECK("X is -300 and Y is 200 (16 bits, sign extended)", e.dx == -300 && e.dy == 200);
	CHECK("the wheel is -1 and the pan 2", e.wheel == -1 && e.pan == 2);
	CHECK("a short report decodes what it has (the rest reads as zero)",
	      btv_hid_map_decode(&a, 2, report, 4, &e) && e.dx == -300 && e.dy == 0);
	CHECK("another ID decodes nothing", !btv_hid_map_decode(&a, 0x10, report, 8, &e));
}

static void keyboard(void)
{
	btv_hid_map m;
	btv_hid_event e;
	/* left shift (bit 1), keys 'a' and 'b'. */
	uint8_t report[] = { 0x02, 0x00, 0x04, 0x05, 0, 0, 0, 0 };

	printf("a keyboard descriptor\n");
	parse(&m, keyboard_map, sizeof(keyboard_map), 22);
	CHECK("it holds a keyboard", btv_hid_map_finish(&m) && m.has_keyboard &&
	      !m.has_pointer);
	CHECK("report 1 is wanted", btv_hid_map_wants(&m, 1));
	CHECK("the LED output report is noticed", m.has_leds && m.led_id == 1);
	CHECK("the report decodes", btv_hid_map_decode(&m, 1, report, 8, &e));
	CHECK("left shift", e.modifiers == 0x02);
	CHECK("and keys a and b", e.keys[0] == 0x04 && e.keys[1] == 0x05 && e.keys[2] == 0);
}

static void into_the_sink(void)
{
	btv_hid_map m;
	btv_hid_event e;
	btv_hid_mouse mouse;
	struct log l;
	btv_hid_sink sink;
	uint8_t press[] = { 0x05, 0x00, 0x0a, 0x00, 0x14, 0x00, 0x00, 0x00 };
	uint8_t up[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t scroll[] = { 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x01 };

	printf("decoded reports reach the pointer path\n");
	parse(&m, mouse_map, sizeof(mouse_map), 22);
	(void)btv_hid_map_finish(&m);
	btv_hid_mouse_init(&mouse);
	mouse.opt.accel = 0;           /* the arithmetic of feel.c is not under test here */
	memset(&l, 0, sizeof(l));
	sink.context = &l; sink.key = on_key; sink.pointer = on_pointer; sink.wheel = on_wheel;

	(void)btv_hid_map_decode(&m, 2, press, 8, &e);
	btv_hid_mouse_event(&mouse, e.buttons, e.dx, e.dy, e.wheel, e.pan, &sink);
	CHECK("movement: x right 10, y down 20 is RISC OS y -20", l.dx == 10 && l.dy == -20);
	CHECK("buttons 1 and 3: Select and Menu down",
	      l.count == 2 && l.keys[0] == BTV_KEY_LeftMouse && l.keys[1] == BTV_KEY_CentreMouse);
	memset(&l, 0, sizeof(l)); l.count = 0;
	(void)btv_hid_map_decode(&m, 2, up, 8, &e);
	btv_hid_mouse_event(&mouse, e.buttons, e.dx, e.dy, e.wheel, e.pan, &sink);
	CHECK("release: both up", l.count == 2 && l.keys[0] == -(int)BTV_KEY_LeftMouse - 1);
	memset(&l, 0, sizeof(l));
	(void)btv_hid_map_decode(&m, 2, scroll, 8, &e);
	btv_hid_mouse_event(&mouse, e.buttons, e.dx, e.dy, e.wheel, e.pan, &sink);
	CHECK("buttons 4 and 5 (back, forward) travel with the wheel call",
	      l.extra == 3u);
	CHECK("wheel -2 (towards the user) is +2 in the kernel's sense, pan 1",
	      l.dz == 2 && l.dw == 1);
	CHECK("no pointer movement", l.dx == 0 && l.dy == 0);
}

static void odd_descriptors(void)
{
	btv_hid_map m;
	static const uint8_t truncated[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x95 };
	static const uint8_t boot_like[] = {
		0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
		0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
		0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
		0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
		0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0
	};
	static const uint8_t long_item[] = { 0xfe, 0x02, 0x10, 0xaa, 0xbb,
		0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x01,
		0x95, 0x01, 0x75, 0x01, 0x81, 0x02, 0xc0 };

	printf("odd descriptors\n");
	parse(&m, truncated, sizeof(truncated), 4);
	CHECK("a descriptor cut off inside an item is not usable", !btv_hid_map_finish(&m));
	parse(&m, boot_like, sizeof(boot_like), 5);
	CHECK("a boot-style mouse without report IDs works",
	      btv_hid_map_finish(&m) && btv_hid_map_wants(&m, 0) && m.fields_used == 3);
	{
		btv_hid_event e;
		uint8_t r[] = { 0x01, 0x05, 0xfb };
		CHECK("and decodes", btv_hid_map_decode(&m, 0, r, 3, &e) &&
		      e.buttons == 1 && e.dx == 5 && e.dy == -5);
	}
	parse(&m, long_item, sizeof(long_item), 3);
	CHECK("a long item is skipped", btv_hid_map_finish(&m) && m.fields_used == 1);
	btv_hid_map_init(&m);
	CHECK("an empty map has nothing", !btv_hid_map_finish(&m));
}


/* Descriptors of the kinds of device a person owns, driven report by report
 * through the same paths as a Bluetooth device: descriptor -> decoded event
 * -> RISC OS key numbers and pointer movement.
 *   - the boot keyboard and the boot mouse, exactly as HID 1.11 Appendix B
 *     prints them (B.1, B.2);
 *   - a three-button wheel mouse of the Microsoft kind (HID 1.11 7-bit-less
 *     form: three axes in one item, the usual descriptor of such mice);
 *   - a Logitech Unifying-style mouse: report ID 2, 16 buttons, two 12-bit
 *     axes, wheel and AC Pan (the structure Logitech's mice use);
 *   - an Apple-style keyboard: keys under report ID 1, the Fn key and the
 *     media keys under other IDs and a vendor page, which must not be taken
 *     for keys.
 * The first two are copied from the specification; the last two are built
 * from the published structure of those devices, not captured from them. */
static const uint8_t boot_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xc0
};
static const uint8_t boot_mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xc0, 0xc0
};
static const uint8_t wheel_mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
	0x81, 0x06, 0xc0, 0xc0
};
static const uint8_t unifying_mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10,
	0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x16, 0x01, 0xf8, 0x26, 0xff, 0x07,
	0x75, 0x0c, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06, 0x15, 0x81,
	0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06, 0x05, 0x0c,
	0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0
};
static const uint8_t apple_keyboard[] = {
	/* Keyboard, ID 1: modifiers, reserved, LEDs, six keys. */
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0,
	0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08,
	0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19,
	0x00, 0x2a, 0xff, 0x00, 0x81, 0x00, 0xc0,
	/* Fn / eject on a vendor page, ID 0x11: one bit and padding. */
	0x06, 0x00, 0xff, 0x09, 0x03, 0xa1, 0x01, 0x85, 0x11, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x03, 0x81, 0x02, 0x75, 0x07, 0x95,
	0x01, 0x81, 0x03, 0xc0,
	/* Media keys, ID 0x12 (consumer page, bits). */
	0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x12, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x04, 0x09, 0xb5, 0x09, 0xb6, 0x09, 0xcd, 0x09, 0xe2,
	0x81, 0x02, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0
};

static void realistic(void)
{
	btv_hid_map m;
	btv_hid_event e;
	btv_hid_keyboard kb;
	btv_hid_mouse mouse;
	btv_hid_sink sink;
	struct log l;

	memset(&l, 0, sizeof l);
	sink.context = &l; sink.key = on_key; sink.pointer = on_pointer;
	sink.wheel = on_wheel; sink.launch = 0;

	printf("realistic descriptors: the boot keyboard (HID 1.11 B.1)\n");
	parse(&m, boot_keyboard, sizeof boot_keyboard, 22);
	CHECK("usable: a keyboard with LEDs and no report ID",
	      btv_hid_map_finish(&m) && m.has_keyboard && !m.has_pointer &&
	      m.has_leds && m.led_id == 0 && btv_hid_map_wants(&m, 0));
	btv_hid_keyboard_init(&kb);
	{
		/* Right shift (bit 5) and 'a' (4) and the right arrow (0x4f). */
		uint8_t down[8] = { 0x20, 0, 0x04, 0x4f, 0, 0, 0, 0 };
		uint8_t again[8] = { 0x20, 0, 0x4f, 0, 0, 0, 0, 0 };
		uint8_t up[8] = { 0 };

		CHECK("the report decodes", btv_hid_map_decode(&m, 0, down, 8, &e) &&
		      e.keyboard && e.modifiers == 0x20 && e.keys[0] == 0x04 &&
		      e.keys[1] == 0x4f);
		btv_hid_keyboard_report(&kb, down, 8, &sink);
		CHECK("RISC OS: right shift, A and cursor right go down, in that order",
		      l.count == 3 && l.keys[0] == BTV_KEY_ShiftRight &&
		      l.keys[1] == BTV_KEY_LetterA && l.keys[2] == BTV_KEY_CursorRight);
		l.count = 0;
		btv_hid_keyboard_report(&kb, again, 8, &sink);
		CHECK("A released while the arrow stays down: one key up, no repeat",
		      l.count == 1 && l.keys[0] == -(int)BTV_KEY_LetterA - 1);
		l.count = 0;
		btv_hid_keyboard_report(&kb, up, 8, &sink);
		CHECK("all up: the arrow and the shift are released",
		      l.count == 2 && l.keys[0] == -(int)BTV_KEY_ShiftRight - 1 &&
		      l.keys[1] == -(int)BTV_KEY_CursorRight - 1);
	}

	printf("realistic descriptors: the boot mouse (B.2) and a wheel mouse\n");
	memset(&l, 0, sizeof l);
	parse(&m, boot_mouse, sizeof boot_mouse, 22);
	{
		uint8_t r[3] = { 0x03, 0x05, 0xfb };
		CHECK("the boot mouse is a pointer", btv_hid_map_finish(&m) &&
		      m.has_pointer && btv_hid_map_decode(&m, 0, r, 3, &e) &&
		      e.buttons == 3 && e.dx == 5 && e.dy == -5 && e.wheel == 0);
	}
	parse(&m, wheel_mouse, sizeof wheel_mouse, 22);
	btv_hid_mouse_init(&mouse);
	mouse.opt.accel = 0;
	{
		uint8_t r[4] = { 0x05, 0x0a, 0x14, 0xff };

		CHECK("the wheel mouse: buttons 1 and 3, x 10, y 20, wheel -1",
		      btv_hid_map_finish(&m) && btv_hid_map_decode(&m, 0, r, 4, &e) &&
		      e.buttons == 5 && e.dx == 10 && e.dy == 20 && e.wheel == -1);
		btv_hid_mouse_event(&mouse, e.buttons, e.dx, e.dy, e.wheel, e.pan, &sink);
		CHECK("RISC OS: pointer right 10 and down 20, Select and Menu, wheel one notch",
		      l.dx == 10 && l.dy == -20 && l.dz == 1 &&
		      l.count == 2 && l.keys[0] == BTV_KEY_LeftMouse &&
		      l.keys[1] == BTV_KEY_CentreMouse);
	}

	printf("realistic descriptors: a Logitech Unifying-style mouse\n");
	memset(&l, 0, sizeof l);
	parse(&m, unifying_mouse, sizeof unifying_mouse, 22);
	{
		/* Buttons 1 and 2, X = -3 (12 bits: 0xffd), Y = +300 (0x12c),
		 * wheel +1, pan -1: the 12-bit pair is packed X low byte, then
		 * X high nibble with Y low nibble, then Y high byte. */
		uint8_t r[7] = { 0x03, 0x00, 0xfd, 0xcf, 0x12, 0x01, 0xff };

		CHECK("a usable pointer under report ID 2",
		      btv_hid_map_finish(&m) && m.has_pointer && btv_hid_map_wants(&m, 2) &&
		      !btv_hid_map_wants(&m, 0));
		CHECK("decodes 12-bit axes with sign: x -3, y 300",
		      btv_hid_map_decode(&m, 2, r, 7, &e) && e.buttons == 3 &&
		      e.dx == -3 && e.dy == 300 && e.wheel == 1 && e.pan == -1);
	}

	printf("realistic descriptors: an Apple-style keyboard\n");
	parse(&m, apple_keyboard, sizeof apple_keyboard, 22);
	{
		uint8_t keys[8] = { 0x08, 0, 0x2b, 0, 0, 0, 0, 0 };   /* GUI left, Tab */
		uint8_t fn[1] = { 0x01 };

		CHECK("keys are report ID 1", btv_hid_map_finish(&m) && m.has_keyboard &&
		      btv_hid_map_wants(&m, 1) && m.has_leds && m.led_id == 1);
		CHECK("the Fn key's vendor report is not taken for keys or a pointer",
		      !btv_hid_map_wants(&m, 0x11) &&
		      !(btv_hid_map_decode(&m, 0x11, fn, 1, &e)));
		CHECK("the keyboard report decodes: left GUI and Tab",
		      btv_hid_map_decode(&m, 1, keys, 8, &e) && e.modifiers == 0x08 &&
		      e.keys[0] == 0x2b);
		memset(&l, 0, sizeof l);
		btv_hid_keyboard_init(&kb);
		{
			uint8_t boot[8] = { e.modifiers, 0, e.keys[0], 0, 0, 0, 0, 0 };
			btv_hid_keyboard_report(&kb, boot, 8, &sink);
		}
		CHECK("RISC OS: the Windows key is the Acorn key, then Tab",
		      l.count == 2 && l.keys[0] == BTV_KEY_AcornLeft &&
		      l.keys[1] == BTV_KEY_Tab);
	}
}

int main(void)
{
	descriptor();
	realistic();
	keyboard();
	into_the_sink();
	odd_descriptors();
	return check_summary("hidmap");
}
