# Third-party code

Everything in this repository is under the BSD 2-Clause licence in
`LICENSE`, except the files below, which keep the licence and copyright
notice of the code they derive from. Each file carries its notice in full.

## NetBSD Bluetooth stack (BSD licences)

Derived from NetBSD's `sys/netbt` (hci_unit.c, hci_event.c, hci_link.c,
l2cap_*.c, rfcomm_*.c, sdp). Copyright (c) 2005 Iain Hibbert, (c) 2006
Itronix Inc., (c) 2009 The NetBSD Foundation, Inc. See also
`docs/NETBSD-NOTICE.md`.

    port/c/acl_link        port/c/l2cap_dispatch  port/c/rfcomm_msc
    port/c/acl_receive     port/c/rfcomm_control  port/c/rfcomm_mux
    port/c/acl_transmit    port/c/rfcomm_credit   port/c/rfcomm_pn
    port/c/hci_bootstrap   port/c/rfcomm_frame    port/c/sdp_data
    port/c/hci_commands    port/c/rfcomm_mcc      port/c/sdp_query
    port/c/l2cap_channel                          port/c/sdp_record

## RISC OS USBDriver HID tables (Apache License 2.0)

`port/c/hid_input` ports the key table and event order of RISC OS 5's
USBDriver `build/c/usbkboard` and `build/c/usbmouse`, Copyright 2003
Tematic Ltd, under the Apache License, Version 2.0
(http://www.apache.org/licenses/LICENSE-2.0). That file is distributed
under the Apache License 2.0.

## Raspberry Pi built-in Bluetooth transport (MIT)

`port/c/ro5_bridge` and `port/h/ro5_bridge` are adapted from the Phoenix
RISC OS project's BluetoothVoice-ro5 contribution, Copyright (c) 2026
Phoenix RISC OS project, under the MIT licence. Their SPDX and copyright
notices are retained in those files.

    MIT License

    Copyright (c) 2026 Phoenix RISC OS project

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Realtek Bluetooth firmware (Realtek redistribution licence)

`desktop/firmware/rtl8761bu_fw,ffd` and `rtl8761bu_config,ffd` are
Realtek's RTL8761B patch files, unmodified, from the linux-firmware
repository (`rtl_bt/`). Copyright (c) 2010 Realtek Semiconductor
Corporation; redistributable in binary form under the terms in
`desktop/firmware/Licence`, which ship with them. They are not covered by
`LICENSE`.

## Modules not in this repository

The desktop package bundles three binary modules that are not part of this
source tree and are not covered by `LICENSE`. Put them in `desktop/modules/`
before building the package (see README):

- `SharedULib,ffa` - SharedUnixLibrary 1.16, from GCCSDK / UnixLib.
- `USBDriver,ffa` - RISC OS Open USBDriver 1.35 (Raspberry Pi only).
- `DWCDriver,ffa` - RISC OS Open DWCDriver, 0.40 with the CM4 changes
  (Raspberry Pi only).
