# Bluetooth for RISC OS (32-bit)

A Bluetooth host stack for 32-bit RISC OS 5 that works through a USB
Bluetooth adapter. Version 0.96.

- **Audio:** plays the computer's sound through Bluetooth speakers and
  headphones (A2DP, SBC).
- **Input:** works with Bluetooth mice and keyboards, both classic (HID)
  and Low Energy (HID over GATT, with LE Secure Connections pairing).
- **Reconnection:** paired devices come back by themselves at start-up and
  when they wake.

Tested on a Raspberry Pi CM4 under RISC OS 5.31 with a TP-Link UB400
(CSR8510) adapter. Realtek RTL8761B adapters, such as the TP-Link UB4A and
UB500, are sent Realtek's patch at start-up, as Linux does; the firmware is
in `desktop/firmware` and is installed in `!Bluetooth.Firmware`. Other RISC OS 5 machines with working USB 2 host drivers,
such as a Pinebook Pro, should work with a USB adapter.

The software has three parts:

| Part | What it is |
|---|---|
| `BluetoothVoice` | The relocatable module: the protocol stack, the USB transport and the sound device. |
| `!Bluetooth` | The icon bar application: device list, pairing, connecting. |
| `!BTSetup` | The Configure plug-in. |

`!Install` puts all three into `!Boot` and adds the start-up entries.

## Repository layout

| Path | Contents |
|---|---|
| `port/c`, `port/h` | Module source: HCI, L2CAP, SDP, A2DP/SBC, AVRCP, HID, LE, the USB transport, the RISC OS glue. |
| `port/cmhg` | The module header (`modhead`), and the CMunge output it is built from. |
| `client/c`, `client/h` | `!Bluetooth` and `!BTSetup`. |
| `desktop/` | Application files, sprites, the PreDesk entry and the `!Install` installer. |
| `scripts/` | The three build scripts. |
| `test/btcore64` | Host-side tests of the protocol core. |
| `docs/` | Design notes. Some describe earlier hands-free (HFP) and Windows bring-up work. |

## Building

### What you need

- **The GCCSDK cross-compiler for RISC OS:** `arm-unknown-riscos-gcc`,
  4.7 or later, with `elf2aif`. The scripts look in
  `~/gccsdk/cross/bin`. Set `RISCOS_MODULE_CC` for the module and
  `RISCOS_CC` for the applications to use another path.
- **A host C compiler and OpenSSL** (`libssl-dev`) for the tests.
- **Python 3** for the sprite generator and the zip builder.
- **`mkzip.py`**, a zip builder that writes RISC OS filetypes into the
  Acorn extra field (ID `0x4341`). Set `MKZIP` to its path. Any tool that
  writes that field works: a plain `zip` loses the filetypes, and the
  applications will not run.

### Third-party modules

The package bundles three binary modules that this repository does not
contain. Put them in `desktop/modules/` before building the desktop part:

| File | What it is |
|---|---|
| `SharedULib,ffa` | SharedUnixLibrary 1.16 or later, from GCCSDK. |
| `USBDriver,ffa` | RISC OS Open USBDriver 1.35. |
| `DWCDriver,ffa` | RISC OS Open DWCDriver with the CM4 changes, 0.40. |

`USBDriver` and `DWCDriver` are loaded only on a Raspberry Pi. On other
machines the files must still be present for the package build, but they
are never loaded.

### Steps

Run these from the top of the repository:

```sh
SKIP=usb_probe sh scripts/build-module70.sh   # the module -> build/module/BluetoothVoice.elf
sh test/btcore64/run.sh                       # host tests (needs the module's headers)
sh scripts/build-desktop.sh                   # the applications -> build/desktop/app
sh scripts/build-package.sh                   # the installer zip -> build/package/Bluetooth-<version>.zip
```

The test run should report 0 failures in every suite.

Copy the zip to a RISC OS machine, unpack it, double-click `!Install` and
click Install.

### Changing the version

The version appears in four places, which must agree:

- `port/cmhg/modhead.o`: the `BluetoothVoice<tab>0.NN` help string.
- `port/cmhg/modhead.h`.
- `client/c/btbar`: `VOICE_VERSION` and `VERSION_TEXT`.
- `client/c/btstack`: `VOICE_VERSION_NEEDED`.

`desktop/predesk/Bluetooth,feb` holds the version that the PreDesk entry
`RMEnsure`s. The installer fills this in from the module itself.

## Programmer's interface

### SWIs

SWI chunk `&D7BC0`; error base `&A79B00`. The SWIs below are called by
number. All take R0 = 0 (flags) and return their results in R1 onwards.

| SWI | Offset | Name | Purpose |
|---|---|---|---|
| `&D7BC8` | 8 | Core | Start, stop and read the stack; settings and diagnostics. |
| `&D7BC9` | 9 | Scan | Look for devices. |
| `&D7BCA` | 10 | Devices | List known and found devices. Offset 7 does the same. |
| `&D7BCB` | 11 | Connect | Pair if needed, then connect. |
| `&D7BCC` | 12 | Disconnect | Drop a device's connection. |
| `&D7BCD` | 13 | Keys | Read, load or forget pairings. |
| `&D7BCE` | 14 | Audio | Audio stream state and statistics. |
| `&D7BCF` | 15 | Adapter | Is a usable adapter plugged in? |
| `&D7BD0` | 16 | Diagnostics | Mix a test tone into the audio stream. |

Offsets 0 to 6 belong to an earlier hands-free interface and now return
"SWI not known".

### Core (`&D7BC8`)

R1 selects the reason.

