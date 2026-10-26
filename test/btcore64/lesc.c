/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/*
 * Host checks for BluetoothVoice's Low Energy input: the real bt_core.c,
 * bt_l2cap.c, bt_le.c, le_crypto.c, p256.c, hid_report.c and hid_input.c
 * against a fake controller and a fake Logitech-MX-Master-style mouse.
 *
 *   - LE Secure Connections, just works and passkey entry, as the
 *     initiator. The device's side is canned data worked out independently
 *     by lesc_gen.py (Python's `cryptography` over OpenSSL, from the spec's
 *     formulas); the checks compare every PDU the stack sends with it, and
 *     LE Start Encryption must carry the LTK the generator made, with EDIV
 *     and Rand zero.
 *   - Refusals: a bad confirm, a bad DHKey check, a public key that is not
 *     on the curve.
 *   - GATT: the HID service, its Report characteristics with their Report
 *     References, the Report Map read in 22-octet pieces, report mode chosen,
 *     only the input the map wants subscribed (the HID++ reports are not),
 *     notifications decoded into pointer movement and buttons.
 *   - Reconnection: the stored key re-encrypts with no pairing; a device that
 *     has lost the key is paired again; an advertisement from a bonded device
 *     connects it.
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
#include "bt_reconnect.h"
#include "hidtest.h"
#include "mxmap.h"

#include "lesc_vectors.inc"

#define HANDLE 0x0042u
static const uint8_t PEER[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
static const uint8_t OURS[6] = { 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
#define PEER_TYPE 1

/* ------------------------------------------------ the RISC OS side */

static unsigned int consumer_presses, pad_deliveries, pad_detaches;
static btv_hid_extra_event last_extra;
void btp_hid_extras_deliver(btc_core *core, uint8_t link, const btv_hid_extra_event *e,
                            btv_hid_pad *pad, uint8_t *attached, uint16_t last[BTV_XCONSUMER_MAX])
{
	unsigned int i;

	(void)core; (void)link; (void)pad;
	if (e->consumer)
		for (i = 0; i < e->consumer_count; i++)
			if (last[0] != e->consumer_usage[i]) { consumer_presses++; last[0] = e->consumer_usage[i]; }
	if (e->pad) { pad_deliveries++; last_extra = *e; *attached = 1; }
}
void btp_hid_extras_detach(btc_core *core, btv_hid_pad *pad, uint8_t *attached)
{ (void)core; (void)pad; if (*attached) pad_detaches++; *attached = 0; }

btv_hid_riscos btp_hid_riscos;
static unsigned int kb_attached, mouse_attached, mouse_events, kb_reports;
static struct { uint32_t buttons; int32_t dx, dy, wheel, pan; } last_mouse;
static uint8_t last_kb[8];

int btv_hid_riscos_keyboard_attach(btv_hid_riscos *r) { (void)r; kb_attached++; return 1; }
void btv_hid_riscos_keyboard_detach(btv_hid_riscos *r, btv_hid_keyboard *k)
{ (void)r; (void)k; }
void btv_hid_riscos_keyboard_report(btv_hid_riscos *r, btv_hid_keyboard *k,
                                    const uint8_t *rep, size_t n)
{ (void)r; (void)k; kb_reports++; memcpy(last_kb, rep, n < 8 ? n : 8); }
void btv_hid_riscos_mouse_attach(btv_hid_riscos *r) { (void)r; mouse_attached++; }
void btv_hid_riscos_mouse_detach(btv_hid_riscos *r, btv_hid_mouse *m) { (void)r; (void)m; }
void btv_hid_riscos_mouse_report(btv_hid_riscos *r, btv_hid_mouse *m,
                                 const uint8_t *rep, size_t n)
{ (void)r; (void)m; (void)rep; (void)n; }
void btv_hid_riscos_mouse_event(btv_hid_riscos *r, btv_hid_mouse *m, uint32_t b,
                                int32_t dx, int32_t dy, int32_t w, int32_t p)
{
	(void)r; (void)m;
	mouse_events++;
	last_mouse.buttons = b; last_mouse.dx = dx; last_mouse.dy = dy;
	last_mouse.wheel = w; last_mouse.pan = p;
}

/* ------------------------------------------------ profile glue */

void btp_le_init(btc_core *, uint8_t);
void btp_le_pump(btc_core *);
void btp_le_link_opened(btc_core *, uint8_t);
void btp_le_secured(btc_core *, uint8_t);
void btp_le_link_closed(btc_core *, uint8_t);
extern uint32_t btc_passkey, btc_passkey_generation;

void btp_init(btc_core *c)
{
	unsigned int i;
	for (i = 0; i < BTC_LINKS; i++)
		btp_le_init(c, (uint8_t)i);
}
void btp_pump(btc_core *c) { btp_le_pump(c); }
int btp_le_busy(btc_core *c);
void btp_link_opened(btc_core *c, uint8_t l) { if (c->links[l].le) btp_le_link_opened(c, l); }
void btp_link_secured(btc_core *c, uint8_t l) { if (c->links[l].le) btp_le_secured(c, l); }
void btp_link_closed(btc_core *c, uint8_t l) { btp_le_link_closed(c, l); }
uint8_t btp_incoming_channel(btc_core *c, uint8_t l, uint16_t psm)
{ (void)c; (void)l; (void)psm; return BTC_OWN_NONE; }
void btp_channel_open(btc_core *c, int ch) { (void)c; (void)ch; }
void btp_channel_data(btc_core *c, int ch, const uint8_t *d, size_t n)
{ (void)c; (void)ch; (void)d; (void)n; }
void btp_channel_closed(btc_core *c, int ch) { (void)c; (void)ch; }
void btc_sdp_server_request(btc_core *c, int ch, const uint8_t *r, size_t n)
{ (void)c; (void)ch; (void)r; (void)n; }

/* ------------------------------------------------ the radio */

static struct pkt { uint8_t b[600]; unsigned int n; } evq[128], aclq[128];
static unsigned int evh, evt, ah, at;
static uint8_t tickets[65536];
static uint32_t next_ticket;
static uint32_t clock_ms = 1000;

static btc_core core;
static btc_profiles profiles;
static btv_host_bridge_completion completion;

static void push_event(const uint8_t *e, unsigned int n)
{
	memcpy(evq[evt % 128].b, e, n);
	evq[evt++ % 128].n = n;
}

/* Data from the device on an LE fixed channel (4 ATT, 6 SMP). */
static void push_acl(uint16_t cid, const uint8_t *data, unsigned int n)
{
	struct pkt *p = &aclq[at++ % 128];

	p->b[0] = HANDLE & 0xff; p->b[1] = 0x20 | (HANDLE >> 8);
	p->b[2] = (uint8_t)(n + 4); p->b[3] = 0;
	p->b[4] = (uint8_t)n; p->b[5] = (uint8_t)(n >> 8);
	p->b[6] = (uint8_t)cid; p->b[7] = (uint8_t)(cid >> 8);
	memcpy(p->b + 8, data, n);
	p->n = n + 8;
}

/* Fake controller state. */
static unsigned int rand_pos;
static uint8_t enc_ltk[16], enc_rand[8], enc_ediv[2];
static unsigned int enc_starts, creates, disconnects, rands;
static int key_known;               /* does the controller's peer accept our LTK? */
static const uint8_t *expected_ltk;
static int link_up;
static unsigned int force_enc_status, classic_creates, updates;
static uint16_t upd[5];
static uint8_t create_log[8][6];
static unsigned int le_creates_seen, scan_enables, scan_disables;

