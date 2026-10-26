/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for how a report-mode mouse feels (modules/network/bluetooth/
 * c/hid_input.c): its counts scaled and accelerated gently, the fractions of
 * a pixel kept between reports, the wheel's counts made into notches with the
 * remainder kept, natural scrolling, horizontal scroll on or off, and the
 * extra buttons that start applications (not passed on as clicks).
 *
 * The kernel applies Configure > Mouse's speed (MouseStep) to every PointerV
 * report, USB and Bluetooth alike (kernel/c/io/mouse.c, os_mouse_moved), so
 * none of that is done here: these deltas are what PointerV is given.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "hid_input.h"

struct log {
	int dx, dy, dz, dw, moves;
	unsigned int extra;
	int keys[16], nkeys;
	unsigned int launches[8], nlaunch;
};

static void on_key(void *c, int down, unsigned int key)
{
	struct log *l = c;
	if (l->nkeys < 16) l->keys[l->nkeys++] = down ? (int)key : -(int)key - 1;
}
static void on_pointer(void *c, int dx, int dy)
{
	struct log *l = c;
	l->dx += dx; l->dy += dy; l->moves++;
}
static void on_wheel(void *c, int dz, int dw, unsigned int extra)
{
	struct log *l = c;
	l->dz += dz; l->dw += dw; l->extra = extra;
}
static void on_launch(void *c, unsigned int button)
{
	struct log *l = c;
	if (l->nlaunch < 8) l->launches[l->nlaunch++] = button;
}

static void arithmetic(void)
{
	int32_t rem = 0;

	printf("scaling arithmetic\n");
	CHECK("no acceleration up to 3 counts", btv_hid_accel_q8(3) == 256 && btv_hid_accel_q8(-2) == 256);
	CHECK("rising gently after that", btv_hid_accel_q8(4) == 272 && btv_hid_accel_q8(11) == 384);
	CHECK("to double at 19 counts, no more", btv_hid_accel_q8(19) == 512 && btv_hid_accel_q8(500) == 512);
	CHECK("the same for the other direction", btv_hid_accel_q8(-19) == 512);
	CHECK("1.0 gain is the count itself", btv_hid_scale(7, 256, &rem) == 7 && rem == 0);
	/* 0.4 of a pixel a report: nothing moves for two reports, then one. */
	rem = 0;
	CHECK("0.4 px: no movement, a remainder",
	      btv_hid_scale(1, 102, &rem) == 0 && rem == 102);
	CHECK("again: still none", btv_hid_scale(1, 102, &rem) == 0 && rem == 204);
	CHECK("the third report makes a pixel, the fraction goes on",
	      btv_hid_scale(1, 102, &rem) == 1 && rem == 50);
	rem = 0;
	CHECK("negative movement keeps a negative remainder, symmetric",
	      btv_hid_scale(-1, 102, &rem) == 0 && rem == -102 &&
	      btv_hid_scale(-1, 102, &rem) == 0 && btv_hid_scale(-1, 102, &rem) == -1 && rem == -50);
	rem = 0;
	CHECK("an exact half: two reports of 0.5 px make one", btv_hid_scale(1, 128, &rem) == 0 &&
	      btv_hid_scale(1, 128, &rem) == 1 && rem == 0);
	rem = 0;
	CHECK("wheel units of 120 to a notch: 30 counts is a quarter, kept",
	      btv_hid_steps(30, 120, &rem) == 0 && rem == 30);
	CHECK("four such make a notch", btv_hid_steps(30, 120, &rem) == 0 &&
	      btv_hid_steps(30, 120, &rem) == 0 && btv_hid_steps(30, 120, &rem) == 1 && rem == 0);
	rem = 0;
	CHECK("a big turn is several notches and a remainder",
	      btv_hid_steps(300, 120, &rem) == 2 && rem == 60);
	rem = 0;
	CHECK("backwards too", btv_hid_steps(-300, 120, &rem) == -2 && rem == -60);
	rem = 5;
	CHECK("unit 1 passes the count through and forgets any remainder",
	      btv_hid_steps(3, 1, &rem) == 3 && rem == 0);
}

