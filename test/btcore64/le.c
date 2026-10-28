/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/*
 * Host checks for the LE Secure Connections toolbox (modules/network/
 * bluetooth/c/le_crypto.c and p256.c).
 *
 * Sources: the sample data of Bluetooth Core Spec Vol 3 Part H Appendix D
 * (D.1 key pair, D.2 f4, D.3 f5, D.4 f6, D.5 g2, D.6 h6, D.7 ah, c1, s1),
 * FIPS-197 and RFC 4493 for AES and AES-CMAC. Every spec value here was also
 * recomputed with an independent implementation (Python's cryptography
 * package over OpenSSL) before it was written down; the P-256 vectors below
 * were produced by OpenSSL, not by this code.
 *
 * The spec writes values most significant octet first; the toolbox takes
 * them least significant first, as SMP carries them, so the helpers reverse.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "le_crypto.h"
#include "p256.h"

static unsigned int unhex(char c)
{
	return c <= '9' ? (unsigned int)(c - '0') : (unsigned int)((c | 0x20) - 'a' + 10);
}

/* hex string -> bytes in the order written (MSB first). */
static unsigned int hex(const char *s, uint8_t *out)
{
	unsigned int n = 0;

	while (s[0] != 0 && s[1] != 0) {
		if (s[0] == ' ') { s++; continue; }
		out[n++] = (uint8_t)(unhex(s[0]) << 4 | unhex(s[1]));
		s += 2;
	}
	return n;
}

/* hex string written MSB first -> LSB-first bytes. */
static unsigned int lsb(const char *s, uint8_t *out)
{
	uint8_t t[64];
	unsigned int n = hex(s, t), i;

	for (i = 0; i < n; i++)
		out[i] = t[n - 1 - i];
	return n;
}

static int same_msb(const uint8_t *got_lsb, const char *want_msb, unsigned int n)
{
	uint8_t w[64];

	(void)lsb(want_msb, w);
	return memcmp(got_lsb, w, n) == 0;
}

static void aes_and_cmac(void)
{
	uint8_t key[16], in[64], out[16], want[16];
	unsigned int n;

	printf("AES and AES-CMAC\n");
	hex("000102030405060708090a0b0c0d0e0f", key);
	hex("00112233445566778899aabbccddeeff", in);
	btv_aes128_encrypt(key, in, out);
	hex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
	CHECK("AES-128 matches FIPS-197 appendix C.1", memcmp(out, want, 16) == 0);

	hex("2b7e151628aed2a6abf7158809cf4f3c", key);
	btv_aes_cmac(key, in, 0, out);
	hex("bb1d6929e95937287fa37d129b756746", want);
	CHECK("AES-CMAC of nothing matches RFC 4493", memcmp(out, want, 16) == 0);
	n = hex("6bc1bee22e409f96e93d7e117393172a", in);
	btv_aes_cmac(key, in, n, out);
	hex("070a16b46b4d4144f79bdd9dd04a287c", want);
	CHECK("AES-CMAC of one block matches RFC 4493", memcmp(out, want, 16) == 0);
	n = hex("6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51"
	        "30c81c46a35ce411", in);
	btv_aes_cmac(key, in, n, out);
	hex("dfa66747de9ae63030ca32611497c827", want);
	CHECK("AES-CMAC of 40 octets matches RFC 4493", memcmp(out, want, 16) == 0);
}

static void legacy(void)
{
	uint8_t k[16] = { 0 }, r[16], preq[7], pres[7], ia[6], ra[6], out[16];
	uint8_t r1[16], r2[16], irk[16], prand[3], hash[3];

	printf("legacy pairing toolbox (spec D.2 sample data)\n");
	lsb("5783d52156ad6f0e6388274ec6702ee0", r);
	/* The spec's PDUs are written MSB first; in PDU order they are these. */
	hex("01 01 00 00 10 07 07", preq);
	hex("02 03 00 00 08 00 05", pres);
	hex("a1a2a3a4a5a6", ia);
	hex("b1b2b3b4b5b6", ra);
	{
		uint8_t ia_l[6], ra_l[6];
		lsb("a1a2a3a4a5a6", ia_l);
		lsb("b1b2b3b4b5b6", ra_l);
		btv_smp_c1(k, r, preq, pres, 1, ia_l, 0, ra_l, out);
	}
	CHECK("c1 matches the spec", same_msb(out, "1e1e3fef878988ead2a74dc5bef13b86", 16));
	lsb("000f0e0d0c0b0a091122334455667788", r1);
	lsb("010203040506070899aabbccddeeff00", r2);
	btv_smp_s1(k, r1, r2, out);
	CHECK("s1 matches the spec", same_msb(out, "9a1fe1f0e8b0f49b5b4216ae796da062", 16));
	lsb("ec0234a357c8ad05341010a60a397d9b", irk);
	lsb("708194", prand);
	btv_smp_ah(irk, prand, hash);
	CHECK("ah matches the spec", same_msb(hash, "0dfbaa", 3));
	(void)ia; (void)ra;
}