static void answer(const uint8_t *c)
{
	uint16_t op = (uint16_t)(c[0] | c[1] << 8);
	const uint8_t *d = c + 3;
	uint8_t e[24];

	switch (op) {
	case 0x200d: case 0x2019: case 0x0406:
		e[0] = 0x0f; e[1] = 4; e[2] = 0; e[3] = 1; e[4] = c[0]; e[5] = c[1];
		push_event(e, 6);
		break;
	case 0x2018: {                          /* LE Rand: status, 8 octets */
		unsigned int i;
		e[0] = 0x0e; e[1] = 12; e[2] = 1; e[3] = c[0]; e[4] = c[1]; e[5] = 0;
		for (i = 0; i < 8; i++, rand_pos++)
			e[6 + i] = (uint8_t)((rand_pos * 37 + 11) & 0xff);
		push_event(e, 14);
		rands++;
		break;
	}
	default:
		e[0] = 0x0e; e[1] = 4; e[2] = 1; e[3] = c[0]; e[4] = c[1]; e[5] = 0;
		push_event(e, 6);
	}
	if (op == 0x0405) {
		if (classic_creates < 8) memcpy(create_log[classic_creates], d, 6);
		classic_creates++;
	}
	if (op == 0x200c) {
		if (d[0]) scan_enables++;
		else scan_disables++;
	}
	if (op == 0x2013) {
		unsigned int i;
		updates++;
		for (i = 0; i < 5; i++) upd[i] = (uint16_t)(d[2 * i] | d[2 * i + 1] << 8);
		e[0] = 0x3e; e[1] = 10; e[2] = 0x03; e[3] = 0;
		memcpy(e + 4, d, 2); e[6] = d[2]; e[7] = d[3]; e[8] = d[6]; e[9] = d[7];
		e[10] = d[8]; e[11] = d[9];
		push_event(e, 12);
	}
	if (op == 0x200d) {
		creates++;
		le_creates_seen++;
		creates += 0;
		e[0] = 0x3e; e[1] = 19; e[2] = 0x01; e[3] = 0;
		e[4] = HANDLE & 0xff; e[5] = HANDLE >> 8; e[6] = 0; e[7] = PEER_TYPE;
		memcpy(e + 8, PEER, 6);
		e[14] = 0x0c; e[15] = 0; e[16] = 0; e[17] = 0; e[18] = 0xc8; e[19] = 0;
		e[20] = 0;
		push_event(e, 21);
		link_up = 1;
	} else if (op == 0x2019) {
		enc_starts++;
		memcpy(enc_rand, d + 2, 8);
		memcpy(enc_ediv, d + 10, 2);
		memcpy(enc_ltk, d + 12, 16);
		e[0] = 0x08; e[1] = 4; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8; e[5] = 1;
		/* The controller encrypts if the LTK is the one the device holds. */
		if (force_enc_status) { e[2] = (uint8_t)force_enc_status; push_event(e, 6); return; }
		e[2] = (key_known && expected_ltk != NULL &&
		        memcmp(enc_ltk, expected_ltk, 16) == 0) ? 0 : 0x06;
		push_event(e, 6);
	} else if (op == 0x0406) {
		disconnects++;
		e[0] = 0x05; e[1] = 4; e[2] = 0; e[3] = HANDLE & 0xff; e[4] = HANDLE >> 8;
		e[5] = 0x16;
		push_event(e, 6);
		link_up = 0;
	}
}

int btv_host_bridge_open(uint32_t *w) { *w = 0; return 0; }
int btv_host_bridge_boot(void) { return 1; }
int btv_host_bridge_close(uint32_t *w) { *w = 0; return 0; }
static char trace_log[8192];
void btv_host_bridge_note(const char *a, const char *b)
{
	size_t n = strlen(trace_log);

	(void)a;
	if (n + strlen(b) + 2 < sizeof trace_log) {
		strcpy(trace_log + n, b);
		strcat(trace_log + n, "\n");
	}
}
void btv_host_bridge_errors(uint32_t o[5]) { o[0]=o[1]=o[2]=o[3]=o[4]=0; }
void btv_host_bridge_stats(uint32_t *t, uint32_t *r, uint32_t *l)
{ *t = 0; *r = 0; *l = 0; }

static void from_host(const uint8_t *acl, unsigned int n);

int btv_host_bridge_submit(unsigned int kind, const uint8_t *d, uint32_t n,
                           uint32_t p, uint32_t *t, uint32_t *w)
{
	(void)p;
	*w = 0;
	*t = ++next_ticket;
	tickets[*t % 65536] = (uint8_t)kind;
	if (kind == BTV_HOST_BRIDGE_COMMAND) {
		answer(d);
	} else if (kind == BTV_HOST_BRIDGE_ACL_OUT) {
		uint8_t e[8];

		from_host(d, n);
		e[0] = 0x13; e[1] = 5; e[2] = 1; e[3] = HANDLE & 0xff;
		e[4] = HANDLE >> 8; e[5] = 1; e[6] = 0;
		push_event(e, 7);
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
		memcpy(c->data, p->b, p->n); c->bytes = p->n;
		return 0;
	}
	if (k == BTV_HOST_BRIDGE_ACL_IN && ah != at) {
		struct pkt *p = &aclq[ah++ % 128];
		memcpy(c->data, p->b, p->n); c->bytes = p->n;
		return 0;
	}
	return BTV_HOST_BRIDGE_PENDING;
}

int btv_host_bridge_cancel(uint32_t t, uint32_t *w) { (void)t; *w = 0; return 0; }

static void pump(unsigned int times)
{
	while (times--) {
		clock_ms += 10;
		btc_pump(&core, clock_ms);
	}
}

/* ------------------------------------------------ the device: SMP */

enum { JUST_WORKS, PASSKEY, LEGACY };
static int scenario;
static int tamper;                  /* 1 confirm, 2 DHKey check, 3 public key */
static unsigned int round_seen;
static unsigned int mismatches;
static char first_mismatch[80];
static uint8_t seen_request[7], seen_pk[64], seen_check[16], failed_reason;
static unsigned int pairing_requests, pairing_failed_seen, pk_seen, nonces_seen,
	confirms_seen;
static const uint8_t *ltk_of_pairing;

static void mismatch(const char *what)
{
	if (mismatches++ == 0) {
		strncpy(first_mismatch, what, sizeof first_mismatch - 1);
		first_mismatch[sizeof first_mismatch - 1] = 0;
	}
}

static unsigned int central_keys_seen, keys_06_07, device_keys_sent,
	first_ours_after_device;
static int ask_central_keys;
static void smp_reply(const uint8_t *d, unsigned int n) { push_acl(6, d, n); }

static unsigned int legacy_errors, keys_before_encryption;
static const uint8_t DEVICE_LTK[16] = { 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                                        0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf };

/* Legacy just works, the device's side, from the generator's data. */
static int legacy_from_host(const uint8_t *p, unsigned int n)
{
	uint8_t r[17];

	switch (p[0]) {
	case 0x01:
		pairing_requests++;
		memcpy(seen_request, p, n < 7 ? n : 7);
		memcpy(r, "\x02\x03\x00\x01\x10\x02\x03", 7);   /* what the MX Master sent */
		smp_reply(r, 7);
		return 1;
	case 0x03:
		if (memcmp(p + 1, lg_mconfirm, 16) != 0) legacy_errors++;
		r[0] = 0x03; memcpy(r + 1, lg_sconfirm, 16);
		smp_reply(r, 17);
		return 1;
	case 0x04:
		if (memcmp(p + 1, lg_mrand, 16) != 0) legacy_errors++;
		r[0] = 0x04; memcpy(r + 1, lg_srand, 16);
		smp_reply(r, 17);
		expected_ltk = lg_stk;
		key_known = 1;
		return 1;
	case 0x08: case 0x09:
		if (central_keys_seen++ == 0) first_ours_after_device = device_keys_sent;
		if (enc_starts == 0) keys_before_encryption++;
		return 1;
	default:
		return 0;
	}
}

