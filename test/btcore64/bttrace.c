/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (C) RISC OS Technologies 2026 */
/* Host checks for the queued trace and poll timing (c/bttrace.c). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bttrace.h"
#include "check.h"
#define C(x) CHECK(#x, (x))

static char sent[BTTR_RING + 64];
static uint32_t nsent;
static void tx(uint8_t b) { sent[nsent++] = (char)b; }

int main(void)
{
	char line[100], big[300];
	struct bttr_slow s;
	uint32_t n;

	bttr_reset();
	C(bttr_put("[bt] hello"));
	C(bttr_pending() == 12);
	/* A drain never sends more than its budget: the callback's bound. */
	n = bttr_drain(5, tx);
	C(n == 5 && nsent == 5 && bttr_pending() == 7);
	n = bttr_drain(BTTR_DRAIN_BYTES, tx);
	C(n == 7 && memcmp(sent, "[bt] hello\r\n", 12) == 0);
	C(bttr_drain(32, tx) == 0);

	/* A burst bigger than the ring drops lines, never blocks, never
	 * overwrites what is queued. */
	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = 0;
	nsent = 0;
	for (n = 0; n < 40; n++)
		(void)bttr_put(big);
	C(bttr_dropped() > 0);
	C(bttr_pending() <= BTTR_RING);
	n = 0;
	while (bttr_pending() != 0) {
		uint32_t got = bttr_drain(BTTR_DRAIN_BYTES, tx);
		C(got <= BTTR_DRAIN_BYTES);
		n++;
	}
	C(n > 100);	/* the heartbeat's burst is spread over many ticks */
	/* Wrap-around: the ring still works after a full lap. */
	nsent = 0;
	C(bttr_put("again"));
	C(bttr_drain(64, tx) == 7 && memcmp(sent, "again\r\n", 7) == 0);

	/* The clock: half way round a centisecond is 5000 us. */
	C(bttr_micros(10, 500, 1000) == 100000 + 5000);
	C(bttr_micros(10, 1000, 1000) == 100000);
	C(bttr_micros(10, 0, 0) == 100000);

	/* Timing: a fast poll is counted, not kept. */
	bttr_begin(1000);
	bttr_stage(BTTR_POLL_TRANSPORT, 1100);
	C(bttr_end(0, 1500, 7) == 0);
	C(bttr_slow_count() == 0 && bttr_polls() == 1);
	/* A slow one names the stage that took the time. */
	bttr_begin(10000);
	bttr_stage(BTTR_TRANSPORT_BOOT, 10100);
	bttr_stage(BTTR_POLL_TRANSPORT, 13100);
	C(bttr_end(0, 13300, 42) == 1);
	C(bttr_slow_get(0, &s) && s.total_us == 3300 &&
	      s.worst_stage == BTTR_TRANSPORT_BOOT && s.worst_us == 3000);
	bttr_format(&s, line, sizeof line);
	C(strstr(line, "controller start-up 3000us") != NULL);
	C(bttr_worst_us() == 3300);
	C(!bttr_slow_get(1, &s));
	C(bttr_task(1, 500, 9) == 0);
	C(bttr_task(1, 4100, 10) == 1);
	C(bttr_slow_get(1, &s) && s.who == 1 && s.total_us == 4100);
	bttr_format(&s, line, sizeof line);
	C(strstr(line, "task null 4100us") != NULL && strstr(line, "housekeeping") != NULL);
	return check_summary("bttrace");
}