static void feel(void)
{
	btv_hid_mouse m;
	struct log l;
	btv_hid_sink sink;

	printf("a report-mode mouse\n");
	memset(&sink, 0, sizeof sink);
	sink.context = &l; sink.key = on_key; sink.pointer = on_pointer;
	sink.wheel = on_wheel; sink.launch = on_launch;

	btv_hid_mouse_init(&m);
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 2, 3, 0, 0, &sink);
	CHECK("small movements pass as they are (no acceleration under 4 counts); y is up",
	      l.dx == 2 && l.dy == -3);
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 19, 0, 0, 0, &sink);
	CHECK("a fast movement is doubled", l.dx == 38);

	btv_hid_mouse_init(&m);
	m.opt.accel = 0;
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 40, -40, 0, 0, &sink);
	CHECK("acceleration off: counts as they come", l.dx == 40 && l.dy == 40);

	btv_hid_mouse_init(&m);
	m.opt.accel = 0;
	m.opt.scale_q8 = 128;          /* 50% for a high-resolution mouse */
	memset(&l, 0, sizeof l);
	{
		int i;
		for (i = 0; i < 10; i++)
			btv_hid_mouse_event(&m, 0, 1, 0, 0, 0, &sink);
	}
	CHECK("slow movement at 50%: ten counts make five pixels, none lost", l.dx == 5);

	btv_hid_mouse_init(&m);
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 0, 0, -1, 2, &sink);
	CHECK("wheel -1 (towards the user) and pan 2 reach the wheel call: dz +1, dw 2",
	      l.dz == 1 && l.dw == 2);
	m.opt.natural = 1;
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 0, 0, -1, 0, &sink);
	CHECK("natural scrolling reverses the wheel", l.dz == -1);
	m.opt.hscroll = 0;
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 0, 0, 0, 3, &sink);
	CHECK("horizontal scroll off: the pan is dropped", l.dw == 0);

	btv_hid_mouse_init(&m);
	m.opt.wheel_unit = 120;
	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 0, 0, 60, 0, &sink);
	btv_hid_mouse_event(&m, 0, 0, 0, 60, 0, &sink);
	CHECK("a high-resolution wheel: two halves are one notch (away: -1)", l.dz == -1);
}

static void buttons(void)
{
	btv_hid_mouse m;
	struct log l;
	btv_hid_sink sink;

	printf("extra buttons that start applications\n");
	memset(&sink, 0, sizeof sink);
	sink.context = &l; sink.key = on_key; sink.pointer = on_pointer;
	sink.wheel = on_wheel; sink.launch = on_launch;
	btv_hid_mouse_init(&m);
	m.opt.mapped = (1u << 3) | (1u << 5);       /* buttons 4 and 6 */
	memset(&l, 0, sizeof l);

	btv_hid_mouse_event(&m, 1u << 3, 0, 0, 0, 0, &sink);
	CHECK("button 4 down starts its application", l.nlaunch == 1 && l.launches[0] == 4);
	CHECK("and is not a click or an extra button", l.nkeys == 0 && l.extra == 0);
	btv_hid_mouse_event(&m, 1u << 3, 0, 0, 0, 0, &sink);
	CHECK("held, it does not start it again", l.nlaunch == 1);
	btv_hid_mouse_event(&m, 0, 0, 0, 0, 0, &sink);
	btv_hid_mouse_event(&m, 1u << 3, 0, 0, 0, 0, &sink);
	CHECK("released and pressed again, it does", l.nlaunch == 2);
	btv_hid_mouse_event(&m, 1u << 5 | 1u << 0, 0, 0, 0, 0, &sink);
	CHECK("button 6 is its own press; a left click with it is still a click",
	      l.nlaunch == 3 && l.launches[2] == 6 && l.nkeys == 1 && l.keys[0] == BTV_KEY_LeftMouse);

	memset(&l, 0, sizeof l);
	btv_hid_mouse_event(&m, 0, 0, 0, 0, 0, &sink);
	btv_hid_mouse_event(&m, 1u << 4, 0, 0, 0, 0, &sink);
	CHECK("an unmapped extra button (5) is passed on as before (bit 1 of extra)",
	      l.nlaunch == 0 && l.extra == 2);
	{
		btv_hid_sink quiet;

		memset(&quiet, 0, sizeof quiet);
		quiet.context = &l; quiet.key = on_key;
		btv_hid_mouse_init(&m);
		m.opt.mapped = 1u << 3;
		btv_hid_mouse_event(&m, 1u << 3, 0, 0, 0, 0, &quiet);
		CHECK("a sink with no launch call is safe", 1);
	}
}

int main(void)
{
	arithmetic();
	feel();
	buttons();
	return check_summary("feel");
}
