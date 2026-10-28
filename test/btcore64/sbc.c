/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for BluetoothVoice's SBC encoder (c/sbc_encoder.c).
 *
 * What is established, and how:
 *
 *  1. Structure, written here from the A2DP 1.3 specification (Appendix B,
 *     "SBC": the frame header, the frame length formula, the CRC-8 with
 *     generator x^8+x^4+x^3+x^2+1 and register preset 0x0F over the
 *     header octets 1 and 2, the joint stereo flags and the scale
 *     factors): every frame of every configuration has the syncword, the
 *     header the configuration asks for, the length the formula gives and
 *     a CRC that this file recomputes by its own code.
 *  2. Content.  The encoder's output for a fixed integer test signal was
 *     decoded by an independent decoder (FFmpeg's "sbc" decoder, run by
 *     hand on the build machine: nothing of FFmpeg is in this tree) and
 *     compared with the input after removing the codec delay.  The
 *     measured signal-to-noise ratios are in the table below; the delay
 *     found was 73 samples for 8 subbands and 37 for 4.
 *     The checksum of each configuration's stream is pinned here, so a
 *     change of the encoder that alters one octet is noticed; to check a
 *     new checksum again, run this program with a directory name as its
 *     argument: it writes <name>.sbc (the stream) and <name>.pcm (the
 *     input, 16-bit stereo) for an external decoder.
 *
 * No test vectors from the Bluetooth SIG were available to this project
 * (the A2DP conformance streams are for members); the specification
 * itself has none for the encoder, only the algorithm.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "sbc_encoder.h"

/* A deterministic tone from integers only: a parabolic sine. phase is
 * 0..65535 for a whole cycle; the result is -32767..32767 (about 1 % off
 * a true sine, which does not matter here, and cannot differ between
 * compilers or libraries). */
static int tone(uint32_t phase)
{
	int x = (int)(phase & 0xffffu);
	int s = 1;
	int y;

	if (x >= 32768) { x -= 32768; s = -1; }
	/* 0..32768 -> 4x(1 - x/32768) scaled: y = x * (32768 - x) / 8192 * ... */
	y = (int)(((int64_t)x * (32768 - x)) >> 15);	/* 0..8192 */
	y = y * 4;					/* 0..32768 */
	if (y > 32767) y = 32767;
	return s * y;
}

static void signal_frames(int16_t *pcm, unsigned int frames,
			  unsigned long *t, uint32_t rate)
{
	unsigned int i;

	for (i = 0; i < frames; i++, (*t)++) {
		uint32_t pa = (uint32_t)(((uint64_t)*t * 1000u * 65536u) / rate);
		uint32_t pb = (uint32_t)(((uint64_t)*t * 5300u * 65536u) / rate);
		uint32_t pc = (uint32_t)(((uint64_t)*t * 1500u * 65536u) / rate);
		uint32_t pd = (uint32_t)(((uint64_t)*t * 7700u * 65536u) / rate);

		pcm[2 * i] = (int16_t)((tone(pa) * 11 + tone(pb) * 6) / 20);
		pcm[2 * i + 1] = (int16_t)((tone(pc) * 11 + tone(pd) * 5) / 20);
	}
}

struct cfg {
	const char *name;
	uint32_t rate;
	uint8_t mode, blocks, subbands, allocation, bitpool;
	uint32_t checksum;	/* FNV-1a of the stream of 100 frames */
	unsigned int snr_db;	/* measured with the external decoder (whole dB): a record */
};

#define FREQ(r) ((r) == 16000 ? 0 : (r) == 32000 ? 1 : (r) == 44100 ? 2 : 3)