| R1 | Action | Registers |
|---|---|---|
| 0 | Start the stack. | |
| 1 | Stop the stack. | |
| 2 | Read the state. | Out: see below. |
| 3 | Bring-up detail. | Out: R1 to R7, diagnostic words. |
| 4 | Packet trace. | In: R2 buffer, R3 size. Out: R3 bytes written. |
| 5 | Log saved at the last failure. | In: R2 buffer, R3 size. |
| 7 | Answer the pairing question. | In: R2 = 1 accept, 0 refuse. R3 = passkey typed, for a passkey question. |
| 8 | Who the pairing question is about. | In: R2 points to 6 bytes for the address. |
| 9 | Discoverable. | In: R2 = 1 on, 0 off. |
| 15 | Reconnect devices at start-up. | In: R2 = 1 or 0. |
| 16 | Reconnect one device automatically. | In: R2 address, R3 = 1 or 0. |
| 18 | Bluetooth on at start-up. | In: R2 = 1 or 0. |
| 22 | Save changed pairings and names now. | |
| 30 | The stack's notes, oldest first. | In: R2 buffer, R3 size. Out: R3 bytes written. |

The other reasons are used by `!Bluetooth` and `!BTSetup` for sound output,
mouse buttons and volume. See the comments in `port/c/module_core`.

Reason 2 returns:

| Register | Meaning |
|---|---|
| R1 | State: 0 off, 1 starting, 2 configuring, 3 ready, 4 stopping, 5 failed. |
| R2 | The error that stopped the stack, or 0. |
| R3 | A number that changes whenever the device list, links, pairings or profiles change. |
| R4, R5 | Passkey display: R4 changes when there is a new passkey, R5 is the passkey. |
| R6 | The last problem code, or 0. |
| R7 | Non-zero while scanning. |
| R8 | The pairing question: bits 0 to 7 kind (0 none, 1 confirm a number, 2 type a number); bits 8 to 30 a generation that changes with each question; bit 31 set when discoverable. |
| R9 | The number to confirm, for kind 1. |

### Scan (`&D7BC9`)

In: R1 = seconds to scan, 1 to 60. R1 = 0 stops a scan. The stack must be
ready.

### Devices (`&D7BCA`)

In: R2 = word-aligned buffer, R3 = its size in bytes.

The buffer is filled with a word holding the number of entries, then a word
holding the generation, then 96-byte entries. Connected devices come first,
then paired ones, then those found by the last scan.

| Offset | Size | Contents |
|---|---|---|
| 0 | 6 | Address, least significant byte first. |
| 6 | 2 | Flags: see below. |
| 8 | 1 | Signal strength (RSSI), signed. |
| 9 | 1 | Address type (LE: 0 public, 1 random). |
| 10 | 3 | Class of device. |
| 13 | 1 | Link: 0 none, 1 connecting, 2 connected. |
| 14 | 1 | Profile state. |
| 15 | 1 | Security level. |
| 16 | 64 | Name, zero-terminated. |
| 80 | 2 | LE appearance. |
| 82 | 1 | Battery level in per cent (LE devices), or 255 when unknown. |

Flags: `&001` audio sink, `&002` hands-free, `&004` HID, `&008` keyboard,
`&010` mouse, `&020` Low Energy, `&080` paired, `&100` connected.

### Connect (`&D7BCB`)

In: R1 = address (6 bytes); R2 = profile, 1 A2DP, 2 HID, 3 LE HID; R3 bit 0
= Low Energy, bits 8 to 15 = address type. Pairs first if the device is not
paired; any question is reported through Core reason 2.

### Disconnect (`&D7BCC`)

In: R1 = address. The device is not reconnected automatically afterwards.

### Keys (`&D7BCD`)

| R1 | Action |
|---|---|
| 0 | Read: R2 buffer, R3 size. Out: R3 bytes used, R4 entry size. |
| 1 | Load: R2 buffer, R3 bytes, in whole entries. |
| 2 | Forget: R2 = address. Also disconnects the device. |

The module keeps the pairings in `Choices:Bluetooth.Keys` itself;
applications do not need to load or save them.

### Audio (`&D7BCE`)

| Register | Out |
|---|---|
| R1 | A2DP state. |
| R2 | Audio packets sent. |
| R3 | Underruns. |
| R4 | Sound device state. |
| R5 | Late packets. |
| R6 | Sound level meter. |
| R7 | ACL packets sent. |
| R8, R9 | Transport counters: see `port/c/module_core`. |

### Adapter (`&D7BCF`)

Out: R1 = 1 when an adapter is plugged in, otherwise 0. R2 = 1 when the
loaded USB stack suits it. R3 = vendor ID, with the product ID in the upper
16 bits.

### Diagnostics (`&D7BD0`)

In: R1 = tone length in milliseconds, up to 5000. Out: R1 = 1 when the tone
started.

### Files

| File | Contents |
|---|---|
| `Choices:Bluetooth.Keys` | Pairings, written by the module. |
| `Choices:Bluetooth.Names` | Device names, one line each: address, space, name. |
| `Choices:Bluetooth.NoAuto` | Devices not to reconnect automatically, one address per line. |
| `Choices:Bluetooth.Config` | Module settings (`Power=`, `Reconnect=`). When missing, Bluetooth is on and reconnects. |
| `Choices:Bluetooth.Settings` | `!BTSetup`'s settings. |

## Licence

BSD 2-Clause: see `LICENSE`. A few files derive from NetBSD (BSD licence) or
from RISC OS USBDriver (Apache License 2.0) and keep those licences: see
`THIRD-PARTY-NOTICES.md`.
