/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for BluetoothVoice's AVRCP (c/avrcp.c): the bytes it sends and
 * answers, against a fake speaker.  The frames are written out here from
 * the AVCTP, AV/C and AVRCP layouts:
 *
 *   AVCTP    label<<4 | packet type<<2 | C/R<<1 | IPID, profile id 0x110E
 *   AV/C     ctype/response, subunit (0x48 panel, 0xFF unit), opcode
 *   vendor   company id 00 19 58, PDU id, packet type, length (2), body
 *
 * None of the Bluetooth SIG's profile documents was available to this
 * project as a file, so the layouts are from knowledge of them and
 * public descriptions (BlueZ's and Android's AVRCP code were read as
 * descriptions of the PDUs, not copied); a real speaker is the test that
 * counts (docs/bluetooth-modern-20261013.md has the plan).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrcp.h"
#include "check.h"

static int same(const uint8_t *got, size_t n, const uint8_t *want, size_t m)
{
	return n == m && memcmp(got, want, n) == 0;
}

#define SAME(got, n, ...) ({ static const uint8_t w_[] = { __VA_ARGS__ }; \
			     same((got), (n), w_, sizeof w_); })

static size_t reply_frame(uint8_t *f, uint8_t label, uint8_t code, uint8_t pdu,
			  const uint8_t *body, size_t n)
{
	size_t k = 0;

	f[k++] = (uint8_t)(label << 4 | 0x02);
	f[k++] = 0x11; f[k++] = 0x0e;
	f[k++] = code; f[k++] = 0x48; f[k++] = 0x00;
	f[k++] = 0x00; f[k++] = 0x19; f[k++] = 0x58;
	f[k++] = pdu; f[k++] = 0x00;
	f[k++] = (uint8_t)(n >> 8); f[k++] = (uint8_t)n;
	memcpy(f + k, body, n);
	return k + n;
}