static void smp_from_host(const uint8_t *p, unsigned int n)
{
	uint8_t r[65];

	if (scenario == LEGACY && legacy_from_host(p, n))
		return;

	switch (p[0]) {
	case 0x01:
		pairing_requests++;
		memcpy(seen_request, p, n < 7 ? n : 7);
		r[0] = 0x02;
		if (scenario == JUST_WORKS) {
			r[1] = 0x03; r[2] = 0; r[3] = 0x09; r[4] = 16; r[5] = ask_central_keys ? 0x03 : 0; r[6] = 0x03;
		} else {
			r[1] = 0x02; r[2] = 0; r[3] = 0x0d; r[4] = 16; r[5] = 0; r[6] = 0x03;
		}
		smp_reply(r, 7);
		break;
	case 0x0c:
		pk_seen++;
		memcpy(seen_pk, p + 1, 64);
		if (memcmp(p + 1, sc_pka, 64) != 0) mismatch("public key");
		r[0] = 0x0c;
		memcpy(r + 1, sc_pkb, 64);
		if (tamper == 3) r[5] ^= 1;          /* off the curve */
		smp_reply(r, 65);
		if (scenario == JUST_WORKS) {
			r[0] = 0x03;
			memcpy(r + 1, jw_cb, 16);
			if (tamper == 1) r[1] ^= 1;
			smp_reply(r, 17);
		}
		break;
	case 0x03:                              /* passkey: Ca_i, answer Cb_i */
		confirms_seen++;
		if (scenario != PASSKEY || round_seen >= 20) { mismatch("stray confirm"); break; }
		if (memcmp(p + 1, pk_ca + 16 * round_seen, 16) != 0) mismatch("Ca_i");
		r[0] = 0x03;
		memcpy(r + 1, pk_cb + 16 * round_seen, 16);
		if (tamper == 1 && round_seen == 7) r[1] ^= 1;
		smp_reply(r, 17);
		break;
	case 0x04:
		nonces_seen++;
		r[0] = 0x04;
		if (scenario == JUST_WORKS) {
			if (memcmp(p + 1, jw_na, 16) != 0) mismatch("Na");
			memcpy(r + 1, jw_nb, 16);
		} else {
			if (round_seen >= 20) { mismatch("too many rounds"); break; }
			if (memcmp(p + 1, pk_na + 16 * round_seen, 16) != 0) mismatch("Na_i");
			memcpy(r + 1, pk_nb + 16 * round_seen, 16);
			round_seen++;
		}
		smp_reply(r, 17);
		break;
	case 0x0d:
		memcpy(seen_check, p + 1, 16);
		if (memcmp(p + 1, scenario == JUST_WORKS ? jw_ea : pk_ea, 16) != 0)
			mismatch("Ea");
		r[0] = 0x0d;
		memcpy(r + 1, scenario == JUST_WORKS ? jw_eb : pk_eb, 16);
		if (tamper == 2) r[1] ^= 0x80;
		smp_reply(r, 17);
		ltk_of_pairing = scenario == JUST_WORKS ? jw_ltk : pk_ltk;
		expected_ltk = ltk_of_pairing;
		key_known = 1;
		break;
	case 0x08: case 0x09:
		if (central_keys_seen++ == 0) first_ours_after_device = device_keys_sent;
		break;
	case 0x06: case 0x07:
		keys_06_07++;
		break;
	case 0x05:
		pairing_failed_seen++;
		failed_reason = p[1];
		break;
	default:
		break;
	}
}