static void secure_connections(void)
{
	uint8_t u[32], v[32], x[16], y[16], w[32], n1[16], n2[16], a1[6], a2[6];
	uint8_t r[16], mackey[16], ltk[16], out[16], iocap[3];
	uint32_t g;

	printf("LE Secure Connections toolbox (spec D.1 to D.6 sample data)\n");
	lsb("20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", u);
	lsb("55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd", v);
	lsb("d5cb8454d177733effffb2ec712baeab", x);
	btv_smp_f4(u, v, x, 0, out);
	CHECK("f4 matches the spec", same_msb(out, "f2c916f107a9bd1cf1eda1bea974872d", 16));

	lsb("ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698", w);
	lsb("d5cb8454d177733effffb2ec712baeab", n1);
	lsb("a6e8e7cc25a75f6e216583f7ff3dc4cf", n2);
	lsb("56123737bfce", a1);
	lsb("a713702dcfc1", a2);
	btv_smp_f5(w, n1, n2, 0, a1, 0, a2, mackey, ltk);
	CHECK("f5 MacKey matches the spec",
	      same_msb(mackey, "2965f176a1084a02fd3f6a20ce636e20", 16));
	CHECK("f5 LTK matches the spec",
	      same_msb(ltk, "6986791169d7cd23980522b594750a38", 16));

	lsb("12a3343bb453bb5408da42d20c2d0fc8", r);
	iocap[0] = 0x02; iocap[1] = 0x01; iocap[2] = 0x01;   /* spec: 01 01 02 */
	btv_smp_f6(mackey, n1, n2, r, iocap, 0, a1, 0, a2, out);
	CHECK("f6 matches the spec", same_msb(out, "e3c473989cd0e8c5d26c0b09da958f61", 16));

	lsb("a6e8e7cc25a75f6e216583f7ff3dc4cf", y);
	g = btv_smp_g2(u, v, x, y);
	CHECK("g2 matches the spec (2f9ed5ba)", g == 0x2f9ed5bau);
	CHECK("and its six digits are 938554", btv_smp_g2_digits(g) == 938554u);

	{
		uint8_t wk[16], id[4], h[16];
		lsb("ec0234a357c8ad05341010a60a397d9b", wk);
		id[0] = 0x72; id[1] = 0x62; id[2] = 0x65; id[3] = 0x6c;   /* "lebr" LSB-first */
		btv_smp_h6(wk, id, h);
		CHECK("h6 matches the spec", same_msb(h, "2d9ae102e76dc91ce8d3a9e280b16399", 16));
	}
}

