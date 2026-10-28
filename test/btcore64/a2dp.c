/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for BluetoothVoice's A2DP source: the core, L2CAP, AVDTP and
 * the SBC encoder (the real c/bt_core.c, bt_l2cap.c, bt_a2dp.c and
 * sbc_encoder.c) against a fake speaker that plays the sink's part of the
 * protocol over fake ACL packets, from the radio up:
 *
 *   connect, authenticate, encrypt; AVDTP signalling channel; Discover,
 *   Get Capabilities, Set Configuration, Open; the media channel; Start;
 *   RTP packets of SBC frames at the rate the audio needs.
 *
 * The expected bytes are AVDTP's (Bluetooth AVDTP 1.3 section 8) and
 * A2DP's (A2DP 1.3 section 4), written out here, and the speaker counts
 * what arrives: a media packet is an RTP header (12 bytes: version 2,
 * payload type 96), the SBC payload header (frame count) and frames that
 * each start with the SBC syncword 0x9C.
 *
 * It also plays the other way: a paired speaker that connects to us, opens
 * AVRCP's channel and not AVDTP's, so that the link must be kept (the
 * fault on the Pi was a loop of connect, authenticate, disconnect).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_core.h"
#include "bt_profiles.h"
#include "check.h"
#include "hid_riscos.h"
#include "hidtest.h"
#include "sound_ring.h"
#include "bt_info.h"


/* ---------------------------------------------------------- the radio */