static const struct cfg cfgs[] = {
	/* A checksum of 0 means "not pinned yet": main prints what the
	 * encoder gives, to be checked with an external decoder first. The
	 * last column is the signal-to-noise ratio that decoder measured
	 * for 100 frames of the test signal after the codec delay (73
	 * samples for 8 subbands, 37 for 4). */
	{ "joint_44k_16_8_loud_35", 44100, 3, 16, 8, 0, 35, 0xcb41cda7u, 28 },
	{ "stereo_44k_16_8_loud_53", 44100, 2, 16, 8, 0, 53, 0xc3c81f6fu, 38 },
	{ "joint_48k_16_8_snr_35", 48000, 3, 16, 8, 1, 35, 0x706d258eu, 33 },
	{ "joint_44k_8_4_loud_32", 44100, 3, 8, 4, 0, 32, 0x0219feadu, 36 },
	{ "stereo_48k_12_8_loud_40", 48000, 2, 12, 8, 0, 40, 0x4d35c0f0u, 32 },
	{ "joint_32k_4_4_snr_20", 32000, 3, 4, 4, 1, 20, 0x77274fb1u, 23 },
	{ "joint_16k_16_8_loud_35", 16000, 3, 16, 8, 0, 35, 0x5b1bd940u, 25 },
	{ "dual_44k_16_8_loud_40", 44100, 1, 16, 8, 0, 40, 0xde4e30bfu, 46 },
};

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
	while (n--) h = (h ^ *p++) * 16777619u;
	return h;
}

/* CRC-8 over `bits` bits taken from the octets in `data`, MSB first,
 * generator 0x1D (x^8+x^4+x^3+x^2+1), preset 0x0F. */
static unsigned int crc8(const uint8_t *data, unsigned int bits)
{
	unsigned int crc = 0x0f, i;

	for (i = 0; i < bits; i++) {
		unsigned int in = (data[i >> 3] >> (7 - (i & 7))) & 1u;
		unsigned int top = (crc >> 7) & 1u;

		crc = (crc << 1) & 0xffu;
		if (in ^ top)
			crc ^= 0x1du;
	}
	return crc;
}

/* The spec's frame length: header 4 octets, scale factors 4 bits per
 * subband per channel, then the samples (and, for joint stereo, one
 * join flag per subband), rounded up to whole octets. */
static unsigned int frame_length(const struct cfg *c)
{
	unsigned int ch = c->mode == 0 ? 1 : 2;
	unsigned int bits = c->blocks * (c->mode == 1 ? ch : 1) * c->bitpool;
	unsigned int len;

	if (c->mode == 0 || c->mode == 1)
		bits = c->blocks * ch * c->bitpool;
	else if (c->mode == 2)
		bits = c->blocks * c->bitpool;
	else
		bits = c->subbands + c->blocks * c->bitpool;
	len = 4 + (4 * c->subbands * ch) / 8 + (bits + 7) / 8;
	return len;
}

static int frame_ok(const struct cfg *c, const uint8_t *f, unsigned int len)
{
	unsigned int ch = c->mode == 0 ? 1 : 2;
	uint8_t crcbits[40];
	unsigned int n = 0, i, total = 16, header_join = 0;
	unsigned int blocks_code = c->blocks == 4 ? 0 : c->blocks == 8 ? 1 :
				   c->blocks == 12 ? 2 : 3;

	if (len < 4 || f[0] != 0x9c)
		return 0;
	if (((f[1] >> 6) & 3u) != (unsigned)FREQ(c->rate)) return 0;
	if (((f[1] >> 4) & 3u) != blocks_code) return 0;
	if (((f[1] >> 2) & 3u) != c->mode) return 0;
	if (((f[1] >> 1) & 1u) != c->allocation) return 0;
	if ((f[1] & 1u) != (c->subbands == 8)) return 0;
	if (f[2] != c->bitpool) return 0;
	/* CRC input: octets 1 and 2, the join flags, the scale factors. */
	crcbits[n++] = f[1];
	crcbits[n++] = f[2];
	if (c->mode == 3) {
		/* One bit per subband, the last of them always 0 when 8:
		 * the flags occupy `subbands` bits from octet 4. */
		header_join = c->subbands;
	}
	for (i = 0; i < (header_join + 4 * c->subbands * ch + 7) / 8; i++)
		crcbits[n++] = f[4 + i];
	total += header_join + 4 * c->subbands * ch;
	/* The partial last octet of the join flags and scale factors holds
	 * the bits in its top; the rest are not covered. */
	if ((total & 7) != 0)
		crcbits[n - 1] &= (uint8_t)(0xffu << (8 - (total & 7)));
	return crc8(crcbits, total) == f[3];
}


/* The adaptive bitpool changes the bitpool mid-stream: the frames after the
 * change must match an encoder that ran at the new bitpool all along (the
 * analysis filter's history kept), or every change clicks. */