static const struct { const char *a, *b, *pa, *pb, *dh; } ecdh_vectors[] = {
	{ "9dd8cc4ab06a9c0b743847703b68773d037d5bf809f73d272f65a14799f9d9fb",
	  "8d9a33169dff2cf78a6607b7a7e8fd36445a734ff20143c84c54c7ac02069ab5",
	  "acb6f81bdc4f1cd0ea2570c25e7921ce156702b15eb11df0ddc566a7dd44994456605c9108858711f3d15f2529988dce1690061f59afb2f50a99ede237355f27",
	  "54ce0c168f86d383271eea904ac481ac9c43234f7487592ba70c8d7054daa0f927fef765a3614b6bf4b5a4d4a6a203a096b0c5d0b177a973f228223317f47265",
	  "ed8d4fbda8fc3b14baf839b8850eb31758677d1c695fc60746f27e03cef78909" },
	{ "8d9a33169dff2cf78a6607b7a7e8fd36445a734ff20143c84c54c7ac02069ab5",
	  "9d22f537a216df68b8812c1f53351691c4f044bf422f9a771ce81c07a7b81c66",
	  "54ce0c168f86d383271eea904ac481ac9c43234f7487592ba70c8d7054daa0f927fef765a3614b6bf4b5a4d4a6a203a096b0c5d0b177a973f228223317f47265",
	  "f8cf73fc7a35de2d39447319720870aa1b9a253678a3a25c75907f0a09304ed4ddf65c68b7e6148abe5f335fa4bea7b2ce126663110916358fc3a62ea25ab7a5",
	  "12413bd43a2417f60b89b0148ac3ecc1c163f78693683157be3e8269fa915160" },
	{ "9d22f537a216df68b8812c1f53351691c4f044bf422f9a771ce81c07a7b81c66",
	  "722e565d89867c39a446bf3c78e36049f73b84ff5f1b50731c6d550ffd4f5231",
	  "f8cf73fc7a35de2d39447319720870aa1b9a253678a3a25c75907f0a09304ed4ddf65c68b7e6148abe5f335fa4bea7b2ce126663110916358fc3a62ea25ab7a5",
	  "09783b8407c70d08aaf48df66f16d91871b5edc4ca9faf95aad92dac9da2b58a75741918cbaabf5231c62bac49be23d04ea7a404422f364ff76b2899cb094e6a",
	  "44a87e79655e62acab8d4b4095c22ed11dcb46b874ebeb343c3a45ab284b618e" },
	{ "722e565d89867c39a446bf3c78e36049f73b84ff5f1b50731c6d550ffd4f5231",
	  "c390f8175cde8c8d2e9726d216efd3dc5890494a128a77f84c9771a81380d967",
	  "09783b8407c70d08aaf48df66f16d91871b5edc4ca9faf95aad92dac9da2b58a75741918cbaabf5231c62bac49be23d04ea7a404422f364ff76b2899cb094e6a",
	  "c62877310de14cbc295f397b1484307d4808682e97a8038f5e74dc8004645692c33354daa11586fdf8316b1be3b2a58d9a73acdc630370533dd7b52c6a889ddf",
	  "5f3bb8af21cb51023e051882d835b2e2cf538c10711c1b64b26c8c40a9bc9486" },
	{ "c390f8175cde8c8d2e9726d216efd3dc5890494a128a77f84c9771a81380d967",
	  "0100000000000000000000000000000000000000000000000000000000000000",
	  "c62877310de14cbc295f397b1484307d4808682e97a8038f5e74dc8004645692c33354daa11586fdf8316b1be3b2a58d9a73acdc630370533dd7b52c6a889ddf",
	  "96c298d84539a1f4a033eb2d817d0377f240a463e5e6bcf847422ce1f2d1176bf551bf376840b6cbce5e316b5733ce2b169e0f7c4aebe78e9b7f1afee242e34f",
	  "c62877310de14cbc295f397b1484307d4808682e97a8038f5e74dc8004645692" },
	{ "0100000000000000000000000000000000000000000000000000000000000000",
	  "0200000000000000000000000000000000000000000000000000000000000000",
	  "96c298d84539a1f4a033eb2d817d0377f240a463e5e6bcf847422ce1f2d1176bf551bf376840b6cbce5e316b5733ce2b169e0f7c4aebe78e9b7f1afee242e34f",
	  "78996647fc480ba6351bf277e26989c0c31ab5040338528a7e4f038d187bf27cd17378229db7049e2982e93ce6ad7dbadb30749fc69a3d2940d08edb10557707",
	  "78996647fc480ba6351bf277e26989c0c31ab5040338528a7e4f038d187bf27c" },
	{ "0200000000000000000000000000000000000000000000000000000000000000",
	  "4f2563fcc2cab9f3849e17a7adfae6bcffffffffffffffff00000000ffffffff",
	  "78996647fc480ba6351bf277e26989c0c31ab5040338528a7e4f038d187bf27cd17378229db7049e2982e93ce6ad7dbadb30749fc69a3d2940d08edb10557707",
	  "78996647fc480ba6351bf277e26989c0c31ab5040338528a7e4f038d187bf27c2e8c87dd6248fb61d67d16c31a52824524cf8b603965c2d6c02f7124eeaa88f8",
	  "5208036b44029350ef965578dbe21f03d02be69e65de2da0bb8fd032354a53e2" },
	{ "4f2563fcc2cab9f3849e17a7adfae6bcffffffffffffffff00000000ffffffff",
	  "9dd8cc4ab06a9c0b743847703b68773d037d5bf809f73d272f65a14799f9d9fb",
	  "78996647fc480ba6351bf277e26989c0c31ab5040338528a7e4f038d187bf27c2e8c87dd6248fb61d67d16c31a52824524cf8b603965c2d6c02f7124eeaa88f8",
	  "acb6f81bdc4f1cd0ea2570c25e7921ce156702b15eb11df0ddc566a7dd44994456605c9108858711f3d15f2529988dce1690061f59afb2f50a99ede237355f27",
	  "e6912c841865abf1e1134fe7a1e0c5f0e0edbd7721a67fe3b2d3773ffe351d6d" },
};