#define HANDLE 0x0042u
static const uint8_t PEER[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static struct pkt { uint8_t b[600]; unsigned int n; } evq[128], aclq[128];
static unsigned int evh, evt, ah, at;
static uint8_t tickets[65536];
static uint32_t next_ticket;
static uint32_t clock_ms = 1000;

static void push_event(const uint8_t *e, unsigned int n)
{
	memcpy(evq[evt % 128].b, e, n);
	evq[evt++ % 128].n = n;
}

static void push_acl(uint16_t cid, const uint8_t *data, unsigned int n)
{
	struct pkt *p = &aclq[at++ % 128];

	p->b[0] = HANDLE & 0xff; p->b[1] = 0x20 | (HANDLE >> 8);   /* start, no broadcast */
	p->b[2] = (uint8_t)(n + 4); p->b[3] = 0;
	p->b[4] = (uint8_t)n; p->b[5] = (uint8_t)(n >> 8);
	p->b[6] = (uint8_t)cid; p->b[7] = (uint8_t)(cid >> 8);
	memcpy(p->b + 8, data, n);
	p->n = n + 8;
}

static int hold_auth;           /* the person is still typing the passkey */

static void answer(const uint8_t *c)
{
	uint16_t op = (uint16_t)(c[0] | c[1] << 8);
	uint8_t e[16];

	switch (op) {
	case 0x0401: case 0x0405: case 0x0406: case 0x0411: case 0x0413:
	case 0x0419:
		e[0] = 0x0f; e[1] = 4; e[2] = 0; e[3] = 1; e[4] = c[0]; e[5] = c[1];
		push_event(e, 6);
		break;
	default:
		e[0] = 0x0e; e[1] = 4; e[2] = 1; e[3] = c[0]; e[4] = c[1]; e[5] = 0;
		push_event(e, 6);
	}
	if (op == 0x0405) {
		/* Connection Complete: the speaker accepts. */
		e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
		memcpy(e + 5, PEER, 6); e[11] = 1; e[12] = 0;
		push_event(e, 13);
	} else if (op == 0x0411) {
		if (hold_auth)
			return;
		e[0] = 0x06; e[1] = 3; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
		push_event(e, 5);
	} else if (op == 0x0413) {
		e[0] = 0x08; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
		e[5] = 1;
		push_event(e, 6);
	}
}

static unsigned int disconnects, auths, acl_out, held, hold_completions,
	exit_sniff, switch_role;
static int accept_role = -1;

int btv_host_bridge_open(uint32_t *w) { *w = 0; return 0; }
int btv_host_bridge_boot(void) { return 1; }
int btv_host_bridge_close(uint32_t *w) { *w = 0; return 0; }
void btv_host_bridge_note(const char *a, const char *b) { (void)a; (void)b; }

static void sink_receive(const uint8_t *acl, unsigned int n);

int btv_host_bridge_submit(unsigned int kind, const uint8_t *d, uint32_t n,
                           uint32_t p, uint32_t *t, uint32_t *w)
{
	(void)p;
	*w = 0;
	*t = ++next_ticket;
	tickets[*t % 65536] = (uint8_t)kind;
	if (kind == BTV_HOST_BRIDGE_COMMAND) {
		uint16_t op = (uint16_t)(d[0] | d[1] << 8);

		if (op == 0x0406) disconnects++;
		if (op == 0x0411) auths++;
		if (op == 0x0409) accept_role = d[9];
		if (op == 0x0804) exit_sniff++;
		if (op == 0x080b) switch_role++;
		answer(d);
	} else if (kind == BTV_HOST_BRIDGE_ACL_OUT) {
		uint8_t e[8];

		acl_out++;
		sink_receive(d, n);
		/* Number Of Completed Packets: one for this handle (or held,
		 * while the radio is pretended to be stalled). */
		if (hold_completions)
			held++;
		else {
			e[0] = 0x13; e[1] = 5; e[2] = 1; e[3] = HANDLE & 0xff;
			e[4] = HANDLE >> 8; e[5] = 1; e[6] = 0;
			push_event(e, 7);
		}
	}
	return 0;
}

int btv_host_bridge_poll(uint32_t t, btv_host_bridge_completion *c)
{
	unsigned int k = tickets[t % 65536];

	memset(c, 0, offsetof(btv_host_bridge_completion, data));
	c->kind = k;
	if (k == BTV_HOST_BRIDGE_COMMAND || k == BTV_HOST_BRIDGE_ACL_OUT)
		return 0;
	if (k == BTV_HOST_BRIDGE_EVENT_IN && evh != evt) {
		struct pkt *p = &evq[evh++ % 128];

		memcpy(c->data, p->b, p->n);
		c->bytes = p->n;
		return 0;
	}
	if (k == BTV_HOST_BRIDGE_ACL_IN && ah != at) {
		struct pkt *p = &aclq[ah++ % 128];

		memcpy(c->data, p->b, p->n);
		c->bytes = p->n;
		return 0;
	}
	return BTV_HOST_BRIDGE_PENDING;
}

int btv_host_bridge_cancel(uint32_t t, uint32_t *w) { (void)t; *w = 0; return 0; }

/* ------------------------------------------------- the profile glue */

/* The real bt_profiles.c (the dispatcher, AVRCP) and bt_sdp_server.c are
 * linked; what it hands to HID and Low Energy, which want RISC OS, is
 * stubbed here. */
void btv_hid_riscos_init(btv_hid_riscos *r) { memset(r, 0, sizeof *r); }
unsigned int btv_hid_riscos_poll(btv_hid_riscos *r, uint32_t *f)
{ (void)r; *f = 0; return 0; }
/* RISC OS's side of HID, recorded. */
static unsigned int kb_attach, kb_detach, mouse_attach_n, pad_events, pad_detach_n;
static unsigned int kbd_reports, mouse_events;
static uint8_t last_kbd[8];
static btv_hid_extra_event last_pad;
static int prefer_boot;
extern btv_hid_riscos btp_hid_riscos;
int btv_hid_riscos_keyboard_attach(btv_hid_riscos *r) { (void)r; kb_attach++; return 1; }
void btv_hid_riscos_keyboard_detach(btv_hid_riscos *r, btv_hid_keyboard *k)
{ (void)r; (void)k; kb_detach++; }
void btv_hid_riscos_keyboard_report(btv_hid_riscos *r, btv_hid_keyboard *k,
                                    const uint8_t *rep, size_t n)
{ (void)r; (void)k; if (n >= 8) memcpy(last_kbd, rep, 8); kbd_reports++; }
void btv_hid_riscos_mouse_attach(btv_hid_riscos *r) { (void)r; mouse_attach_n++; }
void btv_hid_riscos_mouse_detach(btv_hid_riscos *r, btv_hid_mouse *m) { (void)r; (void)m; }
void btv_hid_riscos_mouse_report(btv_hid_riscos *r, btv_hid_mouse *m, const uint8_t *p, size_t n)
{ (void)r; (void)m; (void)p; (void)n; mouse_events++; }
void btv_hid_riscos_mouse_event(btv_hid_riscos *r, btv_hid_mouse *m, uint32_t b,
                                int32_t dx, int32_t dy, int32_t w, int32_t pan)
{ (void)r; (void)m; (void)b; (void)dx; (void)dy; (void)w; (void)pan; mouse_events++; }
void btv_hid_riscos_pad_event(btv_hid_riscos *r, btv_hid_pad *pad, const btv_hid_extra_event *e)
{ (void)r; (void)pad; last_pad = *e; pad_events++; }
void btv_hid_riscos_pad_detach(btv_hid_riscos *r, btv_hid_pad *pad)
{ (void)r; (void)pad; pad_detach_n++; }
int btv_hid_riscos_prefer_boot(void) { return prefer_boot; }
void btp_le_init(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_le_link_opened(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_le_link_closed(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_le_secured(btc_core *c, uint8_t l) { (void)c; (void)l; }
void btp_le_pump(btc_core *c) { (void)c; }
int btp_le_busy(btc_core *c) { (void)c; return 0; }
void btp_le_advert(btc_core *c, const uint8_t *a, uint8_t t) { (void)c; (void)a; (void)t; }
void btp_le_data(btc_core *c, uint8_t l, uint16_t id, const uint8_t *p, size_t n)
{ (void)c; (void)l; (void)id; (void)p; (void)n; }
void btp_le_encryption(btc_core *c, uint8_t l, uint8_t s) { (void)c; (void)l; (void)s; }
void btp_le_rand(btc_core *c, uint8_t l, uint8_t s, const uint8_t *r, size_t n)
{ (void)c; (void)l; (void)s; (void)r; (void)n; }

/* The sound device: the real queue between SoundDMA and the media clock,
 * filled by a producer as SoundDMA's ticker would, from the moment the
 * speakers are registered (which is before the stream starts). */
static btv_sound_ring ring;
static unsigned int sound_on, producing = 1, silent;
static unsigned int state_suspend, reject_suspend;
static int16_t tone[4096 * 2];
int btv_sound_device_stereo_sync(int a, uint32_t r, uint32_t n)
{
	(void)r; (void)n;
	if (a && !sound_on)
		btv_sound_ring_reset(&ring);
	sound_on = a != 0;
	return sound_on;
}
unsigned int btv_sound_device_stereo_read(int16_t *o, unsigned int f)
{
	unsigned int got;

	if (!sound_on)
		return 0;
	got = btv_sound_ring_read(&ring, o, f);
	return got;
}
void btv_sound_device_flush(void) { btv_sound_ring_flush(&ring); }
int btv_sound_device_stereo_audible(void)
{ return sound_on && btv_sound_ring_audible(&ring, 2); }
void btv_sound_device_counts(uint32_t *r, uint32_t *d, uint32_t *q)
{ *r = ring.refused; *d = ring.dropped; *q = btv_sound_ring_used(&ring); }
void btv_host_bridge_errors(uint32_t o[5]) { o[0]=o[1]=o[2]=o[3]=o[4]=0; }
void btv_host_bridge_stats(uint32_t *t, uint32_t *r, uint32_t *l)
{ *t = 0; *r = 0; *l = 0; }
uint32_t btv_sound_device_state(void) { return 0; }
uint32_t btv_sound_device_meter(void) { return 0; }
int btv_sound_device_running(void) { return 0; }
int btv_sound_device_registered(void) { return 0; }
void btv_sound_device_orphan(void) { }
void btv_sound_device_sync(void *p, int a) { (void)p; (void)a; }

/* 441 frames a 10 ms, as the real time makes them (44.1 kHz). */
static void produce(void)
{
	unsigned int i;
	static unsigned int ph;

	if (!sound_on || !producing)
		return;
	for (i = 0; i < 441; i++, ph++) {
		int16_t v = silent ? 0 : (int16_t)(((ph / 50) & 1 ? 8000 : -8000));
		tone[2 * i] = v; tone[2 * i + 1] = (int16_t)-v;
	}
	(void)btv_sound_ring_write(&ring, tone, 441);
}

/* ---------------------------------------------------- the speaker */

struct chan { uint16_t local, remote, psm; int open; };
static struct chan chans[8];
static unsigned int nchans;
static uint16_t next_cid = 0x0100;
static unsigned int state_discover, state_caps, state_config, state_open,
	state_start, media_packets, sbc_frames, bad_packets, media_cid_n;
static uint8_t configured[4];
static int stream_media_cid = -1;
static uint8_t sink_ident = 0x40;
static uint16_t peer_core_cid;
static unsigned int peer_answered;

static void sig(uint8_t code, uint8_t ident, const uint8_t *d, unsigned int n)
{
	uint8_t b[64];

	b[0] = code; b[1] = ident; b[2] = (uint8_t)n; b[3] = 0;
	memcpy(b + 4, d, n);
	push_acl(1, b, n + 4);
}

static void avdtp(uint8_t cid_local, uint8_t label, uint8_t id, const uint8_t *p,
                  unsigned int n)
{
	uint8_t r[32];
	unsigned int len = 0, i;
	int accept = !(id == 0x09 && reject_suspend);

	(void)p; (void)n;
	switch (id) {
	case 0x01:
		state_discover++;
		r[len++] = 1 << 2; r[len++] = 0x08;   /* SEID 1, audio sink */
		break;
	case 0x02: case 0x0c:
		state_caps++;
		r[len++] = 0x01; r[len++] = 0x00;
		r[len++] = 0x07; r[len++] = 0x06; r[len++] = 0x00; r[len++] = 0x00;
		r[len++] = 0x3f; r[len++] = 0xff; r[len++] = 2; r[len++] = 53;
		break;
	case 0x03:
		state_config++;
		/* Acp SEID, Int SEID, then the capabilities: find the codec. */
		for (i = 2; i + 2 <= n; i += 2u + p[i + 1])
			if (p[i] == 0x07 && p[i + 1] >= 6)
				memcpy(configured, p + i + 4, 4);
		break;
	case 0x06: state_open++; break;
	case 0x09: state_suspend++; break;
	case 0x07: state_start++; break;
	default: break;
	}
	r[len] = 0;
	{
		uint8_t out[40];

		out[0] = (uint8_t)(label << 4 | (accept ? 2 : 3));
		out[1] = id;
		memcpy(out + 2, r, len);
		push_acl(cid_local, out, len + 2);
	}
}

/* The speaker's AVRCP: what a speaker with absolute volume does. */
static unsigned int avrcp_caps_seen, avrcp_register_seen, avrcp_volume_sets;
static int avrcp_volume_events = 1;
static uint8_t avrcp_volume = 0x40, avrcp_register_label, avrcp_last_set;
static int avrcp_registered;
static int avrcp_chan = -1;

static void avrcp_frame(unsigned int k, uint8_t label, uint8_t code,
			uint8_t pdu, const uint8_t *body, unsigned int n)
{
	uint8_t f[40];
	unsigned int len = 0;

	f[len++] = (uint8_t)(label << 4 | 0x02);
	f[len++] = 0x11; f[len++] = 0x0e;
	f[len++] = code; f[len++] = 0x48; f[len++] = 0x00;
	f[len++] = 0x00; f[len++] = 0x19; f[len++] = 0x58;
	f[len++] = pdu; f[len++] = 0x00;
	f[len++] = 0x00; f[len++] = (uint8_t)n;
	memcpy(f + len, body, n);
	push_acl(chans[k].remote, f, len + n);
}

static void avrcp_sink(unsigned int k, const uint8_t *p, unsigned int len)
{
	uint8_t label = (uint8_t)(p[0] >> 4), body[8];

	avrcp_chan = (int)k;
	if (len < 14 || (p[0] & 2) != 0 || p[5] != 0x00)
		return;                       /* a response, or not vendor dependent */
	switch (p[9]) {
	case 0x10:
		avrcp_caps_seen++;
		body[0] = 0x03;
		body[1] = avrcp_volume_events ? 2 : 1;
		body[2] = 0x01; body[3] = 0x0d;
		avrcp_frame(k, label, 0x0c, 0x10, body, 2u + body[1]);
		break;
	case 0x31:
		avrcp_register_seen++;
		avrcp_registered = 1;
		avrcp_register_label = label;
		body[0] = 0x0d; body[1] = avrcp_volume;
		avrcp_frame(k, label, 0x0f, 0x31, body, 2);
		break;
	case 0x50:
		avrcp_volume_sets++;
		avrcp_last_set = avrcp_volume = (uint8_t)(p[13] & 0x7f);
		body[0] = avrcp_volume;
		avrcp_frame(k, label, 0x09, 0x50, body, 1);
		break;
	default:
		break;
	}
}

/* The speaker's own buttons: a volume change notification. */
static void avrcp_speaker_volume(uint8_t volume)
{
	uint8_t body[2];

	avrcp_volume = volume;
	if (avrcp_chan < 0 || !avrcp_registered)
		return;
	body[0] = 0x0d; body[1] = volume;
	avrcp_registered = 0;                 /* used once */
	avrcp_frame((unsigned int)avrcp_chan, avrcp_register_label, 0x0d, 0x31, body, 2);
}

/* The speaker's pass-through key (an AV/C PASS THROUGH command). */
static void avrcp_key(uint8_t operation_and_state)
{
	uint8_t f[8] = { 0x70, 0x11, 0x0e, 0x00, 0x48, 0x7c, operation_and_state, 0x00 };

	if (avrcp_chan >= 0)
		push_acl(chans[avrcp_chan].remote, f, sizeof f);
}

/* A HID device: its SDP server (the record in pieces of 60 octets), and
 * its control channel (HANDSHAKE to SET_PROTOCOL). */
static unsigned int sdp_requests, sdp_mute, set_protocol_seen;
static uint8_t protocol_byte, handshake_result;

static void sdp_fake(unsigned int k, const uint8_t *p, unsigned int len)
{
	uint8_t resp[160];
	size_t n;

	(void)len;
	if (p[0] != 0x06 || sdp_mute)
		return;
	sdp_requests++;
	n = sdp_server_reply(p, 60, resp);
	push_acl(chans[k].remote, resp, (unsigned int)n);
}

static void hid_control_fake(unsigned int k, const uint8_t *p, unsigned int len)
{
	uint8_t handshake = handshake_result;

	(void)len;
	if ((p[0] & 0xf0) == 0x70) {
		set_protocol_seen++;
		protocol_byte = p[0];
		push_acl(chans[k].remote, &handshake, 1);
	}
}

static int chan_by_psm(uint16_t psm)
{
	unsigned int k;

	for (k = 0; k < nchans; k++)
		if (chans[k].psm == psm)
			return (int)k;
	return -1;
}

static void sink_receive(const uint8_t *acl, unsigned int n)
{
	const uint8_t *l2 = acl + 4;
	unsigned int len = l2[0] | l2[1] << 8, cid = l2[2] | l2[3] << 8, k;
	const uint8_t *p = l2 + 4;

	(void)n;
	if (cid == 1) {
		unsigned int off = 0;

		while (off + 4 <= len) {
			uint8_t code = p[off], ident = p[off + 1];
			unsigned int clen = p[off + 2] | p[off + 3] << 8;
			const uint8_t *d = p + off + 4;
			uint8_t r[16];

			if (code == 0x02) {          /* Connection Request */
				uint16_t psm = (uint16_t)(d[0] | d[1] << 8);
				uint16_t scid = (uint16_t)(d[2] | d[3] << 8);
				struct chan *c = &chans[nchans++];
				c->psm = psm;
				c->local = next_cid++; c->remote = scid;
				r[0] = (uint8_t)c->local; r[1] = (uint8_t)(c->local >> 8);
				r[2] = (uint8_t)scid; r[3] = (uint8_t)(scid >> 8);
				r[4] = r[5] = r[6] = r[7] = 0;
				sig(0x03, ident, r, 8);
				/* Our Configuration Request. */
				r[0] = (uint8_t)scid; r[1] = (uint8_t)(scid >> 8);
				r[2] = r[3] = 0;
				sig(0x04, sink_ident++, r, 4);
			} else if (code == 0x03) {   /* Connection Response to ours */
				peer_core_cid = (uint16_t)(d[0] | d[1] << 8);
				peer_answered++;
			} else if (code == 0x04) {   /* Configuration Request */
				uint16_t dcid = (uint16_t)(d[0] | d[1] << 8);
				int found = 0;
				for (k = 0; k < nchans; k++)
					if (chans[k].local == dcid) {
						r[0] = (uint8_t)chans[k].remote;
						r[1] = (uint8_t)(chans[k].remote >> 8);
						r[2] = r[3] = r[4] = r[5] = 0;
						sig(0x05, ident, r, 6);
						found = 1;
					}
				if (!found) {        /* a channel the speaker opened */
					r[0] = (uint8_t)peer_core_cid;
					r[1] = (uint8_t)(peer_core_cid >> 8);
					r[2] = r[3] = r[4] = r[5] = 0;
					sig(0x05, ident, r, 6);
				}
			} else if (code == 0x06) {   /* Disconnection Request */
				sig(0x07, ident, d, 4);
			}
			off += 4u + clen;
		}
		return;
	}
	for (k = 0; k < nchans; k++) {
		if (chans[k].local != cid) continue;
		if (stream_media_cid == (int)cid || (len >= 13 && p[0] == 0x80)) {
			/* Media: RTP, then the SBC payload header and frames. */
			if (len < 14 || p[0] != 0x80 || (p[1] & 0x7f) != 0x60 ||
			    (p[12] & 0x0f) == 0 || p[13] != 0x9c)
				bad_packets++;
			else {
				media_packets++;
				sbc_frames += p[12] & 0x0f;
			}
			stream_media_cid = (int)cid;
			media_cid_n++;
			return;
		}
		if (chans[k].psm == 0x0017) {
			avrcp_sink(k, p, len);
			return;
		}
		if (chans[k].psm == 0x0001) {
			sdp_fake(k, p, len);
			return;
		}
		if (chans[k].psm == 0x0011) {
			hid_control_fake(k, p, len);
			return;
		}
		if (chans[k].psm == 0x0013)
			return;
		/* Signalling. */
		avdtp((uint8_t)chans[k].remote, (uint8_t)(p[0] >> 4), (uint8_t)(p[1] & 0x3f),
		      p + 2, len - 2);
		return;
	}
}

/* ----------------------------------------------------------- the tests */

static btc_core core;
static btc_profiles profiles;
static btv_host_bridge_completion completion;

static void pump(unsigned int times)
{
	while (times--) {
		clock_ms += 10;
		produce();
		btc_pump(&core, clock_ms);
	}
}

/* A paired speaker that connects to us. It authenticates and encrypts as
 * the master, opens AVRCP's channel (PSM 0x17) and not AVDTP's. The fault
 * on the Pi was a loop: we authenticated at once too (HCI error 0x23, a
 * collision), then dropped the link. Now the link must stay, the AVRCP
 * channel be accepted, and AVDTP be opened by us when the speaker does not. */
static void reconnect(void)
{
	uint8_t e[16], c[8];
	unsigned int i;

	printf("a speaker that reconnects\n");
	memset(&core, 0, sizeof core);
	memset(&profiles, 0, sizeof profiles);
	evh = evt = ah = at = 0;
	memset(chans, 0, sizeof chans);
	nchans = 0; next_cid = 0x0100; stream_media_cid = -1;
	state_discover = state_caps = state_config = state_open = state_start = 0;
	media_packets = sbc_frames = bad_packets = 0;
	disconnects = auths = 0;
	avrcp_chan = -1; avrcp_registered = 0; avrcp_caps_seen = avrcp_register_seen = 0;
	avrcp_volume_sets = 0; avrcp_volume = 0x40;
	btc_start(&core, &profiles, &completion, clock_ms);
	core.state = BTC_READY;
	core.credits = 1;
	core.acl_mtu = 1021;
	core.acl_max = core.acl_free = 8;
	btp_init(&core);
	btc_key_store(&core, PEER, 0)->flags = BTC_DEV_AUDIO_SINK;

	e[0] = 0x04; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x14; e[9] = 0x04; e[10] = 0x24; e[11] = 0x01;
	push_event(e, 12);
	pump(4);
	e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	memcpy(e + 5, PEER, 6); e[11] = 1; e[12] = 0;
	push_event(e, 13);
	pump(4);
	/* The speaker authenticates, using the key we stored. */
	e[0] = 0x17; e[1] = 6; memcpy(e + 2, PEER, 6);
	push_event(e, 8);
	pump(4);
	e[0] = 0x06; e[1] = 3; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	push_event(e, 5);
	pump(4);
	e[0] = 0x08; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	e[5] = 1;
	push_event(e, 6);
	pump(10);
	CHECK("we did not start authentication ourselves", auths == 0);
	CHECK("a speaker's connection is accepted as its master (role 0)",
	      accept_role == 0);
	/* A speaker that puts the link into sniff: we take it out. */
	e[0] = 0x14; e[1] = 6; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	e[5] = 2; e[6] = 0x40; e[7] = 0x00;
	push_event(e, 8);
	pump(6);
	CHECK("a sniffing link is made active again (Exit Sniff Mode)",
	      exit_sniff == 1);

	/* The speaker opens AVRCP: Connection Request, PSM 0x17, SCID 0x40. */
	c[0] = 0x17; c[1] = 0; c[2] = 0x40; c[3] = 0;
	sig(0x02, 0x21, c, 4);
	pump(20);
	chans[nchans].local = 0x40; chans[nchans].remote = peer_core_cid;
	chans[nchans++].psm = 0x0017;
	/* ... and, as a speaker does, asks to configure its direction (the
	 * channel opens when both directions are). */
	c[0] = (uint8_t)peer_core_cid; c[1] = (uint8_t)(peer_core_cid >> 8);
	c[2] = c[3] = 0;
	sig(0x04, 0x22, c, 4);
	pump(20);
	CHECK("AVRCP's channel is accepted", core.profiles->avrcp_channel >= 0 && peer_answered >= 1);

	for (i = 0; i < 700 && core.profiles->a2dp.state != BTP_A2DP_STREAMING; i++)
		pump(1);
	CHECK("the link was not dropped", disconnects == 0);
	CHECK("we opened AVDTP ourselves when the speaker did not",
	      state_discover == 1 && state_start == 1);
	CHECK("and the stream runs", core.profiles->a2dp.state == BTP_A2DP_STREAMING);
	/* The speaker opened AVRCP itself, so we did not open a second. */
	for (i = 0; i < 100 && !btp_avrcp_absolute(&core); i++)
		pump(1);
	CHECK("a channel the speaker opened is used for the volume exchange",
	      btp_avrcp_absolute(&core) && avrcp_caps_seen == 1);
	CHECK("and only one AVCTP channel exists", nchans <= 3);
}

static void release_completions(void)
{
	uint8_t e[8];

	hold_completions = 0;
	if (held) {
		e[0] = 0x13; e[1] = 5; e[2] = 1; e[3] = HANDLE & 0xff;
		e[4] = HANDLE >> 8; e[5] = (uint8_t)held; e[6] = (uint8_t)(held >> 8);
		push_event(e, 7);
		held = 0;
	}
}

/* Frames sent in the next `ms`, in SBC frames. */
static unsigned int rate_over(unsigned int ms)
{
	unsigned int before = sbc_frames;

	pump(ms / 10);
	return sbc_frames - before;
}

/* AVRCP beside the stream: the controller role, against a speaker with
 * absolute volume (the engine's own bytes are in avrcp.c's checks). */
static void avrcp_scenario(void)
{
	unsigned int i;
	uint32_t g;

	printf("AVRCP: absolute volume beside the stream\n");
	for (i = 0; i < 400 && !btp_avrcp_absolute(&core); i++)
		pump(1);
	CHECK("the stream's speaker is asked for its events and registered with",
	      avrcp_caps_seen == 1 && avrcp_register_seen == 1);
	CHECK("absolute volume is active", btp_avrcp_absolute(&core));
	CHECK("the speaker's volume (64) is known and offered to the desktop",
	      btp_avrcp_speaker_volume(&core) == 64 && btp_avrcp_generation(&core) == 1);

	btp_avrcp_host_volume(&core, 100);
	pump(20);
	CHECK("the desktop's volume goes to the speaker as Set Absolute Volume",
	      avrcp_volume_sets == 1 && avrcp_last_set == 100);
	CHECK("and the speaker's answer is believed",
	      btp_avrcp_speaker_volume(&core) == 100 && btp_avrcp_generation(&core) == 1);

	avrcp_speaker_volume(50);
	pump(20);
	g = btp_avrcp_generation(&core);
	CHECK("the speaker's buttons: the desktop is told (volume 50, new generation)",
	      btp_avrcp_speaker_volume(&core) == 50 && g == 2);
	CHECK("a notification is registered again for the next press",
	      avrcp_register_seen == 2);
	CHECK("and our volume is not sent back over the speaker's",
	      avrcp_volume_sets == 1 && avrcp_volume == 50);

	avrcp_key(0x41);      /* VOLUME UP pressed */
	avrcp_key(0xc1);      /* released */
	avrcp_key(0x42);      /* VOLUME DOWN pressed */
	avrcp_key(0x42);
	avrcp_key(0xc2);
	pump(10);
	CHECK("pass-through volume keys are counted for the desktop (up 1, down 2)",
	      btp_avrcp_take_steps(&core) == -1);
	CHECK("and the stream goes on", core.profiles->a2dp.state == BTP_A2DP_STREAMING &&
	      disconnects == 0);
}

/* The desktop plays nothing: after the idle time the stream is suspended,
 * which leaves the radio to a keyboard and a mouse; sound brings it back
 * from the start of that sound. */
static void idle_suspend(void)
{
	unsigned int n, starts = state_start, sus = state_suspend, i;
	uint32_t refused, dropped, queued;

	printf("idle: a silent stream is suspended, and sound starts it again\n");
	/* A speaker that will not be suspended is left running. */
	reject_suspend = 1;
	core.profiles->a2dp.idle_ms = 2000;
	silent = 1;
	(void)rate_over(3500);
	CHECK("a speaker that rejects Suspend keeps its stream, and is not asked again",
	      core.profiles->a2dp.state == BTP_A2DP_STREAMING &&
	      core.profiles->a2dp.idle_ms == 0 && disconnects == 0);
	reject_suspend = 0;
	silent = 0;
	(void)rate_over(500);
	sus = state_suspend;
	core.profiles->a2dp.idle_ms = 3000;
	silent = 1;
	n = rate_over(1500);
	CHECK("a second or so of silence does not stop the stream",
	      core.profiles->a2dp.state == BTP_A2DP_STREAMING && n >= 100 &&
	      state_suspend == sus);
	(void)rate_over(2500);
	CHECK("after the idle time the speaker is sent Suspend",
	      state_suspend == sus + 1 &&
	      core.profiles->a2dp.state == BTP_A2DP_SUSPENDED &&
	      core.profiles->a2dp.idle_suspended);
	n = rate_over(1500);
	CHECK("and no media is sent while it is", n == 0);
	btv_sound_device_counts(&refused, &dropped, &queued);
	CHECK("the silent audio is not left to pile up", queued < 900);
	CHECK("the link stays", disconnects == 0);

	silent = 0;
	for (i = 0; i < 50 && core.profiles->a2dp.state != BTP_A2DP_STREAMING; i++)
		pump(1);
	CHECK("sound starts the stream again within half a second",
	      core.profiles->a2dp.state == BTP_A2DP_STREAMING &&
	      state_start == starts + 1);
	n = rate_over(1000);
	CHECK("at the full rate", n >= 320 && n <= 370);
	CHECK("and does not suspend while there is sound",
	      state_suspend == sus + 1 && !core.profiles->a2dp.idle_suspended);
	core.profiles->a2dp.idle_ms = 0;
}

/* After the stream is up: the radio stalls, the audio piles up, then the
 * radio comes back; and the speaker pauses and resumes the stream. Both
 * must come back to the full rate, from the present. */
static void stalls_and_pauses(void)
{
	uint8_t c[4];
	unsigned int n;
	uint32_t refused, dropped, queued;

	printf("a stalled radio and a paused stream\n");
	n = rate_over(1000);
	CHECK("the stream runs at the full rate", n >= 320 && n <= 370);

	/* A short stall: the radio is slow for 700 ms, then back. */
	hold_completions = 1;
	n = rate_over(700);
	btv_sound_device_counts(&refused, &dropped, &queued);
	/* 32-bit build: the adaptive bitpool lowers the bitpool as the queue
	 * backs up, so more (smaller) packets fit the backlog limit before the
	 * stream stops: the bound is the backlog in bytes, not 60 packets. */
	CHECK("a stalled radio stops the stream", n < 140);
	release_completions();
	n = rate_over(1000);
	btv_sound_device_counts(&refused, &dropped, &queued);
	CHECK("when the radio is back the stream is full rate in a second", n >= 250);
	n = rate_over(1000);
	CHECK("and stays there", n >= 320 && n <= 370);
	CHECK("with a short queue, not a backlog", queued <= BTV_RING_TRIM_AT + 900);

	/* Completed-packet events lost for good: the credits would never come
	 * back. After a second of nothing the core counts the packets as not in
	 * flight and the stream runs again. */
	{
		unsigned int before = core.resyncs;
		uint32_t before_dropped;

		hold_completions = 1;
		(void)rate_over(2500);
		btv_sound_device_counts(&refused, &before_dropped, &queued);
		CHECK("a second without a completed packet is noticed", core.resyncs > before);
		CHECK("and the audio piles up meanwhile, the producer being refused", refused > 0);
		hold_completions = 0;
		held = 0;                          /* the events are gone */
		(void)rate_over(4000);
		n = rate_over(1000);
		btv_sound_device_counts(&refused, &dropped, &queued);
		CHECK("the stream is back at the full rate", n >= 320 && n <= 370);
		CHECK("the oldest audio was dropped", dropped > before_dropped);
	}

	/* The speaker pauses us (AVDTP Suspend, signal 9) ... */
	c[0] = (uint8_t)(0x50 | 0); c[1] = 0x09; c[2] = 1 << 2;
	push_acl(chans[0].remote, c, 3);
	n = rate_over(1500);
	CHECK("a suspended stream stops sending", core.profiles->a2dp.state ==
	      BTP_A2DP_SUSPENDED && n < 30);
	/* ... and starts it again (Start, signal 7). */
	c[0] = (uint8_t)(0x60 | 0); c[1] = 0x07; c[2] = 1 << 2;
	push_acl(chans[0].remote, c, 3);
	pump(10);
	CHECK("a start resumes it", core.profiles->a2dp.state == BTP_A2DP_STREAMING);
	n = rate_over(1000);
	btv_sound_device_counts(&refused, &dropped, &queued);
	CHECK("at the full rate", n >= 320 && n <= 370);
	CHECK("from the present, not a backlog", queued <= BTV_RING_TRIM_AT + 900);
	CHECK("and the link was kept", disconnects == 0);
}

/* ------------------------------------------------ classic HID devices */

static void hid_reset(uint16_t flags)
{
	memset(&core, 0, sizeof core);
	memset(&profiles, 0, sizeof profiles);
	evh = evt = ah = at = 0;
	memset(chans, 0, sizeof chans);
	nchans = 0; next_cid = 0x0100; stream_media_cid = -1;
	disconnects = auths = 0;
	sdp_requests = sdp_mute = set_protocol_seen = 0;
	kb_attach = kb_detach = mouse_attach_n = pad_events = pad_detach_n = 0;
	kbd_reports = mouse_events = 0;
	memset(&btp_hid_riscos, 0, sizeof btp_hid_riscos);
	memset(&last_pad, 0, sizeof last_pad);
	protocol_byte = 0;
	handshake_result = 0;
	hold_auth = 0;
	btc_start(&core, &profiles, &completion, clock_ms);
	core.state = BTC_READY;
	core.credits = 1;
	core.acl_mtu = 1021;
	core.acl_max = core.acl_free = 8;
	btp_init(&core);
	btc_key_store(&core, PEER, 0)->flags = flags;
}

/* An interrupt-channel input report from the device. */
static void hid_report_from_device(const uint8_t *data, unsigned int n)
{
	int k = chan_by_psm(0x0013);

	if (k >= 0)
		push_acl(chans[k].remote, data, n);
}

static void hid_outgoing(void)
{
	unsigned int i;
	btp_hid *h;
	/* A keyboard with consumer keys; report 1 keyboard, 2 consumer. */
	static const uint8_t kbd_report[] = { 0xa1, 0x01, 0x02, 0x00, 0x04, 0, 0, 0, 0, 0 };
	static const uint8_t vol_up[] = { 0xa1, 0x02, 0xe9, 0x00 };
	static const uint8_t none[] = { 0xa1, 0x02, 0x00, 0x00 };
	static const uint8_t mute[] = { 0xa1, 0x02, 0xe2, 0x00 };
	/* A boot keyboard report (id 1) and a game pad report (id 1). */
	static const uint8_t boot_kbd[] = { 0xa1, 0x01, 0x00, 0x00, 0x05, 0, 0, 0, 0, 0 };
	static const uint8_t pad_report[] = { 0xa1, 0x01, 0xff, 0xff, 0x00, 0x80, 0x03, 0x05, 0x00 };

	printf("classic HID: a keyboard in report protocol, from its own descriptor\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	kbd_desc_build();
	record_boot_device = 1;
	build_record_for(kbd_desc, kbd_desc_bytes);
	CHECK("a connection for HID is started",
	      btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("the keyboard reaches Ready", h->state == BTP_HID_READY);
	CHECK("its record was read, in pieces (continuation)", sdp_requests >= 3 &&
	      h->sdp == BTP_HID_SDP_DONE);
	CHECK("the query channel was closed afterwards", core.profiles->sdp_link == 0xff);
	CHECK("report protocol was selected (SET_PROTOCOL 0x71)",
	      set_protocol_seen == 1 && protocol_byte == 0x71 && h->report_mode);
	CHECK("report IDs are in use, and the descriptor has LEDs and consumer keys",
	      h->ids_used && h->has_leds && h->extras.consumer_used == 1);
	hid_report_from_device(kbd_report, sizeof kbd_report);
	pump(5);
	CHECK("a keyboard report: the keyboard was announced once, modifiers 02 and key 04 sent",
	      kb_attach == 1 && kbd_reports == 1 && last_kbd[0] == 0x02 && last_kbd[2] == 0x04);
	hid_report_from_device(vol_up, sizeof vol_up);
	pump(5);
	CHECK("Volume Increment on the consumer report: one step up for the desktop",
	      btp_key_take_steps(&core) == 1);
	hid_report_from_device(vol_up, sizeof vol_up);
	pump(5);
	CHECK("the same report again is the key still held: no second step",
	      btp_key_take_steps(&core) == 0);
	hid_report_from_device(none, sizeof none);
	hid_report_from_device(mute, sizeof mute);
	pump(5);
	CHECK("released, then Mute: one mute press", btp_key_take_mutes(&core) == 1);

	printf("classic HID: a descriptor that does not arrive means boot protocol\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	sdp_mute = 1;
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 100 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("not Ready within a second while the query waits", h->state != BTP_HID_READY);
	for (i = 0; i < 500 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("Ready within five seconds of giving up", h->state == BTP_HID_READY);
	CHECK("on boot protocol (SET_PROTOCOL 0x70)", set_protocol_seen == 1 &&
	      protocol_byte == 0x70 && !h->report_mode);
	hid_report_from_device(boot_kbd, sizeof boot_kbd);
	pump(5);
	CHECK("a boot keyboard report is delivered as before",
	      kbd_reports == 1 && last_kbd[2] == 0x05);

	printf("classic HID: Bluetooth$HIDBoot keeps boot protocol\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	prefer_boot = 1;
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("boot protocol although the descriptor was readable",
	      h->state == BTP_HID_READY && protocol_byte == 0x70 && !h->report_mode);
	prefer_boot = 0;

	printf("classic HID: a game controller\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_GAMEPAD);
	record_boot_device = 0;
	build_record_for(gamepad, sizeof gamepad);
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("report protocol for a device with no boot mode",
	      h->state == BTP_HID_READY && protocol_byte == 0x71 && h->extras.pad_used);
	hid_report_from_device(pad_report, sizeof pad_report);
	pump(5);
	CHECK("a report: stick right, hat right, buttons 1 and 3, sent to RISC OS as one event",
	      pad_events == 1 && last_pad.pad && last_pad.axis[0] == 32767 &&
	      last_pad.hat == 3 && last_pad.pad_buttons == 5);
	CHECK("it counted as a keyboard being present (for KeyV)", kb_attach == 1);
	btc_disconnect(&core, PEER);
	pump(10);
	{
		uint8_t e[8];

		e[0] = 0x05; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8; e[5] = 0x16;
		push_event(e, 6);
	}
	pump(30);
	CHECK("when it goes its keys are let go", pad_detach_n == 1);
}

/* A keyboard that wakes up: it pages us, authenticates, and opens the
 * control and interrupt channels itself.  We read its record meanwhile (or,
 * when the bond holds it from last time, do not) and select the protocol
 * once we have it, not before. */
static void wake_and_connect(void)
{
	uint8_t e[16], c[8];

	e[0] = 0x04; e[1] = 10; memcpy(e + 2, PEER, 6);
	e[8] = 0x40; e[9] = 0x05; e[10] = 0x00; e[11] = 0x01;
	push_event(e, 12);
	pump(4);
	e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	memcpy(e + 5, PEER, 6); e[11] = 1; e[12] = 0;
	push_event(e, 13);
	pump(4);
	e[0] = 0x17; e[1] = 6; memcpy(e + 2, PEER, 6);
	push_event(e, 8);
	pump(4);
	e[0] = 0x06; e[1] = 3; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	push_event(e, 5);
	pump(4);
	e[0] = 0x08; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	e[5] = 1;
	push_event(e, 6);
	pump(3);
	/* The keyboard opens the control channel, then the interrupt channel. */
	c[0] = 0x11; c[1] = 0; c[2] = 0x40; c[3] = 0;
	sig(0x02, 0x31, c, 4);
	pump(6);
	chans[nchans].local = 0x40; chans[nchans].remote = peer_core_cid;
	chans[nchans++].psm = 0x0011;
	c[0] = (uint8_t)peer_core_cid; c[1] = (uint8_t)(peer_core_cid >> 8); c[2] = c[3] = 0;
	sig(0x04, 0x32, c, 4);
	pump(6);
	c[0] = 0x13; c[1] = 0; c[2] = 0x41; c[3] = 0;
	sig(0x02, 0x33, c, 4);
	pump(6);
	chans[nchans].local = 0x41; chans[nchans].remote = peer_core_cid;
	chans[nchans++].psm = 0x0013;
	c[0] = (uint8_t)peer_core_cid; c[1] = (uint8_t)(peer_core_cid >> 8); c[2] = c[3] = 0;
	sig(0x04, 0x34, c, 4);
}

static void hid_incoming(void)
{
	unsigned int i;
	btp_hid *h;
	btc_key kept;
	static const uint8_t kbd_report[] = { 0xa1, 0x01, 0x00, 0x00, 0x06, 0, 0, 0, 0, 0 };

	printf("classic HID: a keyboard that wakes up and connects to us\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	kbd_desc_build();
	record_boot_device = 1;
	build_record_for(kbd_desc, kbd_desc_bytes);
	wake_and_connect();
	h = &core.profiles->hid[0];
	CHECK("the channels are open and its record is being read: no protocol chosen yet",
	      set_protocol_seen == 0 || h->sdp == BTP_HID_SDP_DONE);
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("then report protocol is selected", h->state == BTP_HID_READY &&
	      set_protocol_seen == 1 && protocol_byte == 0x71 && h->report_mode);
	CHECK("the record was read over a channel of our own", sdp_requests >= 3);
	hid_report_from_device(kbd_report, sizeof kbd_report);
	pump(5);
	CHECK("a key is delivered", kbd_reports == 1 && last_kbd[2] == 0x06);
	CHECK("the descriptor went with the bond",
	      btc_key_find(&core, PEER)->gatt.valid == 2);
	CHECK("the first report is counted and decoded", h->reports == 1 && h->decoded == 1);
	kept = *btc_key_find(&core, PEER);

	printf("classic HID: the same keyboard wakes again; its descriptor is in the bond\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	*btc_key_find(&core, PEER) = kept;
	sdp_mute = 1;                       /* a device that does not answer SDP now */
	wake_and_connect();
	h = &core.profiles->hid[0];
	for (i = 0; i < 60 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("it is Ready within a fraction of a second, with no query",
	      h->state == BTP_HID_READY && sdp_requests == 0 && h->from_cache);
	CHECK("report protocol was selected", set_protocol_seen == 1 &&
	      protocol_byte == 0x71 && h->report_mode);
	hid_report_from_device(kbd_report, sizeof kbd_report);
	pump(5);
	CHECK("and the first key press is not lost", kbd_reports == 1 && last_kbd[2] == 0x06);

	printf("classic HID: a kept descriptor that fits nothing the device sends\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	*btc_key_find(&core, PEER) = kept;
	sdp_mute = 1;
	wake_and_connect();
	h = &core.profiles->hid[0];
	for (i = 0; i < 60 && h->state != BTP_HID_READY; i++)
		pump(1);
	{
		static const uint8_t strange[] = { 0xa1, 0x05, 0x01, 0x02, 0x03 };

		for (i = 0; i < 8; i++) {
			hid_report_from_device(strange, sizeof strange);
			pump(3);
		}
		CHECK("eight reports that the kept descriptor does not describe: it is dropped",
		      btc_key_find(&core, PEER)->gatt.valid == 0 && h->unmatched == 8);
	}
}

/* A keyboard that is pairing: the person reads the passkey off the screen
 * and types it on the keyboard, which takes longer than any step of the
 * link's set-up may. */
static void hid_slow_pairing(void)
{
	unsigned int i;
	uint8_t e[8];
	btp_hid *h;

	printf("classic HID: a pairing that takes the person thirty seconds\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	kbd_desc_build();
	record_boot_device = 1;
	build_record_for(kbd_desc, kbd_desc_bytes);
	hold_auth = 1;
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 3000; i++)
		pump(1);
	CHECK("thirty seconds on it is still waiting to be secured, not dropped",
	      h->state == BTP_HID_SECURING && disconnects == 0 && auths == 1);
	hold_auth = 0;
	e[0] = 0x06; e[1] = 3; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	push_event(e, 5);                       /* the passkey was right */
	pump(5);
	e[0] = 0x08; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
	e[5] = 1;
	push_event(e, 6);
	for (i = 0; i < 400 && h->state != BTP_HID_READY; i++)
		pump(1);
	CHECK("and the keyboard then comes Ready", h->state == BTP_HID_READY);

	printf("classic HID: nobody ever types it: dropped after a minute\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	hold_auth = 1;
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	for (i = 0; i < 7000; i++)
		pump(1);
	CHECK("after a minute and ten seconds the link is let go", disconnects == 1);
	hold_auth = 0;
}

/* A device that will not leave boot protocol, one that sends boot-format
 * reports though it was told report protocol, and what *BTInfo says. */
static void hid_odd(void)
{
	unsigned int i;
	btp_hid *h;
	static const uint8_t keyboard_only[] = {
		0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
		0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
		0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
		0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
		0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0
	};
	static const uint8_t boot_kbd[] = { 0xa1, 0x01, 0x00, 0x00, 0x07, 0, 0, 0, 0, 0 };
	static const uint8_t boot_mouse[] = { 0xa1, 0x02, 0x01, 0x05, 0xfb };
	static char text[8192];
	size_t n;

	printf("classic HID: a device that answers SET_PROTOCOL(report) with an error\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	handshake_result = 0x03;                   /* ERR_UNSUPPORTED_REQUEST */
	record_boot_device = 1;
	build_record_for(keyboard_only, sizeof keyboard_only);
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	pump(10);
	CHECK("a boot device that refuses is put in boot protocol (SET_PROTOCOL 0x70)",
	      set_protocol_seen == 2 && protocol_byte == 0x70 && !h->report_mode &&
	      h->boot_after_handshake);
	hid_report_from_device(boot_kbd, sizeof boot_kbd);
	pump(5);
	CHECK("and its boot reports are delivered", kbd_reports == 1 && last_kbd[2] == 0x07);

	printf("classic HID: a device with no boot mode that refuses stays in report protocol\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	handshake_result = 0x03;
	record_boot_device = 0;
	build_record_for(keyboard_only, sizeof keyboard_only);
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	pump(10);
	CHECK("SET_PROTOCOL was sent once, and report protocol is kept",
	      set_protocol_seen == 1 && protocol_byte == 0x71 && h->report_mode);
	CHECK("the handshake result is on record", (h->handshake & 0x80) && (h->handshake & 0x0f) == 3);

	printf("classic HID: boot-format reports from a device told report protocol\n");
	hid_reset(BTC_DEV_HID | BTC_DEV_KEYBOARD);
	record_boot_device = 1;
	build_record_for(keyboard_only, sizeof keyboard_only);
	CHECK("a connection is started", btc_connect(&core, PEER, 0, 0, BTC_PROFILE_HID));
	h = &core.profiles->hid[0];
	for (i = 0; i < 300 && h->state != BTP_HID_READY; i++)
		pump(1);
	hid_report_from_device(boot_mouse, sizeof boot_mouse);
	pump(5);
	CHECK("a mouse report (ID 2) that the descriptor has no place for is not lost",
	      h->unmatched == 1 && h->fallbacks == 1 && mouse_events == 1);
	CHECK("the mouse was announced to the desktop", mouse_attach_n == 1);

	printf("*BTInfo\n");
	n = btinfo_text(&core, text, sizeof text);
	CHECK("it writes a report", n > 200 && n < sizeof text - 1);
	CHECK("that says the state and the HID link's protocol and counts",
	      strstr(text, "BluetoothVoice: ready") != 0 &&
	      strstr(text, "protocol report") != 0 &&
	      strstr(text, "boot-format fallbacks 1") != 0 &&
	      strstr(text, "unmatched 1") != 0);
	CHECK("and the pointer type line for the mouse",
	      strstr(text, "Pointer: type in use") != 0);
	CHECK("and the bonded device", strstr(text, "Bonded devices:") != 0 &&
	      strstr(text, "66:55:44:33:22:11") != 0);
	if (getenv("BTINFO_PRINT") != 0)
		fputs(text, stdout);
	{
		char small[64];
		n = btinfo_text(&core, small, sizeof small);
		CHECK("a small buffer is filled to its end, never past it",
		      n < sizeof small && strlen(small) == n);
	}
}

int main(void)
{
	unsigned int i;
	char what[96];

	printf("a2dp\n");
	btc_start(&core, &profiles, &completion, clock_ms);
	core.state = BTC_READY;
	core.credits = 1;
	core.acl_mtu = 1021;
	core.acl_max = core.acl_free = 8;
	btp_init(&core);
	btc_key_store(&core, PEER, 0)->flags = BTC_DEV_AUDIO_SINK;

	CHECK("a connection for audio is started",
	      btc_connect(&core, PEER, 0, 0, BTC_PROFILE_A2DP));
	for (i = 0; i < 400 && core.profiles->a2dp.state != BTP_A2DP_STREAMING; i++)
		pump(1);
	snprintf(what, sizeof what, "the stream reaches Streaming (state %u error %u "
	         "after %u pumps)", core.profiles->a2dp.state,
	         core.profiles->a2dp.error, i);
	CHECK(what, core.profiles->a2dp.state == BTP_A2DP_STREAMING);
	CHECK("the speaker was discovered, asked for its capabilities,",
	      state_discover == 1 && state_caps >= 1);
	CHECK("configured, opened and started, each once",
	      state_config == 1 && state_open == 1 && state_start == 1);
	CHECK("the configuration is SBC at 44.1 kHz, joint stereo",
	      (configured[0] & 0x30) == 0x20 && (configured[0] & 0x0f) == 0x01);
	CHECK("the sound device is asked for", sound_on);

	/* A second of stream: the audio needs 44100 / 128 = 345 SBC frames. */
	media_packets = sbc_frames = 0;
	for (i = 0; i < 100; i++)
		pump(1);
	snprintf(what, sizeof what, "a second of stream carries ~345 SBC frames (got %u in "
	         "%u packets)", sbc_frames, media_packets);
	CHECK(what, sbc_frames >= 320 && sbc_frames <= 370);
	CHECK("every media packet is RTP with SBC frames in it", bad_packets == 0);
	CHECK("the link was never dropped", disconnects == 0);

	avrcp_scenario();
	idle_suspend();
	stalls_and_pauses();
	reconnect();
	hid_outgoing();
	hid_incoming();
	hid_slow_pairing();
	hid_odd();
	return check_summary("a2dp");
}