static void bitpool_change_keeps_history(void)
{
	btv_sbc_config config;
	btv_sbc_encoder a, b;
	int16_t pcm[2 * 128];
	uint8_t fa[BTV_SBC_MAX_FRAME_BYTES], fb[BTV_SBC_MAX_FRAME_BYTES];
	unsigned long ta = 0, tb = 0;
	unsigned int f, same = 0, n;
	size_t la, lb;

	memset(&config, 0, sizeof config);
	config.sample_rate = 44100; config.channel_mode = BTV_SBC_JOINT_STEREO;
	config.blocks = 16; config.subbands = 8; config.allocation = 0;
	config.bitpool = 18;
	CHECK("encoder A at bitpool 18", btv_sbc_init(&a, &config));
	config.bitpool = 15;
	CHECK("encoder B at bitpool 15", btv_sbc_init(&b, &config));
	n = btv_sbc_frame_samples(&a);
	for (f = 0; f < 40; f++) {
		if (f == 20)
			CHECK("A changes to bitpool 15 mid-stream", btv_sbc_set_bitpool(&a, 15));
		signal_frames(pcm, n, &ta, 44100);
		la = btv_sbc_encode(&a, pcm, fa, sizeof fa);
		signal_frames(pcm, n, &tb, 44100);
		lb = btv_sbc_encode(&b, pcm, fb, sizeof fb);
		if (f >= 20 && la == lb && memcmp(fa, fb, la) == 0) ++same;
	}
	CHECK("after the change A's frames are B's exactly (history kept)", same == 20);
	CHECK("and the frame length follows the bitpool", btv_sbc_frame_bytes(&a) == btv_sbc_frame_bytes(&b));
}

int main(int argc, char **argv)
{
	unsigned int ci;

	for (ci = 0; ci < sizeof cfgs / sizeof cfgs[0]; ci++) {
		const struct cfg *c = &cfgs[ci];
		btv_sbc_config config;
		btv_sbc_encoder enc;
		int16_t pcm[2 * 128];
		uint8_t out[BTV_SBC_MAX_FRAME_BYTES];
		unsigned long t = 0;
		uint32_t h = 2166136261u;
		unsigned int f, bad_len = 0, bad_frame = 0, n, want;
		FILE *sbc = NULL, *raw = NULL;
		char path[256];
		char what[160];

		memset(&config, 0, sizeof config);
		config.sample_rate = c->rate;
		config.channel_mode = c->mode;
		config.blocks = c->blocks;
		config.subbands = c->subbands;
		config.allocation = c->allocation;
		config.bitpool = c->bitpool;
		if (!btv_sbc_init(&enc, &config)) {
			snprintf(what, sizeof what, "%s: encoder accepts the configuration", c->name);
			CHECK(what, 0);
			continue;
		}
		n = btv_sbc_frame_samples(&enc);
		want = frame_length(c);
		if (argc > 1) {
			snprintf(path, sizeof path, "%s/%s.sbc", argv[1], c->name);
			sbc = fopen(path, "wb");
			snprintf(path, sizeof path, "%s/%s.pcm", argv[1], c->name);
			raw = fopen(path, "wb");
		}
		for (f = 0; f < 100; f++) {
			size_t len;

			signal_frames(pcm, n, &t, c->rate);
			len = btv_sbc_encode(&enc, pcm, out, sizeof out);
			if (len != want) bad_len++;
			if (!frame_ok(c, out, (unsigned int)len)) bad_frame++;
			h = fnv(h, out, len);
			if (sbc) fwrite(out, 1, len, sbc);
			if (raw) fwrite(pcm, 4, n, raw);
		}
		if (sbc) fclose(sbc);
		if (raw) fclose(raw);
		snprintf(what, sizeof what, "%s: every frame has the length the specification's formula gives (%u octets)", c->name, want);
		CHECK(what, bad_len == 0 && btv_sbc_frame_bytes(&enc) == want);
		snprintf(what, sizeof what, "%s: syncword, header fields and CRC-8 of every frame as the specification has them", c->name);
		CHECK(what, bad_frame == 0);
		if (c->checksum != 0) {
			snprintf(what, sizeof what, "%s: stream checksum is the one verified with an external decoder", c->name);
			CHECK(what, h == c->checksum);
		} else
			printf("  checksum %s = 0x%08xu\n", c->name, h);
	}
	bitpool_change_keeps_history();
	return check_summary("sbc");
}