static void p256(void)
{
	uint8_t a[32], b[32], pa[64], pb[64], pub[64], dh[32], want[64];
	unsigned int i, ok = 0, ok_pub = 0, ok_dh = 0, total;

	printf("P-256 (vectors made with OpenSSL)\n");
	total = (unsigned int)(sizeof(ecdh_vectors) / sizeof(ecdh_vectors[0]));
	for (i = 0; i < total; i++) {
		(void)hex(ecdh_vectors[i].a, a);
		(void)hex(ecdh_vectors[i].b, b);
		(void)hex(ecdh_vectors[i].pa, pa);
		(void)hex(ecdh_vectors[i].pb, pb);
		(void)hex(ecdh_vectors[i].dh, want);
		ok += btv_p256_private_ok(a) && btv_p256_private_ok(b);
		ok_pub += btv_p256_public(a, pub) && memcmp(pub, pa, 64) == 0 &&
		          btv_p256_public(b, pub) && memcmp(pub, pb, 64) == 0;
		ok_dh += btv_p256_shared(a, pb, dh) && memcmp(dh, want, 32) == 0 &&
		         btv_p256_shared(b, pa, dh) && memcmp(dh, want, 32) == 0;
	}
	CHECK("every test private key is in range", ok == total);
	CHECK("public keys match OpenSSL's", ok_pub == total);
	CHECK("both sides get OpenSSL's shared secret", ok_dh == total);

	/* Spec D.1: private key A and its public key. */
	lsb("3f49f6d4a3c55f3874c9b3e3d2103f504aff607beb40b7995899b8a6cd3c1abd", a);
	CHECK("spec D.1 private key A gives its public key",
	      btv_p256_public(a, pub) &&
	      same_msb(pub, "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", 32) &&
	      same_msb(pub + 32, "dc809c49652aeb6d63329abf5a52155c766345c28fed3024741c8ed01589d28b", 32));
	CHECK("and it is a valid point", btv_p256_valid(pub));

	/* Range and curve checks. */
	memset(a, 0, 32);
	CHECK("a zero private key is refused", !btv_p256_private_ok(a) && !btv_p256_public(a, pub));
	lsb("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", a);
	CHECK("the group order as a private key is refused", !btv_p256_private_ok(a));
	memset(a, 0xff, 32);
	CHECK("all ones as a private key is refused", !btv_p256_private_ok(a));
	(void)hex(ecdh_vectors[0].pa, pa);
	(void)hex(ecdh_vectors[0].a, a);
	pa[40] ^= 1;
	CHECK("a public key off the curve is refused", !btv_p256_valid(pa));
	CHECK("and no shared secret comes from it", !btv_p256_shared(a, pa, dh));
	memset(pa, 0, 64);
	CHECK("the point (0,0) is refused", !btv_p256_valid(pa));
	memset(pa, 0xff, 64);
	CHECK("coordinates above p are refused", !btv_p256_valid(pa));
}

int main(void)
{
	aes_and_cmac();
	legacy();
	secure_connections();
	p256();
	return check_summary("btle-crypto");
}