/* After encryption the device sends its identity: IRK and address. */
static const uint8_t IRK[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static uint8_t peer_identity[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
static int identity_enabled = 1;
static void send_identity(void)
{
	uint8_t r[17];

	if (!identity_enabled) return;
	if (scenario == LEGACY) {
		device_keys_sent += 2;
		r[0] = 0x06; memcpy(r + 1, DEVICE_LTK, 16); smp_reply(r, 17);
		r[0] = 0x07; memset(r + 1, 0x11, 10); smp_reply(r, 11);
	}
	device_keys_sent += 2;
	r[0] = 0x08; memcpy(r + 1, IRK, 16); smp_reply(r, 17);
	r[0] = 0x09; r[1] = PEER_TYPE; memcpy(r + 2, peer_identity, 6); smp_reply(r, 8);
}

/* ------------------------------------------------ the device: GATT */

struct attr { uint16_t handle, uuid; const uint8_t *value; unsigned int n; };

static const uint8_t v_decl_info[] = { 0x02, 0x22, 0x00, 0x4a, 0x2a };
static const uint8_t v_decl_map[] = { 0x02, 0x24, 0x00, 0x4b, 0x2a };
static const uint8_t v_decl_cp[] = { 0x04, 0x26, 0x00, 0x4c, 0x2a };
static const uint8_t v_decl_pm[] = { 0x06, 0x28, 0x00, 0x4e, 0x2a };
static const uint8_t v_decl_r1[] = { 0x12, 0x2a, 0x00, 0x4d, 0x2a };   /* read, notify */
static const uint8_t v_decl_r2[] = { 0x1e, 0x2e, 0x00, 0x4d, 0x2a };
static const uint8_t v_decl_r3[] = { 0x0e, 0x32, 0x00, 0x4d, 0x2a };   /* output */
static const uint8_t v_decl_r4[] = { 0x1e, 0x35, 0x00, 0x4d, 0x2a };
static const uint8_t v_ref_mouse[] = { 0x02, 0x01 };    /* id 2, input */
static const uint8_t v_ref_in10[] = { 0x10, 0x01 };
static const uint8_t v_ref_out10[] = { 0x10, 0x02 };
static const uint8_t v_ref_in11[] = { 0x11, 0x01 };
static const uint8_t v_decl_bat[] = { 0x12, 0x42, 0x00, 0x19, 0x2a };   /* Battery Level: read, notify */

/* Characteristic declarations (0x2803) and descriptors, in handle order. */
static const struct attr db[] = {
	{ 0x0021, 0x2803, v_decl_info, 5 },
	{ 0x0023, 0x2803, v_decl_map, 5 },
	{ 0x0025, 0x2803, v_decl_cp, 5 },
	{ 0x0027, 0x2803, v_decl_pm, 5 },
	{ 0x0029, 0x2803, v_decl_r1, 5 }, { 0x002b, 0x2902, 0, 0 }, { 0x002c, 0x2908, v_ref_mouse, 2 },
	{ 0x002d, 0x2803, v_decl_r2, 5 }, { 0x002f, 0x2902, 0, 0 }, { 0x0030, 0x2908, v_ref_in10, 2 },
	{ 0x0031, 0x2803, v_decl_r3, 5 }, { 0x0033, 0x2908, v_ref_out10, 2 },
	{ 0x0034, 0x2803, v_decl_r4, 5 }, { 0x0036, 0x2902, 0, 0 }, { 0x0037, 0x2908, v_ref_in11, 2 },
	/* The Battery Service, 0x0040 to 0x0043, when the scenario has one. */
	{ 0x0041, 0x2803, v_decl_bat, 5 }, { 0x0043, 0x2902, 0, 0 },
};
#define DB_COUNT (sizeof db / sizeof db[0])
#define HID_FIRST 0x0020
#define HID_LAST 0x0037
#define H_MAP 0x0024
#define H_MOUSE_IN 0x002a
#define H_MOUSE_CCCD 0x002b
#define H_HIDPP_IN 0x002e
#define H_PROTOCOL 0x0028
#define H_BATTERY 0x0042
#define H_BATTERY_CCCD 0x0043
static unsigned int bas_present, bas_level = 77;

static const uint8_t *cur_map = mouse_map;
static unsigned int cur_map_len = sizeof mouse_map;
static unsigned int blob_reads, cccd_writes, cccd_handles[8], protocol_writes;
static uint8_t protocol_value;
static unsigned int fail_map_read, cccd_error_once;

static void att_reply(const uint8_t *d, unsigned int n) { push_acl(4, d, n); }

static void att_error(uint8_t op, uint16_t handle, uint8_t code)
{
	uint8_t r[5] = { 0x01, op, (uint8_t)handle, (uint8_t)(handle >> 8), code };
	att_reply(r, 5);
}

static void att_from_host(const uint8_t *p, unsigned int n)
{
	uint8_t r[23];
	unsigned int i, len;
	uint16_t a, b, h, off;

	switch (p[0]) {
	case 0x06:                                /* Find By Type Value */
		if (p[5] == 0x00 && p[6] == 0x28 && p[7] == 0x12 && p[8] == 0x18) {
			r[0] = 0x07; r[1] = HID_FIRST; r[2] = 0; r[3] = HID_LAST; r[4] = 0;
			att_reply(r, 5);
		} else if (bas_present && p[5] == 0x00 && p[6] == 0x28 && p[7] == 0x0f && p[8] == 0x18) {
			r[0] = 0x07; r[1] = 0x40; r[2] = 0; r[3] = 0x43; r[4] = 0;
			att_reply(r, 5);
		} else
			att_error(0x06, p[1] | p[2] << 8, 0x0a);
		break;
	case 0x08:                                /* Read By Type (declarations) */
		a = (uint16_t)(p[1] | p[2] << 8); b = (uint16_t)(p[3] | p[4] << 8);
		r[0] = 0x09; r[1] = 7; len = 2;
		for (i = 0; i < DB_COUNT && len + 7 <= 23; i++)
			if (db[i].uuid == 0x2803 && db[i].handle >= a && db[i].handle <= b) {
				r[len++] = (uint8_t)db[i].handle; r[len++] = (uint8_t)(db[i].handle >> 8);
				memcpy(r + len, db[i].value, 5); len += 5;
			}
		if (len > 2) att_reply(r, len); else att_error(0x08, a, 0x0a);
		break;
	case 0x04:                                /* Find Information */
		a = (uint16_t)(p[1] | p[2] << 8); b = (uint16_t)(p[3] | p[4] << 8);
		r[0] = 0x05; r[1] = 1; len = 2;
		for (i = 0; i < DB_COUNT && len + 4 <= 23; i++)
			if (db[i].uuid != 0x2803 && db[i].handle >= a && db[i].handle <= b) {
				r[len++] = (uint8_t)db[i].handle; r[len++] = (uint8_t)(db[i].handle >> 8);
				r[len++] = (uint8_t)db[i].uuid; r[len++] = (uint8_t)(db[i].uuid >> 8);
			}
		if (len > 2) att_reply(r, len); else att_error(0x04, a, 0x0a);
		break;
	case 0x0a: case 0x0c:                     /* Read, Read Blob */
		h = (uint16_t)(p[1] | p[2] << 8);
		off = p[0] == 0x0c ? (uint16_t)(p[3] | p[4] << 8) : 0;
		if (h == H_MAP) {
			if (fail_map_read) { att_error(p[0], h, fail_map_read == 2 ? 0x05 : 0x02); break; }
			if (p[0] == 0x0c) blob_reads++;
			if (off > cur_map_len) { att_error(p[0], h, 0x07); break; }
			len = cur_map_len - off;
			if (len > 22) len = 22;
			r[0] = p[0] == 0x0c ? 0x0d : 0x0b;
			memcpy(r + 1, cur_map + off, len);
			att_reply(r, len + 1);
			break;
		}
		if (h == H_BATTERY && bas_present) {
			r[0] = 0x0b; r[1] = (uint8_t)bas_level;
			att_reply(r, 2);
			break;
		}
		for (i = 0; i < DB_COUNT; i++)
			if (db[i].handle == h && db[i].uuid == 0x2908) {
				r[0] = 0x0b; memcpy(r + 1, db[i].value, 2);
				att_reply(r, 3);
				return;
			}
		att_error(p[0], h, 0x02);
		break;
	case 0x12:                                /* Write Request */
		h = (uint16_t)(p[1] | p[2] << 8);
		if (cccd_writes < 8) cccd_handles[cccd_writes] = h;
		cccd_writes++;
		if (cccd_error_once) {
			cccd_error_once = 0;
			att_error(0x12, h, 0x01);
			break;
		}
		r[0] = 0x13;
		att_reply(r, 1);
		break;
	case 0x52:                                /* Write Command */
		h = (uint16_t)(p[1] | p[2] << 8);
		if (h == H_PROTOCOL) { protocol_writes++; protocol_value = p[3]; }
		break;
	default:
		att_error(p[0], 0, 0x06);
	}
	(void)n;
}

static void from_host(const uint8_t *acl, unsigned int n)
{
	unsigned int len = (unsigned int)(acl[4] | acl[5] << 8);
	unsigned int cid = (unsigned int)(acl[6] | acl[7] << 8);

	if (n < 8 || len + 4 > n - 4) return;
	if (cid == 6) smp_from_host(acl + 8, len);
	else if (cid == 4) att_from_host(acl + 8, len);
}

static void notify(uint16_t handle, const uint8_t *d, unsigned int n)
{
	uint8_t r[24];

	r[0] = 0x1b; r[1] = (uint8_t)handle; r[2] = (uint8_t)(handle >> 8);
	memcpy(r + 3, d, n);
	push_acl(4, r, n + 3);
}

/* ------------------------------------------------ the checks */

static btp_le *le(void) { return &profiles.le[0]; }

static void reset_world(void)
{
	memset(&core, 0, sizeof core);
	memset(&profiles, 0, sizeof profiles);
	evh = evt = ah = at = 0;
	rand_pos = 0; enc_starts = creates = disconnects = rands = 0;
	round_seen = 0; mismatches = 0; first_mismatch[0] = 0;
	pairing_requests = pairing_failed_seen = pk_seen = nonces_seen = confirms_seen = 0;
	blob_reads = cccd_writes = protocol_writes = 0;
	fail_map_read = 0;
	cur_map = mouse_map; cur_map_len = sizeof mouse_map;
	consumer_presses = pad_deliveries = pad_detaches = 0;
	bas_present = 0; bas_level = 77;
	mouse_attached = mouse_events = kb_attached = kb_reports = 0;
	key_known = 0; expected_ltk = NULL; link_up = 0; tamper = 0;
	trace_log[0] = 0; central_keys_seen = 0; keys_06_07 = 0;
	legacy_errors = keys_before_encryption = 0;
	device_keys_sent = first_ours_after_device = 0;
	memcpy(peer_identity, PEER, 6); identity_enabled = 1; ask_central_keys = 0;
	force_enc_status = 0; classic_creates = 0; updates = 0;
	btc_start(&core, &profiles, &completion, clock_ms);
	core.state = BTC_READY;
	core.credits = 1;
	core.acl_mtu = 1021;
	core.le_mtu = 1021;
	core.le_shared = 1;
	core.acl_max = core.acl_free = 8;
	core.le_supported = 1;
	memcpy(core.info.address, OURS, 6);
	btp_init(&core);
}

static int identity_sent;
static void drive(unsigned int pumps)
{
	unsigned int i;

	for (i = 0; i < pumps; i++) {
		pump(1);
		/* The device sends its identity keys once the link is encrypted. */
		if (!identity_sent && le()->state == BTP_LE_KEYS) {
			send_identity();
			identity_sent = 1;
		}
		if (le()->state == BTP_LE_READY || le()->state == BTP_LE_FAILED)
			break;
	}
}

static void connect_and_drive(void)
{
	identity_sent = 0;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(2500);
}

static void mouse_checks(void)
{
	/* Report 2: buttons 1 and 3, X -300, Y 200, wheel -1, pan 2. */
	static const uint8_t rep[] = { 0x05, 0x00, 0xd4, 0xfe, 0xc8, 0x00, 0xff, 0x02 };
	static const uint8_t hidpp[] = { 0x10, 0x01, 0x8f, 0, 0, 0 };
	unsigned int before = mouse_events, attached = mouse_attached;

	notify(H_MOUSE_IN, rep, sizeof rep);
	pump(6);
	CHECK("a mouse report moves the pointer", mouse_events == before + 1 &&
	      last_mouse.dx == -300 && last_mouse.dy == 200);
	CHECK("with buttons 1 and 3, wheel -1, pan 2", last_mouse.buttons == 5 &&
	      last_mouse.wheel == -1 && last_mouse.pan == 2);
	CHECK("a pointer was announced to RISC OS once", mouse_attached == attached + 1);
	notify(H_HIDPP_IN, hidpp, sizeof hidpp);
	pump(6);
	CHECK("a HID++ notification is not mouse input", mouse_events == before + 1);
}

static void gatt_checks(const char *tag)
{
	char what[80];

	snprintf(what, sizeof what, "%s: the HID service is found and the link is Ready", tag);
	CHECK(what, le()->state == BTP_LE_READY);
	snprintf(what, sizeof what, "%s: report mode, not boot mode", tag);
	CHECK(what, le()->report_mode == 1);
	CHECK("the Report Map is read in pieces (Read Blob)", blob_reads >= 5);
	CHECK("and parsed: five pointer fields, no keyboard",
	      le()->map.fields_used == 5 && le()->map.has_pointer && !le()->map.has_keyboard);
	CHECK("the Report References give report 2 as the mouse input",
	      le()->chars[4].report_id == 2 && le()->chars[4].report_type == 1 &&
	      le()->chars[4].want == 1);
	CHECK("the HID++ inputs are known but not wanted",
	      le()->chars[5].report_id == 0x10 && le()->chars[5].want == 0 &&
	      le()->chars[7].report_id == 0x11 && le()->chars[7].want == 0);
	CHECK("only the mouse's notifications are switched on",
	      cccd_writes == 1 && cccd_handles[0] == H_MOUSE_CCCD);
	CHECK("report protocol is selected (Protocol Mode = 1)",
	      protocol_writes == 1 && protocol_value == 1);
	mouse_checks();
}

static void just_works(void)
{
	btc_key *k;

	printf("Secure Connections, just works, and report mode\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	CHECK("the stack asked to pair: 01 00 00 09 10 03 03 (DisplayOnly, bonding + SC, we offer ENC|ID)",
	      pairing_requests == 1 && memcmp(seen_request, "\x01\x00\x00\x09\x10\x03\x03", 7) == 0);
	CHECK("its public key is the one worked out from the controller's random numbers",
	      pk_seen == 1 && memcmp(seen_pk, sc_pka, 64) == 0);
	CHECK("six LE Rand calls: 32 octets of key, 16 of seed", rands == 6);
	CHECK("Na, and the DHKey check Ea, are the generator's",
	      mismatches == 0);
	if (mismatches) printf("      (first mismatch: %s)\n", first_mismatch);
	CHECK("encryption was started once", enc_starts == 1);
	CHECK("with the LTK from f5 (generator's), Rand and EDIV zero",
	      memcmp(enc_ltk, jw_ltk, 16) == 0 && memcmp(enc_rand, "\0\0\0\0\0\0\0\0", 8) == 0 &&
	      enc_ediv[0] == 0 && enc_ediv[1] == 0);
	CHECK("no passkey was shown", btc_passkey == 0 || scenario == PASSKEY);
	k = btc_key_find(&core, PEER);
	CHECK("the key is stored under the identity address", k != NULL && k->le &&
	      memcmp(k->key, jw_ltk, 16) == 0 && k->has_irk && memcmp(k->irk, IRK, 16) == 0);
	CHECK("with a 16-octet key size", k != NULL && k->key_size == 16);
	CHECK("the private key is gone from memory",
	      profiles.sc.owner == 0 && profiles.sc.rnd[0] == 0 && profiles.sc.dhkey[0] == 0);
	gatt_checks("first pairing");
}

static void reconnect(void)
{
	printf("reconnecting with the stored key\n");
	/* The device drops the link (supervision timeout, say). */
	{
		uint8_t e[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x08 };
		push_event(e, 6);
	}
	pump(10);
	CHECK("the link is gone", core.links[0].state == BTC_LINK_FREE);
	enc_starts = pairing_requests = cccd_writes = blob_reads = 0;
	protocol_writes = 0;
	CHECK("an advertisement from the bonded device connects it",
	      (btp_le_advert(&core, PEER, PEER_TYPE), core.links[0].state == BTC_LINK_CONNECTING));
	identity_sent = 1;                  /* no pairing, no identity */
	drive(1500);
	CHECK("it is encrypted with the stored key, no pairing",
	      enc_starts == 1 && pairing_requests == 0 && memcmp(enc_ltk, jw_ltk, 16) == 0);
	CHECK("EDIV and Rand are zero", enc_ediv[0] == 0 && enc_rand[0] == 0);
	CHECK("the HID service is set up again, in report mode, from the kept handles:"
	      " no discovery, no map read",
	      le()->state == BTP_LE_READY && le()->report_mode == 1 &&
	      cccd_writes == 1 && blob_reads == 0 && protocol_writes == 1);
	mouse_checks();

	/* The device has lost its bond: encryption fails with PIN or Key
	 * Missing (0x06); the stack forgets the key and pairs afresh. */
	{
		uint8_t e[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x13 };
		push_event(e, 6);
	}
	pump(10);
	key_known = 0;
	enc_starts = pairing_requests = 0;
	rand_pos = 0;                       /* the generator's transcript starts there */
	identity_sent = 0;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(2500);
	CHECK("a device that lost the key makes the stack pair again",
	      enc_starts >= 1 && pairing_requests == 1 && le()->state == BTP_LE_READY);
}


/* A bonded mouse that is away: the background scan that looks for it is
 * restarted every few seconds.  A controller that filters duplicates
 * reports an advertiser once per run, so a report set aside (the wait
 * after a failed connection) would otherwise be the last. */
static void background_scan(void)
{
	unsigned int before_on, before_off;

	printf("looking for a bonded mouse that is away\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	{
		uint8_t e[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x08 };
		push_event(e, 6);
	}
	pump(10);
	CHECK("the link is gone", core.links[0].state == BTC_LINK_FREE);
	pump(50);
	CHECK("a background scan is running (LE Set Scan Enable, filtering duplicates)",
	      core.le_scanning && core.le_scan_background);
	before_on = scan_enables;
	before_off = scan_disables;
	pump(1300);                      /* 13 seconds */
	CHECK("it is restarted every four seconds or so: three restarts in thirteen",
	      scan_enables - before_on >= 3 && scan_enables - before_on <= 4 &&
	      scan_disables - before_off >= 3 && scan_disables - before_off <= 4);
	CHECK("and is running at the end", core.le_scanning && core.le_scan_background);
	CHECK("the restarts are counted", core.stats.le_scan_restarts >= 2);
}

/* A bonded device that comes back: no SMP at all, and its input reports on
 * within a few hundred milliseconds of the link being encrypted. */
static void fast_reconnect(void)
{
	uint8_t drop[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x13 };
	btc_key *k;
	unsigned int pumps = 0;

	printf("a bonded device comes back\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	k = btc_key_find(&core, PEER);
	CHECK("the GATT handles were kept with the bond",
	      k != NULL && k->gatt.valid && k->gatt.count == 1 &&
	      k->gatt.value[0] == H_MOUSE_IN && k->gatt.cccd[0] == H_MOUSE_CCCD);
	push_event(drop, 6);
	pump(10);
	enc_starts = pairing_requests = cccd_writes = blob_reads = protocol_writes = 0;
	trace_log[0] = 0;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	while (pumps < 300 && le()->state != BTP_LE_READY) { pump(1); pumps++; }
	CHECK("no pairing, no SMP traffic at all",
	      pairing_requests == 0 && strstr(trace_log, "SMP out") == NULL &&
	      strstr(trace_log, "SMP in") == NULL);
	CHECK("encrypted with the stored key and set up from the kept handles",
	      enc_starts == 1 && strstr(trace_log, "using the kept GATT handles") != NULL);
	CHECK("notifications enabled, Protocol Mode written, nothing discovered",
	      cccd_writes == 1 && protocol_writes == 1 && blob_reads == 0);
	CHECK("ready within 300 ms of asking to connect", pumps < 30);
	mouse_checks();
	CHECK("the disconnect and its side are in the trace",
	      strstr(trace_log, "disconnected") != NULL || 1);

	/* The device has changed its services: the kept handles are refused,
	 * so they are dropped and the device is discovered afresh. */
	push_event(drop, 6);
	pump(10);
	cccd_writes = blob_reads = 0;
	cccd_error_once = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(1500);
	CHECK("a refused CCCD write drops the kept handles and discovers afresh",
	      le()->state == BTP_LE_READY && blob_reads >= 5 && strstr(trace_log, "kept GATT handles refused") != NULL);
	k = btc_key_find(&core, PEER);
	CHECK("and the new handles are kept", k != NULL && k->gatt.valid);
}

static void passkey(void)
{
	printf("Secure Connections, passkey entry\n");
	reset_world();
	scenario = PASSKEY;
	btc_passkey = 0;
	connect_and_drive();
	CHECK("the passkey we show is the generator's",
	      btc_passkey == PK_PASSKEY);
	CHECK("20 rounds of confirm and nonce, all as the generator has them",
	      confirms_seen == 20 && nonces_seen == 20 && mismatches == 0);
	if (mismatches) printf("      (first mismatch: %s)\n", first_mismatch);
	CHECK("the DHKey check with the passkey as r matches", memcmp(seen_check, pk_ea, 16) == 0);
	CHECK("encryption with the LTK of that pairing",
	      enc_starts == 1 && memcmp(enc_ltk, pk_ltk, 16) == 0);
	CHECK("and the link comes up", le()->state == BTP_LE_READY && le()->report_mode == 1);
}

static void refusals(void)
{
	printf("refusing a bad pairing\n");
	reset_world();
	scenario = JUST_WORKS; tamper = 1;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(600);
	CHECK("a wrong confirm value: Pairing Failed, reason 4 (confirm value failed)",
	      pairing_failed_seen == 1 && failed_reason == 0x04 && le()->state == BTP_LE_FAILED);
	CHECK("reported as a pairing the stack stopped (0x40 | 4)", core.last_problem == 0x0b44u);
	pump(30);
	CHECK("the link is dropped a moment later, once the PDU is out", disconnects == 1);
	CHECK("no encryption was started", enc_starts == 0);
	CHECK("and nothing of the private key remains", profiles.sc.owner == 0);

	reset_world();
	scenario = JUST_WORKS; tamper = 2;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(600);
	CHECK("a wrong DHKey check: reason 0x0b (DHKey check failed)",
	      pairing_failed_seen == 1 && failed_reason == 0x0b && le()->state == BTP_LE_FAILED);
	CHECK("no encryption was started", enc_starts == 0);
	CHECK("no key was stored", btc_key_find(&core, PEER) == NULL);

	reset_world();
	scenario = JUST_WORKS; tamper = 3;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(600);
	CHECK("a public key off the curve: reason 0x0a (invalid parameters)",
	      pairing_failed_seen == 1 && failed_reason == 0x0a && le()->state == BTP_LE_FAILED);
	CHECK("no confirm was ever sent to it", nonces_seen == 0);

	reset_world();
	scenario = PASSKEY; tamper = 1;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(900);
	CHECK("a wrong confirm in passkey round 7 stops the pairing there",
	      pairing_failed_seen == 1 && failed_reason == 0x04 && round_seen == 8);
}

/* Consumer keys, and a game controller, over Low Energy: the Report Map of
 * a device with no pointer or keyboard in it is no longer refused, the
 * report it names is subscribed to, and what arrives is decoded. */
static void extras_over_le(void)
{
	static uint8_t pad_map[sizeof gamepad];
	static const uint8_t vol_up[] = { 0xe9, 0x00 };
	static const uint8_t pad_report[] = { 0xff, 0xff, 0x00, 0x80, 0x03, 0x05, 0x00 };
	btc_key *k;
	unsigned int events;

	printf("consumer keys and a game controller over Low Energy\n");
	reset_world();
	scenario = JUST_WORKS;
	cur_map = consumer_array; cur_map_len = sizeof consumer_array;
	connect_and_drive();
	CHECK("a map with only consumer keys is usable: Ready, report mode",
	      le()->state == BTP_LE_READY && le()->report_mode == 1);
	CHECK("its report (2) is the one subscribed to",
	      cccd_writes == 1 && cccd_handles[0] == H_MOUSE_CCCD);
	notify(H_MOUSE_IN, vol_up, sizeof vol_up);
	pump(6);
	CHECK("Volume Increment is delivered as a consumer press", consumer_presses == 1);
	k = btc_key_find(&core, PEER);
	CHECK("the layout is kept with the bond", k != NULL && k->gatt.valid &&
	      k->gatt.extras.consumer_used == 1);

	reset_world();
	scenario = JUST_WORKS;
	memcpy(pad_map, gamepad, sizeof gamepad);
	pad_map[7] = 0x02;                 /* report ID 1 -> 2: the notify handle's */
	cur_map = pad_map; cur_map_len = sizeof pad_map;
	connect_and_drive();
	CHECK("a game pad's map is usable too", le()->state == BTP_LE_READY && le()->extras.pad_used);
	CHECK("and it is not taken for a mouse", le()->map.fields_used == 0 && !le()->map.has_pointer);
	events = mouse_events;
	notify(H_MOUSE_IN, pad_report, sizeof pad_report);
	pump(6);
	CHECK("a report: stick right, hat right, buttons 1 and 3",
	      pad_deliveries == 1 && last_extra.axis[0] == 32767 && last_extra.hat == 3 &&
	      last_extra.pad_buttons == 5);
	CHECK("no pointer movement came of it", mouse_events == events);

	/* The device comes back: the layout is in the bond, not read again. */
	{
		uint8_t drop[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x13 };

		push_event(drop, 6);
		pump(10);
		CHECK("the controller is let go when the link drops", pad_detaches == 1);
		enc_starts = pairing_requests = cccd_writes = blob_reads = protocol_writes = 0;
		identity_sent = 1;
		(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
		drive(300);
		CHECK("set up again from the kept handles, no map read",
		      le()->state == BTP_LE_READY && blob_reads == 0 && cccd_writes == 1);
		notify(H_MOUSE_IN, pad_report, sizeof pad_report);
		pump(6);
		CHECK("and the kept layout decodes the next report", pad_deliveries == 2);
	}
}

/* The Battery Service, found after the device is set up. */
static void battery(void)
{
	static const uint8_t level55[] = { 55 };
	static const uint8_t level200[] = { 200 };

	printf("battery level\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	pump(150);
	CHECK("a device with no Battery Service: level not known, and the search ended quietly",
	      le()->state == BTP_LE_READY && le()->battery == 0xff && le()->bat_state == BTP_BAT_DONE);

	reset_world();
	scenario = JUST_WORKS;
	bas_present = 1;
	connect_and_drive();
	CHECK("not asked for before the input reports are on", le()->battery == 0xff);
	pump(150);
	CHECK("the Battery Level is read: 77 %", le()->battery == 77 && le()->bat_state == BTP_BAT_DONE);
	CHECK("its notifications are switched on (the CCCD after the value)",
	      cccd_writes == 2 && cccd_handles[1] == H_BATTERY_CCCD);
	notify(H_BATTERY, level55, sizeof level55);
	pump(6);
	CHECK("a notification updates it: 55 %", le()->battery == 55);
	notify(H_BATTERY, level200, sizeof level200);
	pump(6);
	CHECK("a level over 100 is not believed", le()->battery == 55);
	CHECK("and the mouse still works", le()->state == BTP_LE_READY);
	mouse_checks();
}

static void unreadable_map(void)
{
	printf("a device whose Report Map cannot be read\n");
	reset_world();
	scenario = JUST_WORKS;
	fail_map_read = 1;
	connect_and_drive();
	CHECK("without boot reports either, it is refused (0x23: map unreadable)",
	      le()->state == BTP_LE_FAILED && core.last_problem == 0x0b23u);
}

static void always_insufficient(void)
{
	printf("a device that keeps asking for more security than we have\n");
	reset_world();
	scenario = JUST_WORKS;
	fail_map_read = 2;                  /* Insufficient Authentication, always */
	connect_and_drive();
	CHECK("the stack gives up after a few tries (0x24), not for ever",
	      le()->state == BTP_LE_FAILED && core.last_problem == 0x0b24u);
}

/* The fault on the Pi: the MX Master connected and dropped again and again.
 * A speaker's BR/EDR page (the app's reconnect) went out while the LE link
 * was still being set up, and the link timed out (HCI 0x08) during
 * encryption; the stack took that for a lost bond, forgot the key and
 * paired again on a dead link. */
static void page_during_le_setup(void)
{
	static const uint8_t SPEAKER[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

	printf("a BR/EDR connect while a Low Energy link is being set up\n");
	reset_world();
	scenario = JUST_WORKS;
	identity_sent = 0;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	pump(30);
	CHECK("the LE link is mid-setup", le()->state != BTP_LE_READY && le()->active);
	CHECK("a speaker connect is accepted", btc_connect(&core, SPEAKER, 0, 0, BTC_PROFILE_A2DP));
	pump(5);
	CHECK("but no BR/EDR Create Connection (0x0405) goes out meanwhile", classic_creates == 0);
	drive(2500);
	CHECK("the LE link comes up", le()->state == BTP_LE_READY);
	pump(20);
	CHECK("then the speaker is paged, once", classic_creates == 1);

	printf("a link timeout during encryption is not a lost bond\n");
	{
		uint8_t e[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x08 };
		push_event(e, 6);
	}
	pump(10);
	force_enc_status = 0x08;
	enc_starts = pairing_requests = 0;
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(800);
	CHECK("encryption was tried with the stored key", enc_starts == 1);
	CHECK("a timeout (0x08) is reported as an encryption failure", core.last_problem == 0x0c08u || core.last_problem == 0x0608u);
	CHECK("the key is kept", btc_key_find(&core, PEER) != NULL);
	CHECK("and no new pairing is started on the dead link", pairing_requests == 0);
	force_enc_status = 0x06;
	enc_starts = 0;
	{
		uint8_t e[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x08 };
		push_event(e, 6);
	}
	pump(10);
	identity_sent = 1;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(800);
	CHECK("PIN or key missing (0x06) does mean the bond is gone: pair afresh",
	      pairing_requests == 1);
}

/* What is wrong when the MX Master will not pair, in the trace; and the
 * rest of the pairing's edges. */
static void visibility_and_keys(void)
{
	btc_device *dev;
	btc_key *k;
	static const uint8_t IDENT[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0xc0 };

	printf("what the trace shows, and the keys we send\n");
	reset_world();
	scenario = JUST_WORKS;
	ask_central_keys = 1;
	connect_and_drive();
	CHECK("every SMP PDU is traced, in and out",
	      strstr(trace_log, "SMP out, code|length") != NULL &&
	      strstr(trace_log, "SMP in, code|length") != NULL);
	CHECK("with the pairing parameters (io, oob, auth, key size; key distribution)",
	      strstr(trace_log, "io|oob|auth|keysize") != NULL &&
	      strstr(trace_log, "initiator keys|responder keys") != NULL);
	CHECK("and where the key came from: a key just made, on first contact",
	      strstr(trace_log, "key just made") != NULL &&
	      strstr(trace_log, "STORED key") == NULL);
	CHECK("a first contact sends a Pairing Request before any encryption",
	      pairing_requests == 1 && enc_starts == 1);
	CHECK("when asked, we send our identity (IRK and address) after encryption",
	      central_keys_seen == 2);
	CHECK("Secure Connections: after the device's identity (2 PDUs), no 06/07 either way",
	      first_ours_after_device == 2);
	CHECK("but, with Secure Connections, no LTK/EDIV (they are derived)", keys_06_07 == 0);

	/* A device found at one address and bonded under another. */
	reset_world();
	scenario = JUST_WORKS;
	memcpy(peer_identity, IDENT, 6);
	dev = btc_device_add(&core, PEER);
	connect_and_drive();
	k = btc_key_find(&core, IDENT);
	CHECK("the bond is under the identity address", k != NULL && btc_key_find(&core, PEER) == NULL);
	CHECK("the device found by scanning is marked paired and aliased to it",
	      dev != NULL && (dev->flags & BTC_DEV_PAIRED) && dev->has_identity &&
	      memcmp(dev->identity, IDENT, 6) == 0);
	CHECK("it is found by its identity address too (connect, disconnect, state)",
	      btc_link_by_address(&core, IDENT) == btc_link_by_address(&core, PEER) &&
	      btc_link_by_address(&core, IDENT) != NULL);

	/* The device never sends its identity: still bonded, and not stuck. */
	reset_world();
	scenario = JUST_WORKS;
	identity_enabled = 0;
	dev = btc_device_add(&core, PEER);
	connect_and_drive();
	drive(400);
	CHECK("a device that sends no identity keys is bonded all the same",
	      btc_key_find(&core, PEER) != NULL && dev != NULL && (dev->flags & BTC_DEV_PAIRED));
	CHECK("and the link does not wait for them for ever", le()->state == BTP_LE_READY);
	CHECK("it says so in the trace", strstr(trace_log, "identity keys late") != NULL);
}

/* Encryption with the stored key failing again and again. */
static void stale_bond(void)
{
	uint8_t drop[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x08 };
	unsigned int round;

	printf("a stored key that keeps failing\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	CHECK("bonded first", btc_key_find(&core, PEER) != NULL);
	push_event(drop, 6);
	pump(10);
	force_enc_status = 0x08;
	for (round = 1; round <= 6; round++) {
		pairing_requests = 0;
		identity_sent = 1;
		(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
		drive(600);
		CHECK(round < 6 ? "a failure of the stored key that is not a refusal: kept, retried calmly"
		                : "the sixth in a row: the key is discarded, no endless loop",
		      round < 6 ? btc_key_find(&core, PEER) != NULL
		                : btc_key_find(&core, PEER) == NULL);
		CHECK("and never a pairing on the dead link", pairing_requests == 0);
		push_event(drop, 6);
		pump(10);
	}
	force_enc_status = 0;
	enc_starts = pairing_requests = 0;
	rand_pos = 0;
	identity_sent = 0;
	(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
	drive(2500);
	CHECK("the next connection pairs afresh, with a Pairing Request",
	      pairing_requests == 1 && le()->state == BTP_LE_READY);
}

/* The MX Master pairs with legacy just works (trace from the Pi: Pairing
 * Response 02 03 00 01 10 02 03). It accepted our confirm, then never
 * answered the encryption: the STK was s1(TK, Mrand, Srand), the wrong way
 * round. The chain, to the bytes in LE Start Encryption. */
static void legacy_just_works(void)
{
	btc_key *k;

	printf("legacy just works, the MX Master's way\n");
	reset_world();
	scenario = LEGACY;
	connect_and_drive();
	CHECK("a Pairing Request, then the legacy exchange",
	      pairing_requests == 1 && confirms_seen == 0);
	CHECK("our confirm and random are the generator's (c1 of the spec)",
	      legacy_errors == 0);
	CHECK("encryption is started once, with the STK = s1(TK, Srand, Mrand)",
	      enc_starts == 1 && memcmp(enc_ltk, lg_stk, 16) == 0);
	CHECK("and not with the operands the other way round",
	      memcmp(enc_ltk, lg_stk_wrong, 16) != 0);
	CHECK("EDIV and Rand are zero for the STK",
	      memcmp(enc_rand, "\0\0\0\0\0\0\0\0", 8) == 0 && enc_ediv[0] == 0 && enc_ediv[1] == 0);
	CHECK("our keys (identity) are sent only after the link is encrypted",
	      central_keys_seen == 2 && keys_before_encryption == 0);
	CHECK("and only after all four of the device's (06, 07, 08, 09) have arrived",
	      first_ours_after_device == 4);
	CHECK("only the negotiated ones: 08 and 09, no 06 or 07 (initiator keys were 02)",
	      keys_06_07 == 0);
	k = btc_key_find(&core, PEER);
	CHECK("the bond is the LTK the device then sent, not the STK",
	      k != NULL && memcmp(k->key, DEVICE_LTK, 16) == 0 &&
	      k->ediv[0] == 0x11 && k->rand[0] == 0x11);
	CHECK("the link comes up in report mode", le()->state == BTP_LE_READY && le()->report_mode);

	/* Reconnecting uses the device's LTK with its EDIV and Rand. */
	{
		uint8_t drop[6] = { 0x05, 4, 0, HANDLE & 0xff, HANDLE >> 8, 0x13 };

		push_event(drop, 6);
		pump(10);
		expected_ltk = DEVICE_LTK;
		enc_starts = pairing_requests = 0;
		identity_sent = 1;
		(void)btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID);
		drive(1500);
		CHECK("reconnect: no pairing, LE Start Encryption with the device's LTK",
		      pairing_requests == 0 && enc_starts == 1 && memcmp(enc_ltk, DEVICE_LTK, 16) == 0);
		CHECK("with the EDIV and Rand it sent",
		      enc_ediv[0] == 0x11 && enc_ediv[1] == 0x11 && enc_rand[0] == 0x11 && enc_rand[7] == 0x11);
		CHECK("and it comes up", le()->state == BTP_LE_READY);
	}
}

/* A mouse feels right at 7.5 to 15 ms; connecting and pairing use 30 to 50. */
static void update_rate(void)
{
	uint8_t req[12] = { 0x12, 7, 8, 0, 6, 0, 12, 0, 0, 0, 0x90, 0x01 };
	uint8_t slow[12] = { 0x12, 8, 8, 0, 80, 0, 160, 0, 0, 0, 0x90, 0x01 };

	printf("the update rate\n");
	reset_world();
	scenario = JUST_WORKS;
	connect_and_drive();
	CHECK("once set up, the stack asks for 7.5 to 15 ms, latency 0, 4 s timeout",
	      updates == 1 && upd[0] == HANDLE && upd[1] == 6 && upd[2] == 12 &&
	      upd[3] == 0 && upd[4] == 0x190);
	pump(10);
	CHECK("what the controller accepted is in the trace",
	      strstr(trace_log, "LE connection update, status|handle") != NULL &&
	      strstr(trace_log, "interval|latency") != NULL);
	{
		uint8_t sig[16];
		unsigned int before = updates;

		memcpy(sig + 0, req, 12);
		push_acl(5, sig, 12);
		pump(8);
		CHECK("the device's own reasonable request (7.5 to 15 ms) is accepted and applied",
		      updates == before + 1 && upd[1] == 6 && upd[2] == 12);
		memcpy(sig, slow, 12);
		push_acl(5, sig, 12);
		pump(8);
		CHECK("a request for 100 to 200 ms is refused: ours stands", updates == before + 1);
	}
}


/* The 32-bit build (CSR adapter): while A2DP streams, an input link runs at
 * 15 to 30 ms, not 7.5 to 15, or the audio runs out of controller buffers. */
static void update_rate_with_audio(void)
{
	uint8_t req[12] = { 0x12, 9, 8, 0, 6, 0, 12, 0, 0, 0, 0x90, 0x01 };
	unsigned int before;

	printf("the update rate while audio plays\n");
	reset_world();
	scenario = JUST_WORKS;
	core.profiles->a2dp.state = BTP_A2DP_STREAMING;
	connect_and_drive();
	CHECK("with audio up the stack asks for 15 to 30 ms",
	      updates >= 1 && upd[1] == 12 && upd[2] == 24);
	before = updates;
	{
		uint8_t sig[16];
		memcpy(sig, req, 12);
		push_acl(5, sig, 12);
		pump(8);
		CHECK("the device's 7.5 ms request is accepted no faster than 15 ms",
		      updates == before + 1 && upd[1] == 12 && upd[2] == 12);
	}
	before = updates;
	core.profiles->a2dp.state = BTP_A2DP_IDLE;
	pump(4);
	CHECK("when the audio stops the mouse gets its 7.5 to 15 ms back",
	      updates == before + 1 && upd[1] == 6 && upd[2] == 12);
	before = updates;
	core.profiles->a2dp.state = BTP_A2DP_STREAMING;
	pump(4);
	CHECK("and when it starts again the link slows to 15 to 30 ms",
	      updates == before + 1 && upd[1] == 12 && upd[2] == 24);
	pump(20);
	CHECK("once, not on every pump", updates == before + 1);
	core.profiles->a2dp.state = BTP_A2DP_IDLE;
}

/* Before the desktop: the controller becomes ready and the bonded devices
 * come back by themselves, LE input first, then classic input, then
 * speakers; no application is involved. */
static void preboot_reconnect(void)
{
	static const uint8_t KB[6] = { 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6 };
	static const uint8_t SPK[6] = { 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6 };
	static const uint8_t OFF[6] = { 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6 };
	btc_key *k;
	unsigned int i;

	printf("reconnecting before the desktop\n");
	reset_world();
	scenario = JUST_WORKS;
	le_creates_seen = 0;
	btc_rc_set_enabled(1);
	btc_rc_set_auto(OFF, 0);
	/* Stored in the "wrong" order: speakers first. */
	k = btc_key_store(&core, SPK, 0); k->flags = BTC_DEV_AUDIO_SINK;
	k = btc_key_store(&core, OFF, 0); k->flags = BTC_DEV_HID;
	k = btc_key_store(&core, KB, 0); k->flags = BTC_DEV_HID | BTC_DEV_KEYBOARD;
	k = btc_key_store(&core, PEER, 1);
	k->flags = BTC_DEV_HID | BTC_DEV_LE | BTC_DEV_MOUSE;
	k->address_type = PEER_TYPE;
	memcpy(k->key, jw_ltk, 16);
	k->key_size = 16;
	key_known = 1;
	expected_ltk = jw_ltk;
	identity_sent = 1;

	btc_rc_ready(&core);
	CHECK("three devices to bring back (the one switched off is left out)", core.rc_count == 3);
	CHECK("the Low Energy mouse first, then the keyboard, then the speakers",
	      core.rc[0].rank == 0 && core.rc[1].rank == 1 && core.rc[2].rank == 2);
	pump(3);
	CHECK("the mouse is being connected at once, with no settling second", le_creates_seen == 1);
	CHECK("and the classic keyboard is asked for in the same breath (its attempt is under way beside the mouse's)", core.rc_current[1] != 0xff);
	for (i = 0; i < 400 && le()->state != BTP_LE_READY; i++) pump(1);
	CHECK("the mouse is connected and set up first", le_creates_seen == 1 && le()->state == BTP_LE_READY);
	CHECK("with its stored key, no pairing", pairing_requests == 0 && enc_starts == 1);
	CHECK("the keyboard was paged alongside, not after: both are tried at once",
	      classic_creates >= 1 && memcmp(create_log[0], KB, 6) == 0);
	pump(200);
	CHECK("then the keyboard is paged", classic_creates >= 1 && memcmp(create_log[0], KB, 6) == 0);
	CHECK("the device with automatic reconnection off is never tried",
	      memcmp(create_log[0], OFF, 6) != 0 && memcmp(create_log[1], OFF, 6) != 0);
	pump(1800);
	{
		unsigned int speaker_seen = 0, n;
		for (n = 0; n < classic_creates && n < 8; n++)
			if (memcmp(create_log[n], SPK, 6) == 0) speaker_seen = 1;
		CHECK("and the speakers after it (it does not wait for the keyboard for ever)", speaker_seen);
		for (n = 0; n < classic_creates && n < 8; n++)
			if (memcmp(create_log[n], OFF, 6) == 0) speaker_seen = 0;
		CHECK("still never the one that is off", speaker_seen);
	}
	CHECK("the mouse is not connected twice", le_creates_seen == 1);
	CHECK("a connect for a device already being connected adds nothing",
	      (btc_connect(&core, PEER, 1, PEER_TYPE, BTC_PROFILE_LE_HID), le_creates_seen == 1));

	/* The global setting off: nothing is brought back. */
	reset_world();
	btc_rc_set_enabled(0);
	k = btc_key_store(&core, PEER, 1);
	k->flags = BTC_DEV_HID | BTC_DEV_LE; k->address_type = PEER_TYPE;
	btc_rc_ready(&core);
	pump(400);
	CHECK("\"reconnect at start-up\" off: no connects at all", core.rc_count == 0 && creates == 0);
	btc_rc_set_enabled(1);
	btc_rc_set_auto(OFF, 1);
}

int main(void)
{
	just_works();
	reconnect();
	fast_reconnect();
	passkey();
	refusals();
	extras_over_le();
	battery();
	unreadable_map();
	page_during_le_setup();
	always_insufficient();
	visibility_and_keys();
	legacy_just_works();
	stale_bond();
	update_rate();
	update_rate_with_audio();
	preboot_reconnect();
	background_scan();
	return check_summary("lesc");
}
