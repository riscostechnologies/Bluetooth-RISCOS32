#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 RISC OS Technologies
# The 64-bit core's host suites, built against bv70's sources (port/c), with
# the 10-line check.c harness.  Run: sh test/btcore64/run.sh
set -eu
cd "$(dirname "$0")"; T=$PWD; B=$(cd ../.. && pwd); O=${TMPDIR:-/tmp}/bv70-tests; mkdir -p $O
[ -d $B/build/include ] || { echo "build the module first (scripts/build-module70.sh)"; exit 1; }
src(){ for f in "$@"; do printf -- "-x c $B/port/c/%s " $f; done; }
CF="-funsigned-char -g -w -I$B/build/include -I$T"
run(){ name=$1; shift; t=$1; shift; gcc $CF $t $(src "$@") -x c $T/check.c -o $O/$name && $O/$name | grep -E "FAIL|checks|passed"; }
run lesc "-x c $T/lesc.c" bt_core bt_reconnect bt_l2cap bt_le le_crypto p256 hid_input hid_report hid_extras hid_pad hci_bootstrap hci_stream hci_frame hci_version
run a2dp "-x c $T/a2dp.c" bt_core bt_reconnect bt_l2cap bt_a2dp avrcp bt_profiles bt_sdp_server hid_input bt_hid bt_sdpc hid_sdp hid_report hid_extras hid_pad sdp_data sbc_encoder sound_ring hci_bootstrap hci_stream hci_frame hci_version bt_info
run checks "-x c $T/checks.c" bt_core bt_reconnect hci_bootstrap hci_stream hci_frame hci_version
run le "-x c $T/le.c" le_crypto p256
run hidmap "-x c $T/hidmap.c" hid_report hid_input
run hid "-x c $T/hid.c" hid_input
run pure "$(for f in $T/pure.c $T/test_*.c; do printf -- '-x c %s ' $f; done)" hci_inquiry hci_pairing hci_security hci_frame l2cap_signal l2cap_channel l2cap_dispatch acl_transmit acl_receive acl_link sdp_query sdp_record sdp_data sdp_session
run sbc "-x c $T/sbc.c" sbc_encoder
run uart "-x c $T/uart.c" btuartcalc
