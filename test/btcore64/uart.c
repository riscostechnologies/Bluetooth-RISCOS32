/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 RISC OS Technologies */
/* Host checks for the UART transport's pure half (c/btuartcalc): H4
 * framing, the 16550 divisor, Broadcom's baud command and the .hcd patch
 * file reader. */
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "btuartcalc.h"

static int feed(struct h4_rx *p, const uint8_t *d, size_t n)
{
    size_t i;
    int packets = 0;
    for (i = 0; i < n; ++i) packets += h4_rx_byte(p, d[i]);
    return packets;
}

int main(void)
{
    static struct h4_rx p;
    /* Command Complete for Reset, as the Pinebook's controller sends it. */
    static const uint8_t reset_done[] = { 4, 0x0E, 4, 1, 3, 0x0C, 0 };
    static const uint8_t acl[] = { 2, 0x01, 0x20, 5, 0, 1, 2, 3, 4, 5 };
    uint8_t out[300], params[6], status = 9;
    size_t at = 0, n;
    uint16_t op;
    const uint8_t *pp;
    uint8_t pl;
    /* two .hcd records: Write RAM (0xFC4C) with 3 bytes, Launch RAM (0xFC4E) */
    static const uint8_t hcd[] = { 0x4C, 0xFC, 3, 0xAA, 0xBB, 0xCC,
                                   0x4E, 0xFC, 4, 1, 2, 3, 4 };

    h4_rx_init(&p);
    CHECK("an event is one packet", feed(&p, reset_done, sizeof reset_done) == 1);
    CHECK("its type", p.type == H4_EVENT);
    CHECK("its length, header included", p.length == 6);
    CHECK("its bytes", p.buf[0] == 0x0E && p.buf[5] == 0);
    CHECK("an ACL packet", feed(&p, acl, sizeof acl) == 1 && p.type == H4_ACL &&
          p.length == 9 && p.buf[4] == 1);
    CHECK("two in a row", feed(&p, reset_done, 3) == 0 &&
          feed(&p, reset_done + 3, 4) == 1);
    /* a bad type byte is skipped, and the next packet is still found */
    {
        static const uint8_t junk[] = { 0x77, 0x00, 4, 0x0E, 4, 1, 3, 0x0C, 0 };
        h4_rx_init(&p);
        CHECK("junk then a packet", feed(&p, junk, sizeof junk) == 1);
        CHECK("junk was counted", p.dropped == 2);
    }
    /* an event with no payload */
    {
        static const uint8_t bare[] = { 4, 0x05, 0 };
        h4_rx_init(&p);
        CHECK("an empty event", feed(&p, bare, sizeof bare) == 1 && p.length == 2);
    }
    /* an over-long ACL packet is not reported, and the stream goes on */
    {
        uint8_t big[4 + 1 + 1200] = { 2, 0x01, 0x20, 0xB0, 0x04 };
        h4_rx_init(&p);
        CHECK("oversize is dropped", feed(&p, big, sizeof big) == 0 && p.oversize == 1);
        CHECK("and the next one is found", feed(&p, reset_done, sizeof reset_done) == 1);
    }

    n = h4_command(out, sizeof out, 0x0C03, NULL, 0);
    CHECK("Reset command", n == 4 && out[0] == 1 && out[1] == 3 && out[2] == 0x0C && out[3] == 0);
    n = h4_command(out, 3, 0x0C03, NULL, 0);
    CHECK("no room", n == 0);
    CHECK("completion matches", hci_command_done(reset_done + 1, 6, 0x0C03, &status) && status == 0);
    CHECK("completion for another opcode", !hci_command_done(reset_done + 1, 6, 0x1001, &status));
    {
        static const uint8_t cs[] = { 0x0F, 4, 0x12, 1, 0x2E, 0xFC };
        CHECK("command status", hci_command_done(cs, sizeof cs, 0xFC2E, &status) && status == 0x12);
    }

    CHECK("115200 from 24 MHz", uart_divisor(24000000u, 115200u) == 13);
    CHECK("1.5 Mbaud from 24 MHz", uart_divisor(24000000u, 1500000u) == 1);
    CHECK("never zero", uart_divisor(1000u, 3000000u) == 1);
    bcm_baud_params(params, 1500000u);
    CHECK("baud command parameters", params[0] == 0 && params[1] == 0 &&
          params[2] == 0x60 && params[3] == 0xE3 && params[4] == 0x16 && params[5] == 0);

    CHECK("first record", hcd_next(hcd, sizeof hcd, &at, &op, &pp, &pl) && op == 0xFC4C &&
          pl == 3 && pp[0] == 0xAA);
    CHECK("second record", hcd_next(hcd, sizeof hcd, &at, &op, &pp, &pl) && op == 0xFC4E && pl == 4);
    CHECK("end of the file", !hcd_next(hcd, sizeof hcd, &at, &op, &pp, &pl));
    at = 0;
    CHECK("a record running past the end", !hcd_next(hcd, 5, &at, &op, &pp, &pl));
    return check_summary("uart");
}