int main(void)
{
	btv_avrcp a;
	uint8_t out[64], in[64], reply[64];
	size_t n, m;
	uint32_t t = 1000;

	/* ---- as the controller: capabilities, registration, volume ---- */
	btv_avrcp_init(&a);
	n = btv_avrcp_poll(&a, t, out, sizeof out);
	CHECK("nothing is sent before the channel is open", n == 0);
	btv_avrcp_open(&a, t);
	n = btv_avrcp_poll(&a, t, out, sizeof out);
	CHECK("opened: GetCapabilities (STATUS, panel, vendor dependent, events)",
	      SAME(out, n, 0x10, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58,
		   0x10, 0x00, 0x00, 0x01, 0x03));
	CHECK("only one command is outstanding at a time",
	      btv_avrcp_poll(&a, t + 1, out, sizeof out) == 0);
	{
		static const uint8_t caps[] = { 0x03, 0x02, 0x01, 0x0d };

		m = reply_frame(in, 1, 0x0c, 0x10, caps, sizeof caps);
		CHECK("a response needs no reply",
		      btv_avrcp_receive(&a, in, m, t + 5, reply, sizeof reply) == 0);
	}
	n = btv_avrcp_poll(&a, t + 6, out, sizeof out);
	CHECK("the speaker has EVENT_VOLUME_CHANGED: RegisterNotification (NOTIFY)",
	      SAME(out, n, 0x20, 0x11, 0x0e, 0x03, 0x48, 0x00, 0x00, 0x19, 0x58,
		   0x31, 0x00, 0x00, 0x05, 0x0d, 0x00, 0x00, 0x00, 0x00));
	{
		static const uint8_t interim[] = { 0x0d, 0x40 };

		m = reply_frame(in, 2, 0x0f, 0x31, interim, sizeof interim);
		(void)btv_avrcp_receive(&a, in, m, t + 10, reply, sizeof reply);
	}
	CHECK("the interim response makes absolute volume active", btv_avrcp_absolute(&a));
	CHECK("the speaker's volume is 64", btv_avrcp_speaker_volume(&a) == 64);
	CHECK("the host had no volume yet, so it is told to take the speaker's",
	      a.generation == 1 && a.want_valid && a.want_volume == 64);
	CHECK("nothing to send", btv_avrcp_poll(&a, t + 11, out, sizeof out) == 0);

	btv_avrcp_set_volume(&a, 100);
	n = btv_avrcp_poll(&a, t + 20, out, sizeof out);
	CHECK("the host's volume goes out as SetAbsoluteVolume (CONTROL, 100)",
	      SAME(out, n, 0x30, 0x11, 0x0e, 0x00, 0x48, 0x00, 0x00, 0x19, 0x58,
		   0x50, 0x00, 0x00, 0x01, 0x64));
	{
		static const uint8_t ok[] = { 0x64 };

		m = reply_frame(in, 3, 0x09, 0x50, ok, sizeof ok);
		(void)btv_avrcp_receive(&a, in, m, t + 25, reply, sizeof reply);
	}
	CHECK("accepted: the speaker is at 100 and the host is not told to follow",
	      btv_avrcp_speaker_volume(&a) == 100 && a.generation == 1);

	btv_avrcp_set_volume(&a, 100);
	CHECK("the same volume again sends nothing",
	      btv_avrcp_poll(&a, t + 30, out, sizeof out) == 0);

	/* The speaker's buttons: a CHANGED notification, unprompted. */
	{
		static const uint8_t changed[] = { 0x0d, 0x20 };

		m = reply_frame(in, 2, 0x0d, 0x31, changed, sizeof changed);
		(void)btv_avrcp_receive(&a, in, m, t + 40, reply, sizeof reply);
	}
	CHECK("a CHANGED notification: the speaker is at 32 and the host follows",
	      btv_avrcp_speaker_volume(&a) == 32 && a.generation == 2 &&
	      a.want_volume == 32);
	n = btv_avrcp_poll(&a, t + 41, out, sizeof out);
	CHECK("and a notification is registered again (it is used once)",
	      n == 18 && out[9] == 0x31 && out[13] == 0x0d && out[3] == 0x03);
	{
		static const uint8_t interim[] = { 0x0d, 0x20 };

		m = reply_frame(in, (uint8_t)(out[0] >> 4), 0x0f, 0x31, interim, sizeof interim);
		(void)btv_avrcp_receive(&a, in, m, t + 45, reply, sizeof reply);
	}
	CHECK("its interim answer does not send the old volume back",
	      btv_avrcp_poll(&a, t + 46, out, sizeof out) == 0 &&
	      btv_avrcp_speaker_volume(&a) == 32);

	/* A volume the host sets while a command is out waits its turn. */
	btv_avrcp_set_volume(&a, 10);
	n = btv_avrcp_poll(&a, t + 50, out, sizeof out);
	CHECK("another host volume: sent", n == 14 && out[13] == 10);
	btv_avrcp_set_volume(&a, 11);
	CHECK("a newer one waits for the answer",
	      btv_avrcp_poll(&a, t + 51, out, sizeof out) == 0);
	{
		static const uint8_t ok[] = { 10 };

		m = reply_frame(in, (uint8_t)(a.outstanding_label), 0x09, 0x50, ok, 1);
		(void)btv_avrcp_receive(&a, in, m, t + 55, reply, sizeof reply);
	}
	n = btv_avrcp_poll(&a, t + 56, out, sizeof out);
	CHECK("then it goes", n == 14 && out[13] == 11);
	{
		static const uint8_t no[] = { 0x06 };

		m = reply_frame(in, (uint8_t)(a.outstanding_label), 0x0a, 0x50, no, 1);
		(void)btv_avrcp_receive(&a, in, m, t + 58, reply, sizeof reply);
	}
	CHECK("a rejection leaves the speaker's volume unknown",
	      btv_avrcp_speaker_volume(&a) == -1);

	btv_avrcp_close(&a);
	CHECK("closed: idle, the host's volume kept",
	      a.state == BTV_AVRCP_IDLE && a.want_valid);

	/* ---- a speaker without absolute volume ---- */
	btv_avrcp_init(&a);
	btv_avrcp_open(&a, t);
	(void)btv_avrcp_poll(&a, t, out, sizeof out);
	{
		static const uint8_t caps[] = { 0x03, 0x01, 0x01 };

		m = reply_frame(in, 1, 0x0c, 0x10, caps, sizeof caps);
		(void)btv_avrcp_receive(&a, in, m, t + 5, reply, sizeof reply);
	}
	CHECK("events without volume: pass-through only",
	      a.state == BTV_AVRCP_PASSTHROUGH && !btv_avrcp_absolute(&a));
	CHECK("and nothing is registered", btv_avrcp_poll(&a, t + 6, out, sizeof out) == 0);

	/* ---- a speaker that never answers ---- */
	btv_avrcp_init(&a);
	btv_avrcp_open(&a, t);
	(void)btv_avrcp_poll(&a, t, out, sizeof out);
	CHECK("an unanswered command is waited for",
	      btv_avrcp_poll(&a, t + 3000, out, sizeof out) == 0 &&
	      a.state == BTV_AVRCP_CAPABILITIES);
	CHECK("then given up on after 4 s", btv_avrcp_poll(&a, t + 4100, out, sizeof out) == 0 &&
	      a.state == BTV_AVRCP_PASSTHROUGH);

	/* ---- as the target: the speaker's questions and buttons ---- */
	btv_avrcp_init(&a);
	{
		static const uint8_t unit_info[] =
			{ 0x50, 0x11, 0x0e, 0x01, 0xff, 0x30, 0xff, 0xff, 0xff, 0xff, 0xff };

		n = btv_avrcp_receive(&a, unit_info, sizeof unit_info, t, reply, sizeof reply);
		CHECK("UNIT INFO: STABLE, panel unit, company id",
		      SAME(reply, n, 0x52, 0x11, 0x0e, 0x0c, 0xff, 0x30, 0x07, 0x48,
			   0x00, 0x19, 0x58));
	}
	{
		static const uint8_t subunit_info[] =
			{ 0x60, 0x11, 0x0e, 0x01, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff };

		n = btv_avrcp_receive(&a, subunit_info, sizeof subunit_info, t, reply, sizeof reply);
		CHECK("SUBUNIT INFO: STABLE, one panel subunit",
		      SAME(reply, n, 0x62, 0x11, 0x0e, 0x0c, 0xff, 0x31, 0x07, 0x48,
			   0xff, 0xff, 0xff));
	}
	{
		static const uint8_t up_press[] =
			{ 0x70, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x41, 0x00 };
		static const uint8_t up_release[] =
			{ 0x80, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0xc1, 0x00 };
		static const uint8_t down_press[] =
			{ 0x90, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x42, 0x00 };
		static const uint8_t mute_press[] =
			{ 0xa0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x43, 0x00 };
		static const uint8_t play_press[] =
			{ 0xb0, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x44, 0x00 };

		n = btv_avrcp_receive(&a, up_press, sizeof up_press, t, reply, sizeof reply);
		CHECK("VOLUME UP pressed: ACCEPTED, the same operands",
		      SAME(reply, n, 0x72, 0x11, 0x0e, 0x09, 0x48, 0x7c, 0x41, 0x00));
		(void)btv_avrcp_receive(&a, up_release, sizeof up_release, t, reply, sizeof reply);
		CHECK("one step up (the release does not count)", btv_avrcp_take_steps(&a) == 1);
		CHECK("taken, so none left", btv_avrcp_take_steps(&a) == 0);
		(void)btv_avrcp_receive(&a, down_press, sizeof down_press, t, reply, sizeof reply);
		(void)btv_avrcp_receive(&a, down_press, sizeof down_press, t, reply, sizeof reply);
		CHECK("two presses of VOLUME DOWN: two steps down", btv_avrcp_take_steps(&a) == -2);
		(void)btv_avrcp_receive(&a, mute_press, sizeof mute_press, t, reply, sizeof reply);
		CHECK("MUTE counted", btv_avrcp_take_mutes(&a) == 1);
		n = btv_avrcp_receive(&a, play_press, sizeof play_press, t, reply, sizeof reply);
		CHECK("PLAY: NOT IMPLEMENTED (nothing here plays)",
		      SAME(reply, n, 0xb2, 0x11, 0x0e, 0x08, 0x48, 0x7c, 0x44, 0x00));
	}
	{
		static const uint8_t company_caps[] =
			{ 0xc0, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00,
			  0x00, 0x01, 0x02 };

		n = btv_avrcp_receive(&a, company_caps, sizeof company_caps, t, reply, sizeof reply);
		CHECK("GetCapabilities(CompanyID): the SIG's identifier",
		      SAME(reply, n, 0xc2, 0x11, 0x0e, 0x0c, 0x48, 0x00, 0x00, 0x19, 0x58,
			   0x10, 0x00, 0x00, 0x05, 0x02, 0x01, 0x00, 0x19, 0x58));
	}
	{
		static const uint8_t meta[] =
			{ 0xd0, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x20, 0x00,
			  0x00, 0x01, 0x00 };

		n = btv_avrcp_receive(&a, meta, sizeof meta, t, reply, sizeof reply);
		CHECK("metadata queries are NOT IMPLEMENTED", n == sizeof meta && reply[3] == 0x08 &&
		      reply[0] == 0xd2);
	}
	{
		static const uint8_t other[] = { 0xe0, 0x11, 0x0f, 0x00, 0x00 };

		n = btv_avrcp_receive(&a, other, sizeof other, t, reply, sizeof reply);
		CHECK("a command for another profile: the invalid profile reply (IPID)",
		      SAME(reply, n, 0xe3, 0x11, 0x0f));
	}
	{
		static const uint8_t fragment[] = { 0xf4, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x41, 0x00 };

		CHECK("a fragment is ignored",
		      btv_avrcp_receive(&a, fragment, sizeof fragment, t, reply, sizeof reply) == 0);
	}
	CHECK("short garbage is ignored",
	      btv_avrcp_receive(&a, in, 2, t, reply, sizeof reply) == 0);
	return check_summary("avrcp");
}
